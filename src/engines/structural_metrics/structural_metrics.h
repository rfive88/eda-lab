// eda-lab: structural_metrics engine — core library (Spike SM1).
//
// Drives the hg_metrics congestion group (C1-C5) end to end over an
// already-built eda::Hypergraph and returns a structured summary. This is the
// library layer of the netlistgen / netlistgen_cli_core split: it does NO ODB
// loading — the caller (structural_metrics_cli, or any future in-process
// consumer) builds the Hypergraph and hands it in, so the analysis can run
// without spawning a subprocess.
//
// run_congestion_analysis() writes every hgm.* per-vertex attribute plane as a
// side effect (via the hg_metrics functions it calls) and returns a
// CongestionAnalysisResult carrying the summary/distribution data.
// print_congestion_report() renders that result as a human-readable two-part
// report through utl::Logger::report().
//
// Timing analysis (T0-T4) is out of scope for SM1 — the report prints a
// placeholder [ Timing ] section. SM2 will add run_timing_analysis /
// print_timing_report. See README.md and FLOW.md.

#pragma once

#include <map>
#include <vector>

#include "hg_metrics/congestion_metrics.h"  // DistributionStats
#include "hg_metrics/congestion_scoring.h"  // CongestionWeights, CongestionReport
#include "hypergraph/hypergraph.h"
#include "support/status.h"

namespace utl {
class Logger;
}  // namespace utl

namespace sm {

// Full result of a congestion analysis run.
// All per-vertex attribute planes are written to the hypergraph as a side
// effect of run_congestion_analysis(); this struct carries the summary data.
struct CongestionAnalysisResult
{
  // Design dimensions
  int num_vertices = 0;
  int num_hyperedges = 0;

  // C1: Degree + fanout distributions (supporting data)
  std::map<int, int> degree_histogram;
  hgm::DistributionStats degree_stats;
  std::map<int, int> fanout_histogram;
  hgm::DistributionStats fanout_stats;
  int high_fanout_count = 0;      // nets >= hf_threshold
  int high_fanout_threshold = 0;  // threshold used

  // C2: K-core (supporting data)
  int degeneracy = 0;  // max k-core number

  // C3: Neighborhood density + net intersection (supporting data)
  hgm::DistributionStats neighborhood_density_stats;
  hgm::DistributionStats net_intersection_stats;

  // C4: Tangle score (supporting data)
  hgm::DistributionStats tangle_stats;
  int tangle_hot_spot_count = 0;  // vertices with tangle > 0.8

  // C5: Congestion score (main output)
  hgm::CongestionReport congestion_report;
};

// Run full congestion analysis (C1 -> C2 -> C3 -> C4 -> C5) on an already-built
// hypergraph. Writes all hgm.* attribute planes as a side effect.
//
// Parameters:
//   hg                    — hypergraph built from a dbBlock (or from topology
//                           for tests)
//   result_out            — populated on success (cleared on entry)
//   logger                — shared logger; may be nullptr (silences diagnostics)
//   high_fanout_threshold — passed to hgm::high_fanout_nets (C1). Default 20.
//   weights               — passed to hgm::score_congestion (C5). Default equal
//                           thirds.
//
// Returns a fully populated CongestionAnalysisResult on success. Returns an
// eda::Status error (result_out then undefined) if any hg_metrics function
// fails (currently only C5's score_congestion can fail — bad weights).
eda::Status run_congestion_analysis(
    eda::Hypergraph& hg, CongestionAnalysisResult& result_out,
    utl::Logger* logger, int high_fanout_threshold = 20,
    const hgm::CongestionWeights& weights = {});

// Print the congestion report to stdout via logger->report(). A null logger is
// a no-op. Section order:
//   1. [ Design ]            — instance and net counts
//   2. [ Congestion Score ]  — score histogram + top clusters (peak >= 4)
//   3. [ Supporting Metrics ]— C1-C4 distribution summaries
//   4. [ Timing ]            — placeholder ("not yet available")
void print_congestion_report(const CongestionAnalysisResult& result,
                             utl::Logger* logger);

}  // namespace sm
