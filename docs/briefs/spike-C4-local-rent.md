# Spike Brief C4 — Local Rent Exponent (Tangle Score)

## Goal

Implement a per-vertex tangle score derived from Rent's rule, computed on the induced k-hop
subgraph around each vertex. A high tangle score indicates that the local cluster of cells is
anomalously highly connected relative to its size — a structural predictor of routing congestion
per the Alpert et al. (DAC 2010) paper.

## Context

- Prerequisites: C1 green, C3 green (C3's BFS traversal pattern informs this brief).
- Rent's rule: `T = k_avg * G^p`, where T = external terminal count (nets crossing the subgraph
  boundary), G = cell count inside the subgraph, and p is the Rent exponent (0 ≤ p ≤ 1 typically).
  High p → high connectivity relative to size → "tangled" structure.
- The tangle score for vertex `u` is the Rent exponent `p` of the k-hop induced subgraph
  centred on `u`. The induced subgraph is **not materialized** as a new data structure; T and G
  are computed inline during the BFS traversal.
- Do not create a separate induced-subgraph utility. Do not modify files outside `src/hg_metrics/`.

## Files to Modify

```
src/hg_metrics/congestion_metrics.h
src/hg_metrics/congestion_metrics.cpp
tests/hg_metrics/congestion_metrics_test.cpp
```

## API Addition

```cpp
// congestion_metrics.h — add inside namespace hgm

// Computes the local Rent exponent (tangle score) for each vertex.
// For each vertex u, induces the subgraph of all vertices within k_hop hops and counts:
//   G = number of internal vertices (those within k_hop of u)
//   T = number of boundary-crossing hyperedge-vertex incidences
//       (for each hyperedge with ≥1 internal and ≥1 external vertex,
//        count its internal vertices — each is a "terminal" pin crossing the boundary)
// Rent exponent: p = log(T) / log(G),  clamped to [0.0, 1.0]
// Special cases: if G <= 1 or T == 0, write p = 0.0.
// Writes result to "hgm.tangle_score" (double) attribute plane on vertices.
// k_hop_radius: hop radius of the induced subgraph. Recommended default: 2
void tangle_score(Hypergraph& hg, int k_hop_radius = 2);
```

## Algorithm

For each vertex `u`:

1. **BFS to collect internal vertices**: run BFS from `u` up to depth `k_hop_radius`, collecting
   the set `internal` of all visited vertices (including `u`). G = |internal|.

2. **Count terminals inline**: iterate over all hyperedges incident to any internal vertex:
   - For each such hyperedge `e`:
     - Count `n_internal` = number of vertices in `e` that are in `internal`.
     - Count `n_external` = (size of `e`) - `n_internal`.
     - If `n_external > 0` (boundary-crossing hyperedge): T += `n_internal`.

3. **Compute exponent**:
   ```
   if G <= 1 or T == 0:
       p = 0.0
   else:
       p = log(T) / log(G)
       p = clamp(p, 0.0, 1.0)
   ```

4. Write `p` to `"hgm.tangle_score"` for vertex `u`.

**Implementation note**: use `std::unordered_set<VertexId>` for the `internal` set during BFS.
For the hyperedge scan in step 2, iterate over the CSR incidence list of each internal vertex
and deduplicate hyperedges with a local `std::unordered_set<HyperedgeId>` to avoid double-counting.

Use `std::log` (natural log). The ratio `log(T)/log(G)` is base-independent so the log base does
not matter as long as it is consistent.

## Interpretation

- p close to 0: the subgraph is well-encapsulated (datapath-like), few external connections.
- p close to 1: the subgraph has as many external connections as cells — highly tangled,
  strong routing congestion predictor.
- p > 1 (clamped to 1.0): pathological; indicates the subgraph has more terminal pins than cells,
  common for very small induced subgraphs (G=2 or 3). Log a `utl::Logger` warning when clamping
  occurs on more than 5% of vertices.

## Test Requirements

Add to `congestion_metrics_test.cpp`:

1. **Isolated vertex** (G=1): tangle_score = 0.0.

2. **Fully enclosed subgraph** — all edges internal, T=0: tangle_score = 0.0.

3. **Known Rent computation**:
   - Construct: 4 internal vertices each in one shared hyperedge (size 4, all internal) and each
     also in one external hyperedge connecting to an outside vertex.
   - G=4, T=4 (each internal vertex is terminal in its external hyperedge), p = log(4)/log(4) = 1.0.
   - Verify tangle_score = 1.0 (or clamped to 1.0).

4. **Datapath-like structure**:
   - A bus: 8 vertices connected to each other via one large internal hyperedge, plus 2 external
     nets (2 terminals total).
   - G=8, T=2, p = log(2)/log(8) ≈ 0.333. Verify within 1e-6.

5. **k_hop_radius=1 vs k_hop_radius=2**:
   - On a graph with a dense core, verify that k_hop_radius=2 yields a higher (or equal)
     tangle score than k_hop_radius=1 for the central vertex, since more of the dense region
     is captured.

6. **Attribute plane**: verify `"hgm.tangle_score"` exists as a double plane after the call.

All tests must pass under `ctest`.

## Deliverables Checklist

- [ ] `tangle_score` implemented and declared
- [ ] No induced-subgraph data structure created; T and G computed inline
- [ ] `"hgm.tangle_score"` double attribute plane written on all vertices
- [ ] Clamping warning logged via `utl::Logger` when triggered on >5% of vertices
- [ ] All C1 + C3 + C4 gtest cases green
- [ ] `FLOW.md` updated with tangle-score BFS + terminal-counting diagram
- [ ] Committed with message `hg_metrics: C4 local Rent tangle score`

## Hard Gate

All tests in `tests/hg_metrics/` must be green before this brief is considered complete.
C4 has no downstream dependents in the congestion track; the timing track (T0–T4) is independent.
