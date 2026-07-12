# Spike Brief T2 — Reconvergent Vertices + Register-to-Register Depth

## Goal

Identify reconvergent vertices (points where paths from a common ancestor merge) and compute
the maximum combinational depth between pairs of sequential elements. These are the two
primary structural timing risk indicators beyond depth alone: reconvergence creates hold/setup
sensitivity, and register-to-register depth directly bounds achievable clock frequency.

## Context

- Prerequisite: T1 green. `TimingDAG` and `"hgm.topo_depth"` plane are available.
- `"is_register"` bool plane on vertices is populated by T0's `annotate_timing_attributes`.
- A vertex `v` is **reconvergent** if it has ≥ 2 predecessors in the DAG (`radj[v].size() >= 2`)
  AND those predecessors share a common ancestor reachable within a bounded depth.
  For this implementation, use the simpler definition: `v` is reconvergent if `radj[v].size() >= 2`.
  This is exact for structural identification and avoids expensive ancestor-set intersection.
- Register-to-register depth: for each vertex `v`, compute the maximum combinational depth from
  any upstream register to `v` (i.e., the longest path from a register source to `v` in the DAG).
  This is particularly meaningful when `v` is itself a register — giving the critical combinational
  path length between sequential stages.
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

// Marks vertices where two or more paths converge (radj[v].size() >= 2).
// Writes true to "hgm.reconvergent" (bool) attribute plane on such vertices, false elsewhere.
// Returns count of reconvergent vertices found.
int mark_reconvergent_vertices(Hypergraph& hg, const TimingDAG& dag);

// For each vertex v, computes the longest combinational path from any upstream register to v.
// "Upstream register" = a vertex u where "is_register"[u] == true and u precedes v in topo_order.
// Vertices with no upstream register get a value of -1.
// Writes result to "hgm.reg_to_reg_depth" (int) attribute plane on vertices.
// Note: when v itself is a register, this gives the combinational depth of the path feeding it.
void register_to_register_depth(Hypergraph& hg, const TimingDAG& dag);
```

## Algorithm

### `mark_reconvergent_vertices`

Single pass over all vertices:
```
count = 0
for each vertex v:
    if dag.radj[v].size() >= 2:
        "hgm.reconvergent"[v] = true
        count++
    else:
        "hgm.reconvergent"[v] = false
return count
```

O(V). No graph traversal needed — `radj` from the TimingDAG provides in-degree directly.

### `register_to_register_depth`

Forward pass in topological order, tracking the longest distance from any upstream register:

```
reg_depth = vector<int>(num_vertices, -1)

for each vertex u in dag.topo_order:
    // Determine this vertex's reg_depth value for propagation purposes
    // If u is a register, it resets the counter for downstream vertices
    propagate_value = 0 if "is_register"[u] else (reg_depth[u] if reg_depth[u] >= 0 else -1)

    if propagate_value >= 0:
        for each vertex v in dag.adj[u]:
            reg_depth[v] = max(reg_depth[v], propagate_value + 1)

write reg_depth to "hgm.reg_to_reg_depth" plane
```

Key: a register vertex itself gets reg_depth = (value from its predecessors, if any), but when
propagating to its successors, it acts as a new source with propagate_value = 0 — so the clock
path "restarts" at each register boundary.

## Test Requirements

Add to `tests/hg_metrics/timing_metrics_test.cpp`:

### `mark_reconvergent_vertices`

1. **Linear chain A→B→C**: no reconvergent vertices (all radj sizes = 0 or 1). Return 0.

2. **Diamond (A→B, A→C, B→D, C→D)**: D is reconvergent (radj[D] = {B,C}). Return 1.
   Verify `"hgm.reconvergent"[D]` = true, all others false.

3. **Two paths merging at two points** (A→C, B→C, A→D, B→D): C and D are both reconvergent.
   Return 2.

4. **Single vertex**: return 0, plane written as false.

### `register_to_register_depth`

Set up `"is_register"` attribute plane manually on the test hypergraph
(bypass `annotate_timing_attributes`).

5. **R1 → A → B → R2** (R1 and R2 are registers, A and B are combinational):
   - reg_depth: R1=-1 (no upstream register), A=1, B=2, R2=3.
   - Verify R2's value = 3 (critical path length from R1 to R2 is 3 hops).

6. **Two register paths merging** (R1→A→C, R2→B→C where C is a register):
   - If path R1→A→C has length 2 and R2→B→C has length 2:
     reg_depth[C] = max(2, 2) = 2.
   - Verify.

7. **No registers in design**: all vertices get reg_depth = -1.

8. **Back-to-back registers** (R1→R2, both registers): R1 has no upstream register (reg_depth=-1);
   R2 gets reg_depth=1 (one hop from R1 acting as a register source).

All tests must pass under `ctest` before proceeding to T4.

## Deliverables Checklist

- [ ] `mark_reconvergent_vertices` implemented — O(V) single pass
- [ ] `register_to_register_depth` implemented — single forward pass in topo_order
- [ ] `"hgm.reconvergent"` bool plane written on all vertices
- [ ] `"hgm.reg_to_reg_depth"` int plane written on all vertices (-1 for no upstream register)
- [ ] All timing gtest cases green
- [ ] `FLOW.md` updated
- [ ] Committed with message `hg_metrics: T2 reconvergent vertices + reg-to-reg depth`

## Hard Gate

All tests green. T2 and T3 must both be green before T4 can begin.
