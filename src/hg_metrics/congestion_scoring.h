// eda-lab: hg_metrics composite congestion scoring (Spike C5).
//
// Aggregation layer that sits *above* the C2/C3/C4 congestion metrics. It reads
// three per-vertex planes those spikes write —
//   "hgm.k_core"               (int,    C2 k_core_numbers)
//   "hgm.neighborhood_density" (double, C3 neighborhood_density)
//   "hgm.tangle_score"         (double, C4 tangle_score)
// — normalizes each to a within-scope percentile rank, blends the three ranks
// with caller-supplied weights, and bins the composite onto a 1-5 congestion
// score written to the single new "hgm.congestion_score" (int) plane. It never
// calls C2/C3/C4 itself; the caller runs those first. A second entry point
// groups hypergraph-adjacent high-score vertices into connected subclusters.
//
// Kept in its own file (not congestion_metrics.h) so the dependency direction
// is explicit: C5 is a consumer of C2-C4, not a peer. See FLOW.md.

#pragma once

#include <map>
#include <vector>

#include "hg_metrics/congestion_metrics.h"  // shared metric group / DistributionStats
#include "hypergraph/hypergraph.h"
#include "support/status.h"

namespace utl {
class Logger;
}  // namespace utl

namespace hgm {

// Per-metric blend weights. Must sum to 1.0 (validated on entry, tolerance
// 1e-6). Default is equal thirds.
struct CongestionWeights
{
  double k_core_weight = 1.0 / 3.0;
  double neighborhood_weight = 1.0 / 3.0;
  double tangle_weight = 1.0 / 3.0;
};

// A maximal set of hypergraph-adjacent vertices that all clear the clustering
// score threshold. Members are dense local vertex indices.
struct CongestionCluster
{
  int cluster_id = 0;      // 0-based, assigned in BFS discovery order
  int peak_score = 0;      // max "hgm.congestion_score" (1-5) among members
  double mean_score = 0.0; // mean "hgm.congestion_score" (1-5) of members
  int size = 0;            // number of member vertices
  std::vector<int> members;  // vertex indices, in BFS discovery order
};

// Summary returned by score_congestion.
struct CongestionReport
{
  std::map<int, int> score_histogram;         // score 1-5 -> vertex count
  std::vector<CongestionCluster> clusters;    // all clusters (min_score = 1)
  int num_vertices_scored = 0;
  double mean_composite = 0.0;                 // mean raw [0,1] composite
  double p90_composite = 0.0;                  // 90th-percentile raw composite
};

// Compute a per-vertex congestion score (1-5) by combining the C2/C3/C4 planes.
//
// Scope: an empty `scope` scores every vertex (full-design mode); a non-empty
// `scope` scores only those vertex indices, and every percentile rank is taken
// *relative to that selection* — so the full 1-5 range is always exercised
// within the active scope. Vertices outside the scope are left untouched:
// their prior "hgm.congestion_score" value is kept if the plane already
// existed, else it stays at the default 0 of the freshly-created plane.
//
// Returns an eda::Status error (writing nothing) if any required input plane is
// missing, the weights do not sum to 1.0 (tolerance 1e-6), or `scope` holds an
// out-of-range index. `report_out` is cleared on entry and populated on
// success. With a logger attached: an aggregate warning (UKN id 132) fires for
// any required plane that is all-zero over the scope (a "did you run C2/C3/C4?"
// signal), a verbosity-1 band-count summary, and — via find_congestion_clusters
// — verbosity-2 cluster stats and capped verbosity-3 per-cluster traces.
eda::Status score_congestion(eda::Hypergraph& hg, CongestionReport& report_out,
                             const CongestionWeights& weights = {},
                             const std::vector<int>& scope = {},
                             utl::Logger* logger = nullptr);

// Group adjacent scored vertices into connected subclusters. Reads the
// "hgm.congestion_score" plane (written by score_congestion; an empty result
// is returned if it is absent). Two vertices are adjacent iff they share a
// hyperedge and both are eligible: in `scope` (when non-empty) AND with
// congestion_score >= min_score. Returns clusters sorted by peak_score
// descending, then size descending; each cluster's members are in BFS
// discovery order from its seed. Pass the same `scope` used at scoring time so
// adjacency is restricted to the same vertex set.
//
// Takes the hypergraph by non-const reference because eda::Hypergraph exposes
// no const reader for an int plane (only findVertexDoublePlane is const); the
// CSR topology and the score plane are only read, never mutated.
std::vector<CongestionCluster> find_congestion_clusters(
    eda::Hypergraph& hg, int min_score = 1,
    const std::vector<int>& scope = {}, utl::Logger* logger = nullptr);

}  // namespace hgm
