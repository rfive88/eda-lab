// See congestion_metrics.h for the metric definitions.

#include "hg_metrics/congestion_metrics.h"

#include <algorithm>

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

}  // namespace hgm
