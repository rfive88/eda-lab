// See structural_metrics.h for the SM1 engine definitions.

#include "engines/structural_metrics/structural_metrics.h"

#include <algorithm>
#include <string>
#include <vector>

#include "support/logging.h"
#include "utl/Logger.h"

namespace sm {

namespace {

// One debug group for the whole engine, so -verbosity lifts the run (see
// src/support/logging.h). Library-layer phase markers go through debugPrint
// (debug-gated, silent at verbosity 0); print_congestion_report's summary uses
// logger->report() so it always shows.
constexpr const char* kGroup = "structural_metrics";

// Message-id block for the core library's warn()/info() lines. The repo
// partitions ids across the shared utl::UKN namespace (see
// src/support/logging.h): structural_metrics core claims 350-374, the CLI
// 375-399. debugPrint takes no id, so only warnings need ids here so far.
// (None emitted yet — the hg_metrics functions own their own diagnostics.)

// Tangle "hot-spot" cutoff: a vertex whose hgm.tangle_score exceeds this is a
// local Rent anomaly worth counting in the report (matches the brief's spec).
constexpr double kTangleHotSpot = 0.8;

// Nearest-rank percentile position in a 0-indexed sorted array of size n:
// index = floor(p * (n - 1)), clamped into [0, n - 1]. Mirrors the logic in
// hg_metrics/congestion_metrics.cpp (percentileIndex) — kept here rather than
// shared because SM1 must not modify src/hg_metrics/, and that helper is a
// file-scope static over int there while we need it over a double plane.
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

// DistributionStats over a per-vertex double plane already written on `hg`.
// Uses the same nearest-rank percentile convention as hgm::vertex_degree_stats.
// DistributionStats::max is int (the shared type), so a double plane's max is
// truncated on store — the percentiles (p90/p99, double fields) keep full
// precision. An absent plane or an empty graph yields a zeroed stats struct.
hgm::DistributionStats stats_from_double_plane(eda::Hypergraph& hg,
                                               const std::string& plane_name)
{
  hgm::DistributionStats stats;
  if (!hg.hasVertexPlane(plane_name)) {
    return stats;
  }
  std::vector<double> values = hg.vertexDoublePlane(plane_name);
  if (values.empty()) {
    return stats;
  }

  double sum = 0.0;
  double max_value = values.front();
  for (const double v : values) {
    sum += v;
    max_value = std::max(max_value, v);
  }
  stats.mean = sum / static_cast<double>(values.size());
  stats.max = static_cast<int>(max_value);

  const int n = static_cast<int>(values.size());
  const int p90_idx = percentileIndex(n, 0.90);
  std::nth_element(values.begin(), values.begin() + p90_idx, values.end());
  stats.p90 = values[p90_idx];

  const int p99_idx = percentileIndex(n, 0.99);
  std::nth_element(values.begin(), values.begin() + p99_idx, values.end());
  stats.p99 = values[p99_idx];

  return stats;
}

// Count vertices in a double plane whose value exceeds `threshold`.
int count_above(eda::Hypergraph& hg, const std::string& plane_name,
                const double threshold)
{
  if (!hg.hasVertexPlane(plane_name)) {
    return 0;
  }
  const std::vector<double>& values = hg.vertexDoublePlane(plane_name);
  int count = 0;
  for (const double v : values) {
    if (v > threshold) {
      ++count;
    }
  }
  return count;
}

}  // namespace

eda::Status run_congestion_analysis(eda::Hypergraph& hg,
                                    CongestionAnalysisResult& result_out,
                                    utl::Logger* logger,
                                    const int high_fanout_threshold,
                                    const hgm::CongestionWeights& weights)
{
  result_out = CongestionAnalysisResult{};
  utl::Logger* lg = logger;  // debugPrint's macro needs a pointer expression

  // --- C1: degree / fanout distributions + high-fanout nets ---
  if (lg != nullptr) {
    debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
               "SM1: C1 degree/fanout distributions");
  }
  result_out.degree_histogram = hgm::vertex_degree_histogram(hg, logger);
  result_out.degree_stats = hgm::vertex_degree_stats(hg, logger);
  result_out.fanout_histogram = hgm::hyperedge_size_histogram(hg, logger);
  result_out.fanout_stats = hgm::hyperedge_size_stats(hg, logger);
  result_out.high_fanout_threshold = high_fanout_threshold;
  result_out.high_fanout_count =
      static_cast<int>(hgm::high_fanout_nets(hg, high_fanout_threshold).size());

