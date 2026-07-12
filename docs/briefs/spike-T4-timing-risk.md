# Spike Brief T4 — Timing Risk Scores

## Goal

Compute two composite timing risk outputs that aggregate the results of T1, T2, and T3 into
actionable per-vertex scores: `critical_path_candidates` (structural identification of vertices
on deep paths) and `timing_risk_score` (a per-vertex composite that combines depth,
reconvergence, and cone size into a single risk ranking).

## Context

- Prerequisites: T2 green AND T3 green. All of the following attribute planes must exist:
  - `"hgm.topo_depth"` (int) — from T1
  - `"hgm.reconvergent"` (bool) — from T2
  - `"hgm.ball_size_2"` (double) — from T3 with t=2 (caller must run `vertex_ball_sizes` with t=2
    before calling `timing_risk_score`)
- These functions are read-only with respect to the previously written planes; they only write
  new planes.
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

// Returns the IDs of vertices whose topo_depth >= depth_threshold.
// These are the vertices structurally on or near the critical path.
// Results are sorted in descending order of topo_depth.
std::vector<VertexId> critical_path_candidates(const Hypergraph& hg, int depth_threshold);

// Computes a composite per-vertex timing risk score:
//   risk(v) = topo_depth(v) * reconvergence_multiplier(v) * ball_size_2(v)
// where reconvergence_multiplier(v) = 2.0 if "hgm.reconvergent"[v] == true, else 1.0.
// and ball_size_2(v) = "hgm.ball_size_2"[v] (must be pre-computed via vertex_ball_sizes(t=2)).
// Writes result to "hgm.timing_risk" (double) attribute plane on vertices.
// Returns the maximum risk score found.
double timing_risk_score(Hypergraph& hg);
```

## Algorithm

### `critical_path_candidates`

```
result = []
for each vertex v:
    if "hgm.topo_depth"[v] >= depth_threshold:
        result.push_back(v)
sort result by "hgm.topo_depth"[v] descending
return result
```

### `timing_risk_score`

```
max_risk = 0.0
for each vertex v:
    depth     = "hgm.topo_depth"[v]
    reconv    = "hgm.reconvergent"[v] ? 2.0 : 1.0
    ball_size = "hgm.ball_size_2"[v]     // 0.0 if plane missing on this vertex
    risk      = depth * reconv * ball_size
    write risk to "hgm.timing_risk"[v]
    max_risk = max(max_risk, risk)
return max_risk
```

If `"hgm.ball_size_2"` plane does not exist on the hypergraph, log a `utl::Logger` error
(verbosity 0) and return -1.0 without writing any values. Do not crash.

## Interpretation Notes (for FLOW.md)

- A vertex with high topo_depth but small ball_size is deep but narrow — likely a sequential
  path through inversion/buffer chains; moderate risk.
- A vertex with moderate depth, reconvergent flag, and large ball_size is a convergence point
  for a large logic cone — high risk.
- The multiplier of 2.0 for reconvergence is a structural heuristic; it can be made
  configurable in a future brief if calibration against placement results warrants it.

## Test Requirements

Add to `tests/hg_metrics/timing_metrics_test.cpp`:

Manually pre-populate all required attribute planes on a hand-constructed hypergraph
(bypassing T1/T2/T3 function calls to keep this brief's tests self-contained):

1. **`critical_path_candidates` basic**:
   - 5 vertices with topo_depth = {0, 1, 2, 3, 4}.
   - threshold=3: returns vertices with depth 3 and 4, sorted descending by depth.
   - threshold=5: returns empty vector.
   - threshold=0: returns all 5 vertices.

2. **`critical_path_candidates` sort order**:
   - Multiple vertices at same depth: verify they all appear in result (order among equal-depth
     vertices is unspecified, but all must be present).

3. **`timing_risk_score` non-reconvergent**:
   - Vertex with depth=4, reconvergent=false, ball_size_2=10.0: risk = 4 * 1.0 * 10.0 = 40.0.
   - Verify `"hgm.timing_risk"` plane value = 40.0.

4. **`timing_risk_score` reconvergent**:
   - Vertex with depth=3, reconvergent=true, ball_size_2=8.0: risk = 3 * 2.0 * 8.0 = 48.0.
   - Verify value = 48.0.

5. **`timing_risk_score` return value**:
   - Mix of the above: return value = max(40.0, 48.0) = 48.0.

6. **Missing `"hgm.ball_size_2"` plane**: verify function returns -1.0 and writes nothing.

7. **Zero depth vertex**: risk = 0.0 (sources have depth 0, regardless of ball_size or reconv).

All tests must pass under `ctest`.

## Deliverables Checklist

- [ ] `critical_path_candidates` implemented — reads `"hgm.topo_depth"`, returns sorted vector
- [ ] `timing_risk_score` implemented — reads three planes, writes `"hgm.timing_risk"`
- [ ] Missing-plane guard with `utl::Logger` error and -1.0 return
- [ ] `"hgm.timing_risk"` double plane written on all vertices
- [ ] All timing gtest cases green
- [ ] `FLOW.md` updated with composite risk scoring diagram and interpretation notes
- [ ] Committed with message `hg_metrics: T4 timing risk scores`

## Hard Gate

All tests in `tests/hg_metrics/` green. This is the final timing brief.
After T4 is committed, the full `src/hg_metrics/` module is complete for the initial scope.
