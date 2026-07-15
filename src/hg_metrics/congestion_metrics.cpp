// See congestion_metrics.h for the metric definitions.

#include "hg_metrics/congestion_metrics.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <list>
#include <unordered_set>
#include <utility>
#include <vector>

#include "support/logging.h"
#include "utl/Logger.h"

namespace hgm {

namespace {

// Debug group for this module's verbosity-3 histogram summaries. Library
// entry point, no CLI flag of its own — silent at verbosity 0/nullptr, per
// the repo's debugPrint convention (see src/support/logging.h).
constexpr const char* kGroup = "hg_metrics";

// Message-id block for hg_metrics warn()/info() lines. The repo partitions
// ids across the shared utl::UKN namespace (see src/support/logging.h:
// hypergraph 100-119, fm 120-129, hello_odb 200-209, netlistgen 300-349);
// hg_metrics claims 130-149. debugPrint takes no id, so only the C3/C4
// warnings below need them so far.
constexpr int kMsgHighDegree = 130;    // C3 net_intersection_score
constexpr int kMsgTangleClamp = 131;   // C4 tangle_score clamp rate

// Above this incident-hyperedge count, net_intersection_score's per-vertex
// pair enumeration (quadratic in degree) may be slow; warn once per such
// vertex when a logger is attached.
constexpr int kHighDegreeWarnThreshold = 64;

// Per-vertex incident-hyperedge count read straight off the vertex-major CSR
// offsets — the C1 "degree" convention, reused as the NESS label weight.
std::vector<int> vertexDegrees(const eda::Hypergraph& hg)
{
  const std::vector<int>& v_off = hg.vertexOffsets();
  std::vector<int> degree(hg.numVertices(), 0);
  for (int v = 0; v < hg.numVertices(); ++v) {
    degree[v] = v_off[v + 1] - v_off[v];
  }
  return degree;
}

// NESS information-propagation BFS. For each source vertex u, walks the
// hypergraph out to h hops and accumulates
//   A(u) = sum_{depth>=1} alpha^depth * sum_{v at that depth} degree(v)
// into out[u]. Two vertices are adjacent iff they share a hyperedge, so the
// BFS expands v -> every member of every hyperedge incident to v. `stamp[w]`
// carries the source index that last visited w, giving O(1) "already visited
// at a shorter distance" tests with no per-source O(n) clear of the marker
// array. Not exposed in the header — a static helper per the C3 brief.
void propagate_neighborhood(const eda::Hypergraph& hg,
                            const double alpha,
                            const int h,
                            const std::vector<int>& degree,
                            std::vector<double>& out)
{
  const int n = hg.numVertices();
  const std::vector<int>& v_off = hg.vertexOffsets();
  const std::vector<int>& v_pins = hg.vertexPinList();  // v -> incident edges
  const std::vector<int>& e_off = hg.hyperedgeOffsets();
  const std::vector<int>& e_pins = hg.pinList();         // e -> member vertices

  // alpha_pow[i] = alpha^i, i in [0, h]. alpha == 0 makes every i >= 1 term
  // zero, so A(u) == 0 for all u (and h == 0 never enters the depth loop).
  std::vector<double> alpha_pow(h + 1, 1.0);
  for (int i = 1; i <= h; ++i) {
    alpha_pow[i] = alpha_pow[i - 1] * alpha;
  }

  std::vector<int> stamp(n, -1);  // source that last visited a vertex
  std::vector<int> dist(n, 0);    // BFS depth of each visited vertex
  std::deque<int> queue;

  for (int u = 0; u < n; ++u) {
    double acc = 0.0;
    stamp[u] = u;
    dist[u] = 0;
    queue.clear();
    queue.push_back(u);

    while (!queue.empty()) {
      const int v = queue.front();
      queue.pop_front();
      const int dv = dist[v];
      if (dv >= h) {
        continue;
      }
      for (int i = v_off[v]; i < v_off[v + 1]; ++i) {
        const int e = v_pins[i];
        for (int j = e_off[e]; j < e_off[e + 1]; ++j) {
          const int w = e_pins[j];
          if (stamp[w] == u) {
            continue;  // source u itself, or already visited at <= this depth
          }
          stamp[w] = u;
          dist[w] = dv + 1;
          acc += alpha_pow[dv + 1] * degree[w];
          queue.push_back(w);
        }
      }
    }
    out[u] = acc;
  }
}

// index -> incident-hyperedge / member count, read straight off a CSR
// offsets array: values[i] = offsets[i + 1] - offsets[i].
std::vector<int> csrSliceSizes(const std::vector<int>& offsets)
{
  std::vector<int> sizes;
  if (offsets.empty()) {
    return sizes;
  }
  sizes.reserve(offsets.size() - 1);
  for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
    sizes.push_back(offsets[i + 1] - offsets[i]);
  }
  return sizes;
}

std::map<int, int> histogramOf(const std::vector<int>& values)
{
  std::map<int, int> histogram;
  for (const int v : values) {
    ++histogram[v];
  }
  return histogram;
}

// Nearest-rank percentile position in a 0-indexed sorted array of size n:
// index = floor(p * (n - 1)), clamped into [0, n - 1].
int percentileIndex(const int n, const double p)
{
  int idx = static_cast<int>(p * (n - 1));
  if (idx < 0) {
    idx = 0;
  }
  if (idx >= n) {
    idx = n - 1;
  }
  return idx;
}

DistributionStats computeStats(std::vector<int> values,
                               utl::Logger* logger,
                               const char* what)
{
  DistributionStats stats;
  if (values.empty()) {
    return stats;
  }

  double sum = 0.0;
  int max_value = values.front();
  for (const int v : values) {
    sum += v;
    max_value = std::max(max_value, v);
  }
  stats.mean = sum / static_cast<double>(values.size());
  stats.max = max_value;

  const int n = static_cast<int>(values.size());
  const int p90_idx = percentileIndex(n, 0.90);
  std::nth_element(values.begin(), values.begin() + p90_idx, values.end());
  stats.p90 = values[p90_idx];

  const int p99_idx = percentileIndex(n, 0.99);
  std::nth_element(values.begin(), values.begin() + p99_idx, values.end());
  stats.p99 = values[p99_idx];

  if (logger != nullptr) {
    debugPrint(logger, utl::UKN, kGroup, eda::kVerbosityTrace,
              "{}: mean={:.3f} p99={:.3f} max={}", what, stats.mean,
              stats.p99, stats.max);
  }
  return stats;
}

}  // namespace

