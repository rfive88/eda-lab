# Spike Brief T1 — Topological Depth + Depth Histogram

## Goal

Compute per-vertex topological depth (longest path from any source in the TimingDAG) and
the depth histogram across the design. Topological depth is the primary structural timing proxy:
vertices at high depth are structurally at risk of setup violations before any timing engine
is run.

## Context

- Prerequisite: T0 green. `TimingDAG` struct is available from `timing_metrics.h`.
- Topological depth of a vertex `v` = length of the longest path from any source (in-degree 0
  vertex) to `v` in the DAG. Sources have depth 0.
- The `TimingDAG::topo_order` gives a valid processing order — iterate it left-to-right and
  propagate depths in a single pass.
- Result written to `"hgm.topo_depth"` (int) attribute plane on vertices.
- Do not modify files outside `src/hg_metrics/` and `tests/hg_metrics/`.

## Files to Modify

```
src/hg_metrics/timing_metrics.h
src/hg_metrics/timing_metrics.cpp
tests/hg_metrics/timing_metrics_test.cpp
```

## API Additions

```cpp
// timing_metrics.h — add inside namespace hgm

// Computes longest-path depth from any source for every vertex in the DAG.
// Writes result to "hgm.topo_depth" (int) attribute plane on vertices.
// Sources (in-degree 0 in dag.radj) get depth 0.
// Returns the maximum depth found (the combinational depth of the design).
int topological_depth(Hypergraph& hg, const TimingDAG& dag);

// Returns histogram: depth -> vertex count, derived from "hgm.topo_depth" plane.
// Requires topological_depth to have been called first.
std::map<int,int> depth_histogram(const Hypergraph& hg);
```

## Algorithm

### `topological_depth`

```
depth = vector<int>(num_vertices, 0)

for each vertex u in dag.topo_order:       // left-to-right = topological order
    for each vertex v in dag.adj[u]:       // u drives v
        depth[v] = max(depth[v], depth[u] + 1)

write depth[v] to "hgm.topo_depth" plane for all v
return max(depth)
```

Single pass, O(V + E). No BFS or DFS required — topo_order from T0 makes this trivial.

### `depth_histogram`

Read `"hgm.topo_depth"` plane from all vertices. Build and return `std::map<int,int>`.

## Test Requirements

Add to `tests/hg_metrics/timing_metrics_test.cpp`:

1. **Linear chain A→B→C→D**: depths = A:0, B:1, C:2, D:3. Return value = 3.

2. **Diamond (A→B, A→C, B→D, C→D)**:
   - A:0, B:1, C:1, D:2. Return value = 2.
   - Verify D's depth is 2 (longest path A→B→D or A→C→D).

3. **Two independent chains** (A→B, C→D): A:0, B:1, C:0, D:1. Return value = 1.

4. **Single vertex, no edges**: depth = 0. Return value = 0.

5. **Wide fan-out** (one source driving 10 sinks, no further edges):
   source depth = 0, all sinks depth = 1. Return value = 1.

6. **`depth_histogram`**: on the diamond case, histogram = {0:1, 1:2, 2:1}.

7. **Attribute plane**: verify `"hgm.topo_depth"` exists as int plane after call.

All tests must pass under `ctest` before proceeding to T2 or T3.

## Deliverables Checklist

- [ ] `topological_depth` and `depth_histogram` implemented and declared
- [ ] `"hgm.topo_depth"` int plane written on all vertices
- [ ] Single-pass algorithm (no BFS/DFS — uses topo_order directly)
- [ ] All timing gtest cases green
- [ ] `FLOW.md` updated
- [ ] Committed with message `hg_metrics: T1 topological depth`

## Hard Gate

All tests green before proceeding to T2 or T3. T2 and T3 are independent of each other and
both unblock after T1.
