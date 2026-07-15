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

// --- k-core decomposition ---

// Computes the k-core number of every vertex and writes it into the
// "hgm.k_core" int attribute plane on hg (created if absent, overwritten in
// place if present). A vertex's k-core number is the largest k such that it
// belongs to a subgraph in which every vertex has effective degree >= k;
// high values mark dense, heavily-connected cores — structural proxies for
// routing congestion hot-spots. Degree is the incident-hyperedge count (C1
// convention); a hyperedge contributes to connectivity while >= 2 of its
// members remain active, and stops the moment it drops to a single survivor.
// Isolated (degree-0) vertices get k-core 0. Returns the degeneracy — the
// maximum k-core number found. The optional logger emits a verbosity-2
// (heartbeat) summary line (group "hg_metrics"), silent otherwise.
int k_core_numbers(eda::Hypergraph& hg, utl::Logger* logger = nullptr);

// --- NESS neighborhood density (Spike C3) ---

// Label-weighted neighborhood propagation (NESS model — Khan et al., SIGMOD
// 2011). For each vertex u accumulates a structural density score over its
// h-hop neighborhood, using each neighbor's degree (incident-hyperedge count,
// the C1 convention) as its label weight and decaying by hop distance:
//   A(u) = sum_{i=1}^{h} alpha^i * sum_{v: d(u,v)==i} degree(v)
// where d(u,v) is the shortest-path hop distance over the hypergraph (two
// vertices are 1 hop apart iff they share a hyperedge). High A(u) marks a
// vertex whose local neighborhood is dense with high-degree cells — a proxy
// for multi-hop routing pressure. Writes the result to the
// "hgm.neighborhood_density" double plane on vertices (created if absent,
// overwritten in place if present).
//   alpha: decay factor, in (0,1). Default 0.5. alpha == 0 yields all zeros.
//   h:     max hop depth. Default 2. h == 0 yields all zeros.
void neighborhood_density(eda::Hypergraph& hg, double alpha = 0.5, int h = 2);

// For each vertex u: the number of distinct vertices that share at least one
// hyperedge with u (u itself excluded). This is the exact integer size of u's
// 1-hop neighborhood — equivalent to neighborhood_density with h=1, alpha=1,
// label=1 per neighbor, but kept separate because it is a plain count. Writes
// to the "hgm.neighborhood_size_1hop" int plane on vertices.
void one_hop_neighborhood_size(eda::Hypergraph& hg);

// For each vertex u: the sum over all unordered pairs (e1, e2) of distinct
// hyperedges incident to u of  |vertices(e1) ∩ vertices(e2)| - 1  (subtracting
// u itself, which is in every such intersection). Measures how much the nets
// incident to u overlap — high overlap means the same neighbors are reached
// through several nets, a local routing-pressure signal. A vertex incident to
// fewer than two hyperedges has no pairs and scores 0. Writes to the
// "hgm.net_intersection_score" int plane on vertices. The pair enumeration is
// quadratic in a vertex's degree; if attached, `logger` emits a warning for
// any vertex of degree > 64 (kHighDegreeWarnThreshold) that this vertex may be
// slow to score.
void net_intersection_score(eda::Hypergraph& hg, utl::Logger* logger = nullptr);

}  // namespace hgm