std::map<int, int> vertex_degree_histogram(const eda::Hypergraph& hg,
                                           utl::Logger* /*logger*/)
{
  return histogramOf(csrSliceSizes(hg.vertexOffsets()));
}

DistributionStats vertex_degree_stats(const eda::Hypergraph& hg,
                                      utl::Logger* logger)
{
  return computeStats(csrSliceSizes(hg.vertexOffsets()), logger,
                      "vertex_degree_stats");
}

std::map<int, int> hyperedge_size_histogram(const eda::Hypergraph& hg,
                                            utl::Logger* /*logger*/)
{
  return histogramOf(csrSliceSizes(hg.hyperedgeOffsets()));
}

DistributionStats hyperedge_size_stats(const eda::Hypergraph& hg,
                                       utl::Logger* logger)
{
  return computeStats(csrSliceSizes(hg.hyperedgeOffsets()), logger,
                      "hyperedge_size_stats");
}

std::vector<HyperedgeId> high_fanout_nets(const eda::Hypergraph& hg,
                                          const int threshold)
{
  std::vector<HyperedgeId> result;
  const std::vector<int>& offsets = hg.hyperedgeOffsets();
  for (int e = 0; e < hg.numHyperedges(); ++e) {
    const int size = offsets[e + 1] - offsets[e];
    if (size >= threshold) {
      result.push_back(e);
    }
  }
  return result;
}

