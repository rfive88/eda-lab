// See congestion_metrics.h for the metric definitions.

#include "hg_metrics/congestion_metrics.h"

#include <algorithm>
#include <list>

#include "support/logging.h"
#include "utl/Logger.h"

namespace hgm {

namespace {

// Debug group for this module's verbosity-3 histogram summaries. Library
// entry point, no CLI flag of its own — silent at verbosity 0/nullptr, per
// the repo's debugPrint convention (see src/support/logging.h).
constexpr const char* kGroup = "hg_metrics";

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

}  // namespace hgm
