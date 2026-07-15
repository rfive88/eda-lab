// eda-lab: hg_metrics congestion metric group (Spike C1).
//
// Read-only distribution metrics computed over an eda::Hypergraph's CSR
// topology: vertex degree (incident hyperedge count), hyperedge size (pin
// count / fanout), and high-fanout net identification. Nothing here mutates
// the hypergraph or writes attribute planes — every function takes the
// hypergraph const. Planes this module writes in later briefs are prefixed
// "hgm." to keep the namespace distinct from other engines.

#pragma once

#include <map>
#include <vector>

#include "hypergraph/hypergraph.h"

namespace utl {
class Logger;
}  // namespace utl

namespace hgm {

// Local hyperedge index (eda::Hypergraph::hyperedgeIndex() space) — the
// hypergraph has no dedicated stable id type for hyperedges beyond OpenDB's
// own dbId<dbNet>, and these metrics are snapshot-local like the CSR arrays
// and attribute planes they read.
using HyperedgeId = int;

// Shared across congestion and timing metric headers (timing_metrics.h
// includes this one rather than redefining it).
struct DistributionStats
{
  double mean = 0.0;
  double p90 = 0.0;  // 90th percentile
  double p99 = 0.0;  // 99th percentile
  int max = 0;
};

// --- Vertex degree ---

// Histogram: degree (incident hyperedge count) -> vertex count.
std::map<int, int> vertex_degree_histogram(const eda::Hypergraph& hg,
                                           utl::Logger* logger = nullptr);
DistributionStats vertex_degree_stats(const eda::Hypergraph& hg,
                                      utl::Logger* logger = nullptr);

// --- Hyperedge size (fanout) ---

// Histogram: pin count -> hyperedge count.
std::map<int, int> hyperedge_size_histogram(const eda::Hypergraph& hg,
                                            utl::Logger* logger = nullptr);
DistributionStats hyperedge_size_stats(const eda::Hypergraph& hg,
                                       utl::Logger* logger = nullptr);

// --- High-fanout nets ---

// IDs (local indices) of hyperedges whose pin count is >= threshold. No
// default threshold — the caller must be explicit.
std::vector<HyperedgeId> high_fanout_nets(const eda::Hypergraph& hg,
                                          int threshold);

}  // namespace hgm