int k_core_numbers(eda::Hypergraph& hg, utl::Logger* logger)
{
  const int n = hg.numVertices();
  const int m = hg.numHyperedges();

  // Create/overwrite the output plane. vertexIntPlane sizes it to n and
  // zero-initializes on first creation; a fill covers the overwrite case so a
  // re-run on the same build starts clean.
  std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  std::fill(k_core.begin(), k_core.end(), 0);

  if (n == 0) {
    return 0;
  }

  const std::vector<int>& v_off = hg.vertexOffsets();
  const std::vector<int>& v_pins = hg.vertexPinList();
  const std::vector<int>& e_off = hg.hyperedgeOffsets();
  const std::vector<int>& e_pins = hg.pinList();

  // A hyperedge's active member count starts at its full pin count; it stops
  // contributing degree once only one active member survives.
  std::vector<int> active_count(m);
  for (int e = 0; e < m; ++e) {
    active_count[e] = e_off[e + 1] - e_off[e];
  }

  // Effective degree: incident hyperedges that still have >= 2 members (a
  // single-pin net never contributes). Also find the max for bucket sizing.
  std::vector<int> cur_deg(n, 0);
  int max_deg = 0;
  for (int v = 0; v < n; ++v) {
    int deg = 0;
    for (int i = v_off[v]; i < v_off[v + 1]; ++i) {
      if (active_count[v_pins[i]] >= 2) {
        ++deg;
      }
    }
    cur_deg[v] = deg;
    max_deg = std::max(max_deg, deg);
  }

  // Bucket queue: buckets[d] holds every currently-active vertex of effective
  // degree d. node[v] is v's iterator into its bucket, for O(1) removal on a
  // degree update.
  std::vector<std::list<int>> buckets(max_deg + 1);
  std::vector<std::list<int>::iterator> node(n);
  for (int v = 0; v < n; ++v) {
    buckets[cur_deg[v]].push_front(v);
    node[v] = buckets[cur_deg[v]].begin();
  }

  std::vector<char> removed(n, 0);

  // Peel vertices in non-decreasing degree order. Scanning level d upward is
  // the standard degeneracy ordering: everything with core number < d is gone
  // before we reach d, so a vertex peeled while bucket d is draining takes
  // core number d even if its remaining degree dipped below d (clamp below).
  int degeneracy = 0;
  for (int d = 0; d <= max_deg; ++d) {
    while (!buckets[d].empty()) {
      const int v = buckets[d].front();
      buckets[d].pop_front();
      removed[v] = 1;
      k_core[v] = d;
      degeneracy = std::max(degeneracy, d);

      for (int i = v_off[v]; i < v_off[v + 1]; ++i) {
        const int e = v_pins[i];
        if (active_count[e] < 2) {
          continue;  // already degenerate — v took no degree from it
        }
        --active_count[e];
        if (active_count[e] != 1) {
          continue;  // still >= 2 survivors: no vertex loses degree yet
        }
        // The edge just went degenerate: its lone survivor loses one degree.
        int u = -1;
        for (int j = e_off[e]; j < e_off[e + 1]; ++j) {
          if (!removed[e_pins[j]]) {
            u = e_pins[j];
            break;
          }
        }
        if (u < 0) {
          continue;
        }
        const int old_deg = cur_deg[u];
        int new_deg = old_deg - 1;
        if (new_deg < d) {
          new_deg = d;  // can't peel below the current level
        }
        if (new_deg != old_deg) {
          buckets[old_deg].erase(node[u]);
          buckets[new_deg].push_front(u);
          node[u] = buckets[new_deg].begin();
          cur_deg[u] = new_deg;
        }
      }
    }
  }

  if (logger != nullptr) {
    double sum = 0.0;
    for (const int c : k_core) {
      sum += c;
    }
    debugPrint(logger, utl::UKN, kGroup, eda::kVerbosityHeartbeat,
               "k_core_numbers: degeneracy={} mean={:.3f} max={}", degeneracy,
               sum / static_cast<double>(n), degeneracy);
  }

  return degeneracy;
}

