# Spike Brief C3 — NESS Neighborhood Density Vectors

## Goal

Implement a label-weighted neighborhood propagation model (based on the NESS paper — Khan et al.,
SIGMOD 2011) to compute per-vertex structural density scores. This captures how densely connected
each vertex's local neighborhood is, weighted by proximity — a stronger congestion predictor than
raw degree because it accounts for multi-hop routing pressure.

Two derived congestion metrics are also implemented here: `net_intersection_score` and
`one_hop_neighborhood_size`.

## Context

- Prerequisite: C1 green. C2 may run in parallel.
- The NESS information propagation model: for each vertex `u` and each "label" `l` in its
  h-hop neighborhood, the accumulated strength is:
  ```
  A(u, l) = sum_{i=1}^{h}  alpha^i  *  |{ v : d(u,v) == i  and  l in L(v) }|
  ```
  where `alpha` in (0,1) is the decay factor and `d(u,v)` is the shortest-path hop distance.
- In the netlist context, the "label" on each vertex is its **hyperedge degree** (from C1: number
  of incident hyperedges). Using degree as a label means A(u, degree_bucket) captures how many
  high-degree cells are near u — a proxy for routing demand in the neighborhood.
- The propagation BFS is a static helper inside `congestion_metrics.cpp` — not exposed in the
  header. Do not create a separate propagation_engine file.
- Results are written to named attribute planes on the hypergraph.
- Do not modify files outside `src/hg_metrics/` and `tests/hg_metrics/`.

## Files to Modify

```
src/hg_metrics/congestion_metrics.h
src/hg_metrics/congestion_metrics.cpp
test/hg_metrics_congestion_test.cpp
```

## API Additions

```cpp
// congestion_metrics.h — add inside namespace hgm

// Computes the NESS structural density score for each vertex.
// A(u) = sum_{i=1}^{h} alpha^i * sum_{v: d(u,v)==i} degree(v)
// (degree(v) = number of incident hyperedges of v, used as the label weight)
// Writes result to "hgm.neighborhood_density" (double) attribute plane on vertices.
// alpha: decay factor in (0,1). Recommended default: 0.5
// h:     max hop depth.        Recommended default: 2
void neighborhood_density(Hypergraph& hg, double alpha = 0.5, int h = 2);

// For each vertex u: count of distinct vertices that share at least one hyperedge with u.
// (Equivalent to neighborhood_density with h=1, alpha=1, label=1 per neighbor — kept as a
//  separate function for clarity and because it is an exact integer count.)
// Writes result to "hgm.neighborhood_size_1hop" (int) attribute plane on vertices.
void one_hop_neighborhood_size(Hypergraph& hg);

// For each vertex u: sum over all pairs of incident hyperedges (e1, e2) of
// |vertices(e1) ∩ vertices(e2)| - 1   (subtract u itself from the intersection).
// Measures how much the nets incident to u overlap — high overlap → local routing pressure.
// Writes result to "hgm.net_intersection_score" (int) attribute plane on vertices.
void net_intersection_score(Hypergraph& hg);
```

## Implementation Notes

### `neighborhood_density` — BFS propagation (static helper)

Implement as a static function `propagate_neighborhood` inside `congestion_metrics.cpp`:

```
for each vertex u as BFS source:
    visited[u] = 0 (distance 0)
    BFS queue with (u, depth=0)
    while queue not empty:
        (v, depth) = dequeue
        if depth >= h: continue
        for each hyperedge e incident to v:
            for each vertex w in e (w != v, not yet visited at shorter distance):
                visited[w] = depth + 1
                A[u] += alpha^(depth+1) * degree(w)
                enqueue (w, depth+1)
```

Use `std::vector<int> dist(num_vertices, -1)` reset per source vertex. Total complexity
O(V * (V + E * avg_edge_size)) for h=2 on typical netlists — acceptable for the netlist sizes
in this project.

Pre-compute `degree[v]` for all vertices once before the outer loop (reuse logic from C1).

### `one_hop_neighborhood_size`

For each vertex `u`: collect the union of all vertices in all incident hyperedges, subtract `u`
itself. Write the count to `"hgm.neighborhood_size_1hop"`. This is a single pass over the CSR
structure — no BFS needed.

### `net_intersection_score`

For each vertex `u`:
- Collect all incident hyperedges into a local list.
- For each pair (e1, e2) from that list: compute |V(e1) ∩ V(e2)| using a hash set built from
  V(e1), then count how many vertices of V(e2) are in it. Subtract 1 for `u` itself.
- Accumulate and write to `"hgm.net_intersection_score"`.

For vertices with many incident hyperedges (high degree), the pair enumeration is quadratic in
degree. Log a warning via `utl::Logger` if any vertex has degree > 64, noting that
`net_intersection_score` may be slow for that vertex.

## Test Requirements

Add to `test/hg_metrics_congestion_test.cpp`:

1. **`one_hop_neighborhood_size`**:
   - Star topology (one central vertex in a single hyperedge of size 5): central vertex score = 4,
     leaf vertices score = 4 (all share the same hyperedge).
   - Two disjoint hyperedges sharing no vertices: each vertex's score = (hyperedge_size - 1).

2. **`neighborhood_density` (h=1, alpha=0.5)**:
   - Reduces to: A(u) = 0.5 * sum of degree(v) for all 1-hop neighbors v.
   - Verify on a small hand-computed graph where neighbor degrees are known.
   - Verify alpha=0.0 produces all zeros. Verify h=0 produces all zeros.

3. **`neighborhood_density` (h=2, alpha=0.5)**:
   - Construct a chain of 5 vertices. Verify that the middle vertex has a higher density score
     than the end vertices (it sees more neighbors at both h=1 and h=2).

4. **`net_intersection_score`**:
   - Two hyperedges sharing 3 vertices (including u): intersection = 2 (exclude u). Verify.
   - Vertex incident to only one hyperedge: score = 0 (no pairs to intersect).
   - Vertex with no incident hyperedges: score = 0.

5. **Attribute planes**: verify all three planes exist on the hypergraph after calling each
   function, with correct type and one value per vertex.

All tests must pass under `ctest` before proceeding to C4.

## Deliverables Checklist

- [ ] `neighborhood_density`, `one_hop_neighborhood_size`, `net_intersection_score` implemented
- [ ] Static BFS helper is not exposed in the header
- [ ] `"hgm.neighborhood_density"` (double), `"hgm.neighborhood_size_1hop"` (int),
      `"hgm.net_intersection_score"` (int) planes written correctly
- [ ] High-degree warning logged via `utl::Logger`
- [ ] All C1 + C3 gtest cases green (C2 may be in flight independently)
- [ ] `FLOW.md` updated with propagation BFS diagram and per-function flows
- [ ] Committed with message `hg_metrics: C3 NESS neighborhood density`

## Hard Gate

Do not proceed to C4 until all tests in `test/` are green.
