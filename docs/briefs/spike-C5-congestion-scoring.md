# Spike Brief C5 — Composite Congestion Scoring + Subcluster Identification

## Goal

Aggregate the outputs of C2 (k-core), C3 (neighborhood density), and C4 (tangle score) into a
single per-vertex congestion score on a 1–5 scale and group adjacent high-congestion vertices
into labelled subclusters. Supports two modes:

- **Full-design mode**: score all vertices in the hypergraph.
- **Selection mode**: score only a caller-supplied set of vertex indices (e.g. a set of instances
  the user wants to inspect), with percentile normalization scoped to that selection.

## Context

- Prerequisites: C2 green, C3 green, C4 green. The following attribute planes must exist on the
  hypergraph before calling any C5 function:
  - `"hgm.k_core"` (int) — from C2
  - `"hgm.neighborhood_density"` (double) — from C3
  - `"hgm.tangle_score"` (double) — from C4
- C5 is a pure aggregation layer. It reads the three planes above and writes one new plane
  (`"hgm.congestion_score"`). It never calls C2/C3/C4 functions itself; the caller is responsible
  for running them first.
- Percentile normalization is **always relative to the active scope** (all vertices or the
  supplied selection). This means the full 1–5 range is always used within the scope. A score of
  5 means "most congested within this comparison set", not an absolute threshold.
- Do not modify files outside `src/hg_metrics/` and `tests/hg_metrics/`.

## Why a Separate File

C5 is an aggregation layer that sits above C2/C3/C4. Putting it in `congestion_metrics.h` would
create a layering violation (that file is a peer of C2–C4, not a consumer of them). A separate
`congestion_scoring.h` makes the dependency graph explicit and keeps each file's responsibility
clear.

## Files to Create

```
src/hg_metrics/congestion_scoring.h
src/hg_metrics/congestion_scoring.cpp
test/hg_metrics_congestion_scoring_test.cpp
```

## Files to Modify

```
CMakeLists.txt (root)
```

The repo has one monolithic root `CMakeLists.txt` — no per-directory CMakeLists files exist.
Two additions are needed:

1. Add `congestion_scoring.cpp` to the existing `hg_metrics` library source list:
   ```cmake
   add_library(hg_metrics
     src/hg_metrics/congestion_metrics.cpp
     src/hg_metrics/timing_metrics.cpp
     src/hg_metrics/congestion_scoring.cpp)   ← add this line
   ```

2. Add a new test executable and ctest entry (following the pattern of existing test targets):
   ```cmake
   add_executable(hg_metrics_congestion_scoring_test
                  test/hg_metrics_congestion_scoring_test.cpp)
   target_link_libraries(hg_metrics_congestion_scoring_test
                         PRIVATE hg_metrics hypergraph odb utl
                                 GTest::gtest GTest::gtest_main)

   add_test(NAME hg_metrics_congestion_scoring_test
            COMMAND hg_metrics_congestion_scoring_test)
   ```

## Data Structures

```cpp
// src/hg_metrics/congestion_scoring.h
#pragma once
#include "hypergraph/hypergraph.h"
#include "support/status.h"
#include "congestion_metrics.h"   // for DistributionStats
#include <map>
#include <string>
#include <vector>

namespace hgm {

// Per-weight control for the three input metrics.
// Weights must sum to 1.0 (validated on entry; returns error otherwise).
// Default is equal weighting.
struct CongestionWeights {
    double k_core_weight       = 1.0 / 3.0;
    double neighborhood_weight = 1.0 / 3.0;
    double tangle_weight       = 1.0 / 3.0;
};

// A maximal set of hypergraph-adjacent vertices that all share the same
// congestion score (or score >= min_score threshold in clustering mode).
struct CongestionCluster {
    int              cluster_id;   // 0-based, assigned in BFS discovery order
    int              peak_score;   // maximum congestion_score (1-5) among members
    double           mean_score;   // mean raw composite [0,1] before binning
    int              size;         // number of member vertices
    std::vector<int> members;      // vertex indices (local dense indices)
};

// Summary returned by score_congestion.
struct CongestionReport {
    std::map<int, int>             score_histogram;    // score 1-5 → vertex count
    std::vector<CongestionCluster> clusters;           // all clusters (min_score=1)
    int                            num_vertices_scored;
    double                         mean_composite;     // mean raw [0,1] composite
    double                         p90_composite;      // 90th-percentile raw composite
};

} // namespace hgm
```

