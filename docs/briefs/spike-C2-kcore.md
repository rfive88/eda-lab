# Spike Brief C2 — k-Core Decomposition

## Goal

Implement k-core decomposition on the hypergraph, writing the k-core number for each vertex
into a named attribute plane. The k-core number is a structural centrality measure: vertices with
high k-core numbers sit inside dense, heavily-connected subgraphs — structural proxies for
routing congestion hot-spots.

## Context

- Prerequisite: C1 must be green. `src/hg_metrics/` and its build wiring already exist.
- Vertex degree is defined as the number of incident hyperedges (consistent with C1).
- A hyperedge is considered "active" as long as it contains ≥ 2 active vertices. When a vertex
  is peeled, each incident hyperedge loses one member; if it drops to 1 remaining active vertex,
  that hyperedge is effectively removed and the surviving vertex loses one degree unit.
- Result is written into the `Hypergraph` attribute plane `"hgm.k_core"` (type `int`).
- Do not modify files outside `src/hg_metrics/` and `tests/hg_metrics/`.

## Files to Modify

```
src/hg_metrics/congestion_metrics.h    ← add declaration
src/hg_metrics/congestion_metrics.cpp  ← add implementation
test/hg_metrics_congestion_test.cpp    ← add test cases
```

## API Addition

```cpp
// congestion_metrics.h — add inside namespace hgm

// Computes k-core number for every vertex and writes it to the
// "hgm.k_core" int attribute plane on hg.
// Returns the maximum k-core number found (the degeneracy of the hypergraph).
int k_core_numbers(Hypergraph& hg);
```

## Algorithm

Use the standard peeling algorithm adapted for hypergraphs:

1. Compute initial effective degree for each vertex: count incident hyperedges whose current size
   ≥ 2 (i.e., hyperedges that still contribute to connectivity).
2. Maintain a bucket queue (array of lists indexed by degree) for O(n + m) total work.
3. Process vertices in non-decreasing degree order:
   a. Assign `k_core[v] = current_degree[v]`.
   b. For each incident hyperedge `e` of `v`:
      - Decrement `active_count[e]`.
      - If `active_count[e]` drops to 1, the hyperedge is now degenerate: find its sole remaining
        active vertex `u` and decrement `current_degree[u]`. Update `u`'s bucket.
4. Write final `k_core[v]` values into the `"hgm.k_core"` attribute plane.
5. Return `max(k_core)`.

Track "removed" status per vertex with a boolean array local to the function; do not mutate
the hypergraph structure itself.

## Implementation Notes

- Use `std::vector<std::list<VertexId>>` for the bucket queue; maintain a pointer per vertex to
  its list node for O(1) removal on degree update.
- Vertices with degree 0 (isolated) get k_core = 0.
- The `"hgm.k_core"` plane should be created if it does not exist; overwritten if it does.
- Log the degeneracy (max k_core) and the distribution summary (mean, max) at verbosity level 2
  via `utl::Logger`.

## Test Requirements

Add to `test/hg_metrics_congestion_test.cpp`:

1. **Isolated vertices** — all get k_core = 0.
2. **Path graph** (vertices 0-1-2-3 connected as a chain via single-pin hyperedges):
   end vertices get k_core = 1, interior vertices get k_core = 1. (A path is 1-degenerate.)
3. **Clique** (4 vertices, every pair in a 2-pin hyperedge): all vertices get k_core = 3.
4. **Star hyperedge** (one hyperedge of size 5): all 5 vertices get k_core = 1 (they all share
   exactly one hyperedge).
5. **Mixed graph** with a dense core and sparse periphery: verify core vertices have strictly
   higher k_core than periphery vertices.
6. **Return value**: verify returned degeneracy matches `max` of all k_core plane values.

All tests must pass under `ctest` before proceeding to C3.

## Deliverables Checklist

- [ ] `k_core_numbers` implemented and declared
- [ ] `"hgm.k_core"` attribute plane written correctly on all vertices
- [ ] All C1 + C2 gtest cases green
- [ ] `FLOW.md` updated with k-core peeling diagram
- [ ] Committed with message `hg_metrics: C2 k-core decomposition`

## Hard Gate

Do not proceed to C3 until all tests in `test/` are green.