  // --- C2: k-core decomposition (writes hgm.k_core) ---
  if (lg != nullptr) {
    debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
               "SM1: C2 k-core decomposition");
  }
  result_out.degeneracy = hgm::k_core_numbers(hg, logger);

  // --- C3: neighborhood density + net intersection ---
  if (lg != nullptr) {
    debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
               "SM1: C3 neighborhood density / net intersection");
  }
  hgm::neighborhood_density(hg);
  hgm::one_hop_neighborhood_size(hg);
  hgm::net_intersection_score(hg, logger);
  result_out.neighborhood_density_stats =
      stats_from_double_plane(hg, "hgm.neighborhood_density");
  // net_intersection_score writes an int plane; lift a copy to double so the
  // shared double-plane stats helper can read it (no int-plane const reader
  // exists and we must not touch hgm code). Cheap: one pass over n vertices.
  {
    const std::vector<int>& nis = hg.vertexIntPlane("hgm.net_intersection_score");
    std::vector<double>& nis_d = hg.vertexDoublePlane("sm.net_intersection_d");
    for (std::size_t i = 0; i < nis.size(); ++i) {
      nis_d[i] = static_cast<double>(nis[i]);
    }
    result_out.net_intersection_stats =
        stats_from_double_plane(hg, "sm.net_intersection_d");
    hg.removeVertexPlane("sm.net_intersection_d");
  }

  // --- C4: local Rent tangle score (writes hgm.tangle_score) ---
  if (lg != nullptr) {
    debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
               "SM1: C4 tangle score");
  }
  hgm::tangle_score(hg, /*k_hop_radius=*/2, logger);
  result_out.tangle_stats = stats_from_double_plane(hg, "hgm.tangle_score");
  result_out.tangle_hot_spot_count =
      count_above(hg, "hgm.tangle_score", kTangleHotSpot);

  // --- C5: composite congestion score (writes hgm.congestion_score) ---
  if (lg != nullptr) {
    debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
               "SM1: C5 composite congestion scoring");
  }
  const eda::Status status = hgm::score_congestion(
      hg, result_out.congestion_report, weights, /*scope=*/{}, logger);
  if (!status.ok()) {
    return status;
  }

  // --- Design dimensions ---
  result_out.num_vertices = hg.numVertices();
  result_out.num_hyperedges = hg.numHyperedges();

  if (lg != nullptr) {
    debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
               "SM1: analysis complete — {} vertices, {} hyperedges, "
               "degeneracy {}, {} tangle hot-spots",
               result_out.num_vertices, result_out.num_hyperedges,
               result_out.degeneracy, result_out.tangle_hot_spot_count);
  }
  return eda::okStatus();
}

void print_congestion_report(const CongestionAnalysisResult& result,
                             utl::Logger* logger)
{
  if (logger == nullptr) {
    return;
  }

  auto stat_line = [&](const char* label, const hgm::DistributionStats& s) {
    logger->report("  {:<20} : mean={:.2f}  p90={:.2f}  p99={:.2f}  max={:.2f}",
                   label, s.mean, s.p90, s.p99, static_cast<double>(s.max));
  };

  // Score histogram: percentages relative to the number of scored vertices.
  const int scored = result.congestion_report.num_vertices_scored;
  auto band = [&](const int score) {
    const auto it = result.congestion_report.score_histogram.find(score);
    return it != result.congestion_report.score_histogram.end() ? it->second : 0;
  };
  auto pct = [&](const int n) {
    return scored > 0 ? 100.0 * static_cast<double>(n) / scored : 0.0;
  };

  logger->report("=== Structural Metrics Report ===");
  logger->report("");

  logger->report("[ Design ]");
  logger->report("  Instances  : {}", result.num_vertices);
  logger->report("  Nets       : {}", result.num_hyperedges);
  logger->report("");

  logger->report("[ Congestion Score ]");
  logger->report("  Score 5 (critical) : {:>3}  ({:>4.1f}%)", band(5),
                 pct(band(5)));
  logger->report("  Score 4            : {:>3}  ({:>4.1f}%)", band(4),
                 pct(band(4)));
  logger->report("  Score 3            : {:>3}  ({:>4.1f}%)", band(3),
                 pct(band(3)));
  logger->report("  Score 2            : {:>3}  ({:>4.1f}%)", band(2),
                 pct(band(2)));
  logger->report("  Score 1 (clean)    : {:>3}  ({:>4.1f}%)", band(1),
                 pct(band(1)));
  logger->report("");
  logger->report("  Top congestion clusters (score >= 4):");
  int printed = 0;
  for (const hgm::CongestionCluster& c : result.congestion_report.clusters) {
    if (c.peak_score < 4) {
      continue;  // clusters are sorted peak desc, so we could break — but a
                 // guard is clearer and the list is short
    }
    logger->report("    Cluster {} : {} instances, peak={}", c.cluster_id,
                   c.size, c.peak_score);
    ++printed;
  }
  if (printed == 0) {
    logger->report("    None");
  }
  logger->report("");

  logger->report("[ Supporting Metrics ]");
  stat_line("Vertex degree", result.degree_stats);
  stat_line("Net fanout", result.fanout_stats);
  logger->report("  {:<20} : {} (threshold >= {})", "High-fanout nets",
                 result.high_fanout_count, result.high_fanout_threshold);
  logger->report("  {:<20} : {}", "K-core degeneracy", result.degeneracy);
  stat_line("Neighborhood density", result.neighborhood_density_stats);
  stat_line("Net intersection", result.net_intersection_stats);
  stat_line("Tangle score", result.tangle_stats);
  logger->report("  {:<20} : {} instances with score > 0.8", "Tangle hot-spots",
                 result.tangle_hot_spot_count);
  logger->report("");

  logger->report("[ Timing ]");
  logger->report("  (not yet available — implement T0-T4 to enable)");
}

}  // namespace sm