## API

```cpp
// src/hg_metrics/congestion_scoring.h — add inside namespace hgm

// Compute per-vertex congestion score (1-5) by combining C2/C3/C4 planes.
//
// Algorithm:
//   1. Validate required planes exist and weights sum to 1.0.
//   2. Determine scope: if scope is empty, use all vertices; else use scope.
//   3. For each metric, compute percentile rank of each vertex within scope → [0,1].
//   4. Weighted average of the three percentile ranks → composite [0,1].
//   5. Map composite to 1-5 using quintile bins:
//        [0.0, 0.2) → 1,  [0.2, 0.4) → 2,  [0.4, 0.6) → 3,
//        [0.6, 0.8) → 4,  [0.8, 1.0] → 5.
//   6. Write result to "hgm.congestion_score" (int) plane on all vertices in scope.
//      Vertices outside the scope are NOT written (their existing plane value is
//      left untouched, or the plane is left absent if it did not exist).
//   7. Populate report_out with histogram, clusters (min_score=1), and summary stats.
//
// Returns eda::Status error (without writing anything) if:
//   - Any required plane is missing.
//   - weights do not sum to 1.0 (tolerance 1e-6).
//   - scope contains an out-of-range vertex index.
//
// Logs warnings via utl::Logger at verbosity 1 for any vertex in scope that has
// a missing or default-zero value in a required plane (may indicate C2/C3/C4
// was not run on that vertex).
eda::Status score_congestion(
    eda::Hypergraph&        hg,
    CongestionReport&       report_out,
    const CongestionWeights& weights = {},
    const std::vector<int>& scope    = {}
);

// Group adjacent scored vertices into connected subclusters.
//
// Reads the "hgm.congestion_score" plane (must have been written by score_congestion).
// Two vertices are adjacent if they share at least one hyperedge AND both satisfy:
//   - in scope (if scope is non-empty)
//   - congestion_score >= min_score
//
// Returns clusters sorted by peak_score descending, then size descending.
// Each cluster's members list is in BFS discovery order from the seed vertex.
//
// Note: if you called score_congestion with a restricted scope, pass the same
// scope here so that adjacency is restricted to the same vertex set.
std::vector<CongestionCluster> find_congestion_clusters(
    const eda::Hypergraph&  hg,
    int                     min_score = 1,
    const std::vector<int>& scope     = {}
);

} // namespace hgm
```

## Implementation Notes

### Static helper — percentile ranking

Implement as a file-scope static function in `congestion_scoring.cpp`, not exposed in the header:

```
static std::vector<double> percentile_ranks(
    const std::vector<double>& all_values,   // plane values for ALL vertices
    const std::vector<int>& indices)         // the scope indices to rank
{
    // 1. Extract values at scope indices.
    // 2. Sort a copy.
    // 3. Edge case: all values identical → all ranks = 0.5 (maps to score 3).
    // 4. For each vertex i in indices:
    //        rank[i] = lower_bound(sorted_copy, all_values[i]) / (N - 1)
    //    where N = indices.size()
    // Return a vector<double> of size numVertices() with ranks set only
    // at scope positions; other positions are 0.0 (unused).
}
```

Apply this three times (once per metric). For `"hgm.k_core"` (int plane), cast values to double
before ranking.

### Quintile binning

```cpp
static int to_score(double composite) {
    if (composite < 0.2) return 1;
    if (composite < 0.4) return 2;
    if (composite < 0.6) return 3;
    if (composite < 0.8) return 4;
    return 5;
}
```

### `find_congestion_clusters` — BFS connected components