void neighborhood_density(eda::Hypergraph& hg, const double alpha, const int h)
{
  std::vector<double>& density = hg.vertexDoublePlane("hgm.neighborhood_density");
  std::fill(density.begin(), density.end(), 0.0);

  if (hg.numVertices() == 0 || h <= 0) {
    return;  // nothing to propagate; plane stays all-zero
  }

  const std::vector<int> degree = vertexDegrees(hg);
  propagate_neighborhood(hg, alpha, h, degree, density);
}

void one_hop_neighborhood_size(eda::Hypergraph& hg)
{
  const int n = hg.numVertices();
  std::vector<int>& size = hg.vertexIntPlane("hgm.neighborhood_size_1hop");
  std::fill(size.begin(), size.end(), 0);
  if (n == 0) {
    return;
  }

  const std::vector<int>& v_off = hg.vertexOffsets();
  const std::vector<int>& v_pins = hg.vertexPinList();
  const std::vector<int>& e_off = hg.hyperedgeOffsets();
  const std::vector<int>& e_pins = hg.pinList();

  // Epoch stamp: a neighbor w counts once per source u even if reached through
  // several incident hyperedges. No per-vertex O(n) clear needed.
  std::vector<int> stamp(n, -1);
  for (int u = 0; u < n; ++u) {
    int count = 0;
    for (int i = v_off[u]; i < v_off[u + 1]; ++i) {
      const int e = v_pins[i];
      for (int j = e_off[e]; j < e_off[e + 1]; ++j) {
        const int w = e_pins[j];
        if (w == u || stamp[w] == u) {
          continue;
        }
        stamp[w] = u;
        ++count;
      }
    }
    size[u] = count;
  }
}

void net_intersection_score(eda::Hypergraph& hg, utl::Logger* logger)
{
  const int n = hg.numVertices();
  std::vector<int>& score = hg.vertexIntPlane("hgm.net_intersection_score");
  std::fill(score.begin(), score.end(), 0);
  if (n == 0) {
    return;
  }

  const std::vector<int>& v_off = hg.vertexOffsets();
  const std::vector<int>& v_pins = hg.vertexPinList();
  const std::vector<int>& e_off = hg.hyperedgeOffsets();
  const std::vector<int>& e_pins = hg.pinList();

  // Membership scratch: stamp[w] == epoch marks w as a member of the "left"
  // hyperedge of the current pair. A monotonic epoch avoids clearing between
  // pairs (and between vertices).
  std::vector<int> stamp(n, -1);
  int epoch = 0;

  for (int u = 0; u < n; ++u) {
    const int deg = v_off[u + 1] - v_off[u];
    if (logger != nullptr && deg > kHighDegreeWarnThreshold) {
      logger->warn(utl::UKN, kMsgHighDegree,
                   "net_intersection_score: vertex {} has degree {} (> {}); "
                   "pair enumeration is quadratic and may be slow",
                   u, deg, kHighDegreeWarnThreshold);
    }

    long long total = 0;
    for (int a = v_off[u]; a < v_off[u + 1]; ++a) {
      const int e1 = v_pins[a];
      ++epoch;
      for (int j = e_off[e1]; j < e_off[e1 + 1]; ++j) {
        stamp[e_pins[j]] = epoch;
      }
      for (int b = a + 1; b < v_off[u + 1]; ++b) {
        const int e2 = v_pins[b];
        int shared = 0;
        for (int j = e_off[e2]; j < e_off[e2 + 1]; ++j) {
          if (stamp[e_pins[j]] == epoch) {
            ++shared;
          }
        }
        total += shared - 1;  // both e1 and e2 contain u; discount it
      }
    }
    score[u] = static_cast<int>(total);
  }
}

