// See congestion_scoring.h for the C5 aggregation-layer definitions.

#include "hg_metrics/congestion_scoring.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <numeric>
#include <string>
#include <vector>

#include "support/logging.h"
#include "utl/Logger.h"

namespace hgm {

namespace {

// Debug group shared with the rest of hg_metrics — one group per component, so
// -verbosity lifts the whole run (see src/support/logging.h).
constexpr const char* kGroup = "hg_metrics";

// Message-id block for hg_metrics warn()/info() lines lives in the shared
// utl::UKN namespace, hg_metrics claiming 130-149 (registered in
// src/support/logging.h). 130 = C3 net_intersection, 131 = C4 tangle clamp,
// 132 = C5 all-zero required plane. debugPrint takes no id.
constexpr int kMsgZeroPlane = 132;

// Input planes read from C2/C3/C4 and the single output plane C5 writes.
constexpr const char* kPlaneKCore = "hgm.k_core";                  // int (C2)
constexpr const char* kPlaneDensity = "hgm.neighborhood_density";  // double (C3)
constexpr const char* kPlaneTangle = "hgm.tangle_score";           // double (C4)
constexpr const char* kPlaneScore = "hgm.congestion_score";        // int (C5)

// Percentile rank of every scope vertex within the scope, in [0,1].
//
// `all_values` is a per-vertex value array (size numVertices()); `indices` is
// the active scope. Returns a vector sized like `all_values` with a rank set
// only at scope positions (others left 0.0, unused). rank(v) = (#scope values
// strictly less than value(v)) / (N-1), so the scope minimum maps to 0.0 and
// the maximum to 1.0. When every scope value is identical (which includes a
// single-vertex scope) every rank is 0.5 — the midpoint that bins to score 3 —
// avoiding a divide-by-zero and giving a well-defined "no spread" answer.
std::vector<double> percentile_ranks(const std::vector<double>& all_values,
                                     const std::vector<int>& indices)
{
  std::vector<double> ranks(all_values.size(), 0.0);
  const int n = static_cast<int>(indices.size());
  if (n == 0) {
    return ranks;
  }

  std::vector<double> sorted;
  sorted.reserve(n);
  for (const int idx : indices) {
    sorted.push_back(all_values[idx]);
  }
  std::sort(sorted.begin(), sorted.end());

  if (sorted.front() == sorted.back()) {
    for (const int idx : indices) {
      ranks[idx] = 0.5;
    }
    return ranks;
  }

  for (const int idx : indices) {
    const auto it =
        std::lower_bound(sorted.begin(), sorted.end(), all_values[idx]);
    const int pos = static_cast<int>(it - sorted.begin());
    ranks[idx] = static_cast<double>(pos) / static_cast<double>(n - 1);
  }
  return ranks;
}

// Quintile bin: [0,0.2)->1, [0.2,0.4)->2, [0.4,0.6)->3, [0.6,0.8)->4, else 5.
int to_score(const double composite)
{
  if (composite < 0.2) {
    return 1;
  }
  if (composite < 0.4) {
    return 2;
  }
  if (composite < 0.6) {
    return 3;
  }
  if (composite < 0.8) {
    return 4;
  }
  return 5;
}

// Nearest-rank percentile of an already-sorted array: value at index
// floor(p * (n-1)), clamped into range. Mirrors congestion_metrics.cpp.
double percentile_of_sorted(const std::vector<double>& sorted, const double p)
{
  if (sorted.empty()) {
    return 0.0;
  }
  const int n = static_cast<int>(sorted.size());
  int idx = static_cast<int>(p * (n - 1));
  if (idx < 0) {
    idx = 0;
  }
  if (idx >= n) {
    idx = n - 1;
  }
  return sorted[idx];
}

}  // namespace

eda::Status score_congestion(eda::Hypergraph& hg, CongestionReport& report_out,
                             const CongestionWeights& weights,
                             const std::vector<int>& scope, utl::Logger* logger)
{
  report_out = CongestionReport{};

  // 1. Weight validation (before touching any plane, so a bad call writes
  //    nothing). eda::ErrorCode has no kInvalidArgument; InvalidConfig is the
  //    closest existing category ("input parsed but violates a required rule").
  const double weight_sum = weights.k_core_weight + weights.neighborhood_weight
                            + weights.tangle_weight;
  if (std::abs(weight_sum - 1.0) > 1e-6) {
    return eda::makeError(
        eda::ErrorCode::InvalidConfig,
        "CongestionWeights must sum to 1.0, got " + std::to_string(weight_sum));
  }

  // 2. Required input planes must exist; missing any is a clean error.
  for (const char* name : {kPlaneKCore, kPlaneDensity, kPlaneTangle}) {
    if (!hg.hasVertexPlane(name)) {
      return eda::makeError(
          eda::ErrorCode::InvalidConfig,
          std::string("required plane '") + name
              + "' is missing (run C2/C3/C4 before scoring)");
    }
  }

  // 3. Resolve the active scope: empty -> all vertices; else validate range.
  const int n = hg.numVertices();
  std::vector<int> active;
  if (scope.empty()) {
    active.resize(n);
    std::iota(active.begin(), active.end(), 0);
  } else {
    active.reserve(scope.size());
    for (const int v : scope) {
      if (v < 0 || v >= n) {
        return eda::makeError(
            eda::ErrorCode::InvalidConfig,
            "scope index out of range: " + std::to_string(v));
      }
      active.push_back(v);
    }
  }

  // Read the (existing) input planes. Fetching them cannot insert into the
  // plane map, so the references stay valid across the later kPlaneScore
  // creation (unordered_map insert never invalidates references to other
  // elements).
  const std::vector<int>& kcore = hg.vertexIntPlane(kPlaneKCore);
  const std::vector<double>& density = hg.vertexDoublePlane(kPlaneDensity);
  const std::vector<double>& tangle = hg.vertexDoublePlane(kPlaneTangle);

  // k_core is int; lift to double for uniform percentile ranking.
  std::vector<double> kcore_d(n);
  for (int v = 0; v < n; ++v) {
    kcore_d[v] = static_cast<double>(kcore[v]);
  }

  // Flag any required plane that is entirely zero over the scope (a strong
  // "C2/C3/C4 not run on this scope" signal) before we compute ranks.
  auto all_zero_int = [&](const std::vector<int>& p) {
    for (const int v : active) {
      if (p[v] != 0) {
        return false;
      }
    }
    return !active.empty();
  };
  auto all_zero_dbl = [&](const std::vector<double>& p) {
    for (const int v : active) {
      if (p[v] != 0.0) {
        return false;
      }
    }
    return !active.empty();
  };
  const bool kcore_zero = all_zero_int(kcore);
  const bool density_zero = all_zero_dbl(density);
  const bool tangle_zero = all_zero_dbl(tangle);

  const std::vector<double> r_kcore = percentile_ranks(kcore_d, active);
  const std::vector<double> r_density = percentile_ranks(density, active);
  const std::vector<double> r_tangle = percentile_ranks(tangle, active);

  // Output plane: created zero-initialized if absent, reused in place if
  // present. No blanket fill — out-of-scope positions keep their prior value.
  std::vector<int>& out = hg.vertexIntPlane(kPlaneScore);

  std::vector<double> composites;
  composites.reserve(active.size());
  for (const int v : active) {
    const double composite = weights.k_core_weight * r_kcore[v]
                             + weights.neighborhood_weight * r_density[v]
                             + weights.tangle_weight * r_tangle[v];
    const int score = to_score(composite);
    out[v] = score;
    ++report_out.score_histogram[score];
    composites.push_back(composite);
  }

  report_out.num_vertices_scored = static_cast<int>(active.size());
  if (!composites.empty()) {
    const double sum =
        std::accumulate(composites.begin(), composites.end(), 0.0);
    report_out.mean_composite = sum / static_cast<double>(composites.size());
    std::vector<double> sorted = composites;
    std::sort(sorted.begin(), sorted.end());
    report_out.p90_composite = percentile_of_sorted(sorted, 0.90);
  }

  // Subclusters over everything scored (min_score = 1), same scope. This also
  // emits the verbosity-2/3 cluster logging when a logger is attached.
  report_out.clusters = find_congestion_clusters(hg, /*min_score=*/1, scope,
                                                  logger);

  if (logger != nullptr) {
    if (kcore_zero) {
      logger->warn(utl::UKN, kMsgZeroPlane,
                   "score_congestion: '{}' is all-zero over the {} scored "
                   "vertices — did you run C2 (k_core_numbers)?",
                   kPlaneKCore, report_out.num_vertices_scored);
    }
    if (density_zero) {
      logger->warn(utl::UKN, kMsgZeroPlane,
                   "score_congestion: '{}' is all-zero over the {} scored "
                   "vertices — did you run C3 (neighborhood_density)?",
                   kPlaneDensity, report_out.num_vertices_scored);
    }
    if (tangle_zero) {
      logger->warn(utl::UKN, kMsgZeroPlane,
                   "score_congestion: '{}' is all-zero over the {} scored "
                   "vertices — did you run C4 (tangle_score)?",
                   kPlaneTangle, report_out.num_vertices_scored);
    }
    debugPrint(logger, utl::UKN, kGroup, eda::kVerbosityDetail,
               "score_congestion: scored {} vertices; bands 1..5 = "
               "{}/{}/{}/{}/{}; mean_composite={:.3f} p90={:.3f}",
               report_out.num_vertices_scored, report_out.score_histogram[1],
               report_out.score_histogram[2], report_out.score_histogram[3],
               report_out.score_histogram[4], report_out.score_histogram[5],
               report_out.mean_composite, report_out.p90_composite);
  }

  return eda::okStatus();
}

std::vector<CongestionCluster> find_congestion_clusters(
    eda::Hypergraph& hg, const int min_score, const std::vector<int>& scope,
    utl::Logger* logger)
{
  std::vector<CongestionCluster> clusters;
  const int n = hg.numVertices();
  if (n == 0 || !hg.hasVertexPlane(kPlaneScore)) {
    return clusters;
  }
  const std::vector<int>& score = hg.vertexIntPlane(kPlaneScore);

  // Eligible = in scope (when scope is non-empty) AND score >= min_score.
  const bool scoped = !scope.empty();
  std::vector<char> in_scope;
  if (scoped) {
    in_scope.assign(n, 0);
    for (const int v : scope) {
      if (v >= 0 && v < n) {
        in_scope[v] = 1;
      }
    }
  }
  std::vector<char> eligible(n, 0);
  for (int v = 0; v < n; ++v) {
    if (scoped && in_scope[v] == 0) {
      continue;
    }
    if (score[v] >= min_score) {
      eligible[v] = 1;
    }
  }

  const std::vector<int>& v_off = hg.vertexOffsets();
  const std::vector<int>& v_pins = hg.vertexPinList();  // v -> incident edges
  const std::vector<int>& e_off = hg.hyperedgeOffsets();
  const std::vector<int>& e_pins = hg.pinList();         // e -> member vertices

  // BFS connected components over the eligible set. Scanning seeds in ascending
  // index order makes both the component partition and each cluster's discovery
  // order deterministic. `visited` is stamped on enqueue so no vertex is queued
  // twice. Vertex-major CSR enumerates a vertex's incident hyperedges;
  // hyperedge-major CSR enumerates each hyperedge's members.
  std::vector<char> visited(n, 0);
  int cluster_id = 0;
  std::deque<int> queue;
  for (int seed = 0; seed < n; ++seed) {
    if (eligible[seed] == 0 || visited[seed] != 0) {
      continue;
    }
    queue.clear();
    queue.push_back(seed);
    visited[seed] = 1;

    std::vector<int> members;
    while (!queue.empty()) {
      const int u = queue.front();
      queue.pop_front();
      members.push_back(u);
      for (int i = v_off[u]; i < v_off[u + 1]; ++i) {
        const int e = v_pins[i];
        for (int j = e_off[e]; j < e_off[e + 1]; ++j) {
          const int w = e_pins[j];
          if (eligible[w] != 0 && visited[w] == 0) {
            visited[w] = 1;
            queue.push_back(w);
          }
        }
      }
    }

    CongestionCluster cluster;
    cluster.cluster_id = cluster_id++;
    cluster.size = static_cast<int>(members.size());
    long long score_sum = 0;
    for (const int v : members) {
      cluster.peak_score = std::max(cluster.peak_score, score[v]);
      score_sum += score[v];
    }
    cluster.mean_score =
        static_cast<double>(score_sum) / static_cast<double>(cluster.size);
    cluster.members = std::move(members);
    clusters.push_back(std::move(cluster));
  }

  // Return order: peak_score desc, then size desc (cluster_id keeps the
  // discovery-order identity assigned above).
  std::sort(clusters.begin(), clusters.end(),
            [](const CongestionCluster& a, const CongestionCluster& b) {
              if (a.peak_score != b.peak_score) {
                return a.peak_score > b.peak_score;
              }
              return a.size > b.size;
            });

  if (logger != nullptr) {
    int largest = 0;
    for (const CongestionCluster& c : clusters) {
      largest = std::max(largest, c.size);
    }
    debugPrint(logger, utl::UKN, kGroup, eda::kVerbosityHeartbeat,
               "find_congestion_clusters: {} clusters (min_score={}), largest "
               "size {}",
               clusters.size(), min_score, largest);

    int traced = 0;
    for (const CongestionCluster& c : clusters) {
      if (traced >= eda::kTraceCap) {
        debugPrint(logger, utl::UKN, kGroup, eda::kVerbosityTrace,
                   "find_congestion_clusters: per-cluster trace capped at {}",
                   eda::kTraceCap);
        break;
      }
      std::string members;
      for (std::size_t k = 0; k < c.members.size(); ++k) {
        if (k != 0) {
          members += ",";
        }
        members += std::to_string(c.members[k]);
      }
      debugPrint(logger, utl::UKN, kGroup, eda::kVerbosityTrace,
                 "cluster {} peak={} size={} members=[{}]", c.cluster_id,
                 c.peak_score, c.size, members);
      ++traced;
    }
  }

  return clusters;
}

}  // namespace hgm