```
eligible = unordered_set of vertices where:
    - in scope (or scope is empty)
    - "hgm.congestion_score"[v] >= min_score

visited = empty set
cluster_id = 0

for each v in eligible (iterate in ascending index order for determinism):
    if v in visited: continue
    BFS from v:
        queue = {v}
        cluster_members = []
        while queue not empty:
            u = dequeue
            if u in visited: continue
            visited.insert(u)
            cluster_members.push_back(u)
            // find u's hyperedge-adjacent eligible neighbors:
            for each hyperedge e incident to u (via vertexOffsets/vertexPinList):
                for each vertex w in e (via hyperedgeOffsets/pinList):
                    if w in eligible and w not in visited:
                        enqueue w
        build CongestionCluster from cluster_members
        cluster_id++

sort clusters by peak_score desc, then size desc
```

Use the vertex-major CSR (`vertexOffsets()` / `vertexPinList()`) to enumerate each vertex's
incident hyperedges, then the hyperedge-major CSR (`hyperedgeOffsets()` / `pinList()`) to
enumerate each hyperedge's members.

### Weight validation

```cpp
double sum = weights.k_core_weight + weights.neighborhood_weight + weights.tangle_weight;
if (std::abs(sum - 1.0) > 1e-6)
    return eda::makeError(eda::ErrorCode::kInvalidArgument,
        "CongestionWeights must sum to 1.0, got " + std::to_string(sum));
```

### Logging

- Verbosity 0: log final score_histogram summary (count per band 1–5).
- Verbosity 1: log per-vertex warnings for zero-valued planes (may indicate un-run C2/C3/C4).
- Verbosity 2: log cluster count, largest cluster size, mean composite.
- Verbosity 3: log per-cluster member list (capped at `eda::kTraceCap`).

## Test Requirements

Create `test/hg_metrics_congestion_scoring_test.cpp`. Manually pre-populate the three required
attribute planes using `hg.vertexIntPlane("hgm.k_core")` etc., bypassing C2/C3/C4 function
calls, so C5 tests are self-contained.

### `score_congestion`

1. **Missing plane** — do not write `"hgm.k_core"` plane; verify `score_congestion` returns a
   non-OK Status and does not write `"hgm.congestion_score"`.

2. **Weight validation** — pass weights {0.5, 0.4, 0.4} (sum = 1.3); verify error returned.

3. **Known percentile → score mapping**:
   - 5 vertices with k_core = {1,2,3,4,5}, neighborhood_density = {1,2,3,4,5},
     tangle_score = {0.1, 0.3, 0.5, 0.7, 0.9}, equal weights.
   - Each metric's percentile ranks are {0.0, 0.25, 0.5, 0.75, 1.0}.
   - Composite: same distribution → scores must be {1, 2, 3, 4, 5} respectively.
   - Verify score_histogram = {1:1, 2:1, 3:1, 4:1, 5:1}.

4. **All identical metric values** — 4 vertices, all planes identical. All composites = 0.5
   (edge case: all tied). Verify all congestion_scores = 3 (midpoint).

5. **Scoped scoring**:
   - 6 vertices with varying values; scope = {2, 3, 4}.
   - Verify only vertices 2, 3, 4 have `"hgm.congestion_score"` written.
   - Verify the three scores span {1, 3, 5} (percentiles within the scope of 3).
   - Verify vertices 0, 1, 5 are absent from the plane (or retain their previous value if the
     plane already existed).

6. **Custom weights** — weights {0.6, 0.2, 0.2}:
   - Construct a vertex where k_core is high (top percentile) but density and tangle are low.
   - Verify that vertex scores higher under {0.6, 0.2, 0.2} than under equal weights.

7. **Single vertex** — scope = {0}, all plane values set. Verify score = 3 (sole member →
   percentile = 0.5 for all metrics → composite = 0.5 → score 3).

8. **report_out**:
   - On the 5-vertex test (case 3): verify `num_vertices_scored` = 5, `mean_composite` ≈ 0.5,
     `score_histogram` sums to 5.