void tangle_score(eda::Hypergraph& hg, const int k_hop_radius,
                  utl::Logger* logger)
{
  const int n = hg.numVertices();
  std::vector<double>& tangle = hg.vertexDoublePlane("hgm.tangle_score");
  std::fill(tangle.begin(), tangle.end(), 0.0);
  if (n == 0) {
    return;
  }

  const std::vector<int>& v_off = hg.vertexOffsets();
  const std::vector<int>& v_pins = hg.vertexPinList();  // v -> incident edges
  const std::vector<int>& e_off = hg.hyperedgeOffsets();
  const std::vector<int>& e_pins = hg.pinList();         // e -> member vertices

  int clamp_count = 0;  // vertices whose raw Rent exponent exceeded 1.0

  for (int u = 0; u < n; ++u) {
    // Step 1: BFS to depth k_hop_radius, collecting the internal vertex set.
    // The induced subgraph is never materialized — `internal` and the inline
    // terminal scan below are all the state we keep.
    std::unordered_set<int> internal;
    internal.insert(u);
    std::deque<std::pair<int, int>> queue;  // (vertex, depth)
    queue.emplace_back(u, 0);
    while (!queue.empty()) {
      const std::pair<int, int> front = queue.front();
      queue.pop_front();
      const int v = front.first;
      const int depth = front.second;
      if (depth >= k_hop_radius) {
        continue;
      }
      for (int i = v_off[v]; i < v_off[v + 1]; ++i) {
        const int e = v_pins[i];
        for (int j = e_off[e]; j < e_off[e + 1]; ++j) {
          const int w = e_pins[j];
          if (internal.insert(w).second) {  // newly discovered at this depth
            queue.emplace_back(w, depth + 1);
          }
        }
      }
    }
    const int g = static_cast<int>(internal.size());

    // Step 2: count boundary terminals T. Scan every hyperedge incident to an
    // internal vertex once (dedup via seen_edges); a hyperedge with any
    // external member contributes its internal members as crossing pins.
    long long terminals = 0;
    std::unordered_set<HyperedgeId> seen_edges;
    for (const int v : internal) {
      for (int i = v_off[v]; i < v_off[v + 1]; ++i) {
        const HyperedgeId e = v_pins[i];
        if (!seen_edges.insert(e).second) {
          continue;  // already accounted for through another internal member
        }
        const int size = e_off[e + 1] - e_off[e];
        int n_internal = 0;
        for (int j = e_off[e]; j < e_off[e + 1]; ++j) {
          if (internal.count(e_pins[j]) != 0) {
            ++n_internal;
          }
        }
        if (size - n_internal > 0) {  // boundary-crossing hyperedge
          terminals += n_internal;
        }
      }
    }

    // Step 3: Rent exponent p = log(T)/log(G), clamped to [0, 1]. G <= 1 or
    // T == 0 (a fully enclosed or single-vertex subgraph) scores 0.
    double p = 0.0;
    if (g > 1 && terminals > 0) {
      const double raw = std::log(static_cast<double>(terminals))
                         / std::log(static_cast<double>(g));
      if (raw > 1.0) {
        ++clamp_count;
      }
      p = std::clamp(raw, 0.0, 1.0);
    }
    tangle[u] = p;
  }

  // Warn only when the pathological clamp is widespread (> 5% of vertices),
  // signalling the k_hop_radius may be too small for this netlist.
  if (logger != nullptr && clamp_count * 20 > n) {
    logger->warn(utl::UKN, kMsgTangleClamp,
                 "tangle_score: Rent exponent clamped to 1.0 on {} of {} "
                 "vertices ({:.1f}%) — induced subgraphs with more terminals "
                 "than cells; k_hop_radius may be too small",
                 clamp_count, n, 100.0 * clamp_count / n);
  }
}

}  // namespace hgm