### `find_congestion_clusters`

9. **Single cluster** — 4 vertices all with score=5, all mutually adjacent via one shared
   hyperedge. Verify: one cluster returned, peak_score=5, size=4.

10. **Two isolated clusters** — 4 vertices forming two pairs (pair A: vertices {0,1} connected;
    pair B: vertices {2,3} connected; no hyperedge crosses the pairs). Both pairs have score≥3.
    Verify: 2 clusters returned.

11. **min_score filter** — 5 vertices; scores = {1, 2, 3, 4, 5}; all connected in a chain.
    - min_score=4: only vertices with score 4 and 5 are eligible.
    - If those two are adjacent, one cluster of size 2; if not adjacent, two singleton clusters.
    - Verify correctly.

12. **Sort order** — three clusters with peak_scores {3, 5, 5} and sizes {4, 2, 6}:
    verify return order is peak=5/size=6, peak=5/size=2, peak=3/size=4.

13. **Scope restriction in clustering** — 6 vertices; vertices {3,4,5} are high-score but only
    {4,5} are in scope. Verify vertex 3 does not appear in any cluster.

14. **Empty hypergraph** — verify both functions return empty results without crash.

All tests must pass under `ctest` before C5 is considered complete.

## Deliverables Checklist

- [ ] `src/hg_metrics/congestion_scoring.h` — `CongestionWeights`, `CongestionCluster`,
      `CongestionReport`, `score_congestion`, `find_congestion_clusters` declared
- [ ] `src/hg_metrics/congestion_scoring.cpp` — both functions implemented; static percentile
      helper and `to_score` not exposed in header
- [ ] `"hgm.congestion_score"` int plane written only on vertices in scope
- [ ] Weight validation returns `eda::Status` error on bad weights
- [ ] Missing-plane guard returns `eda::Status` error without writing anything
- [ ] BFS clustering uses both CSR directions (vertex-major to find edges, edge-major for members)
- [ ] Clusters sorted by peak_score desc, then size desc
- [ ] All-identical-values edge case handled (all score 3)
- [ ] `test/hg_metrics_congestion_scoring_test.cpp` — all 14 test cases implemented
- [ ] Root `CMakeLists.txt` updated: `congestion_scoring.cpp` added to `hg_metrics` library,
      `hg_metrics_congestion_scoring_test` executable and `add_test` entry added
- [ ] `src/hg_metrics/README.md` updated to describe C5 functions
- [ ] `src/hg_metrics/FLOW.md` updated with:
      - percentile normalization → quintile binning flow
      - BFS clustering flow
      - composite scoring formula diagram
- [ ] All `tests/hg_metrics/` gtest cases green under `ctest`
- [ ] Committed with message `hg_metrics: C5 composite congestion scoring + subclusters`

## Design Notes for FLOW.md

**Why percentile ranking over raw-value normalization:**
Raw values can't be safely combined because k_core is unbounded integer (can reach 20+ on
dense netlists), neighborhood_density is unbounded double, and tangle_score is already [0,1].
Min-max normalization would let one dominant outlier compress all other scores into a narrow
band. Percentile ranking gives each metric equal influence regardless of its raw scale.

**Why scope-relative normalization:**
When the user supplies a selection, they want to know which cells in *that selection* are most
congested relative to each other. Normalizing against the full netlist could make the entire
selection score 1 if it happens to be from an uncongested region — not useful. Callers who want
absolute scores should pass an empty scope (full-design mode) and interpret the results knowing
the scale is relative to the full netlist.

**Why adjacency for clustering uses both CSR directions:**
The vertex-major CSR gives fast access to "what hyperedges does vertex u belong to" (needed to
enumerate neighbors). The hyperedge-major CSR gives fast access to "what vertices are in this
hyperedge" (needed to enumerate the actual neighbors). Using both avoids O(n) scans.

## Hard Gate

All tests in `tests/hg_metrics/` (including C1–C4 tests) must remain green. C5 must not break
any existing test. This is the final congestion brief; the timing track (T0–T4) is independent.
