# src/hg_metrics/

Metrics computed over an `eda::Hypergraph`, grouped by what they help
diagnose: congestion (routability risk from degree/fanout distributions and
structural density) and, in a later brief, timing. The distribution
functions take the hypergraph `const` and read its CSR topology directly;
`k_core_numbers` (Spike C2) also reads only the CSR arrays but writes its
result into an attribute plane. Attribute planes this module writes carry the
`"hgm."` prefix to keep the namespace distinct from other engines.

See FLOW.md for algorithmic flow diagrams.

## Congestion metrics (`congestion_metrics.h/.cpp`)

Three metric groups, all backed by the hypergraph's dual CSR arrays:

- **Vertex degree** — number of incident hyperedges per vertex, read from
  `vertexOffsets()` slice lengths.
  - `vertex_degree_histogram(hg)` → `std::map<int,int>`, degree → vertex
    count.
  - `vertex_degree_stats(hg)` → `DistributionStats` over vertex degrees.
- **Hyperedge size (fanout)** — pin count per hyperedge, read from
  `hyperedgeOffsets()` slice lengths.
  - `hyperedge_size_histogram(hg)` → `std::map<int,int>`, pin count →
    hyperedge count.
  - `hyperedge_size_stats(hg)` → `DistributionStats` over hyperedge sizes.
- **High-fanout nets** — `high_fanout_nets(hg, threshold)` returns the
  `HyperedgeId`s (local hyperedge indices) of every hyperedge whose pin
  count is `>= threshold`. No default threshold — the caller must be
  explicit.

### k-core decomposition (`k_core_numbers`)

`k_core_numbers(hg, logger?)` computes the **k-core number** of every vertex
— the largest `k` such that the vertex belongs to a subgraph in which every
vertex has effective degree `>= k` — and writes it into the `"hgm.k_core"`
int attribute plane (created if absent, overwritten in place if present).
High k-core numbers mark dense, heavily-connected cores: structural proxies
for routing congestion hot-spots. It is the one function here that takes the
hypergraph by **non-const** reference (it writes the plane) and returns the
**degeneracy** — the maximum k-core number found.

Definitions follow C1: a vertex's degree is its incident-hyperedge count, and
a hyperedge contributes to connectivity only while `>= 2` of its members
remain active — once it drops to a single survivor it is effectively removed
and that survivor loses one degree unit. Isolated (degree-0) vertices get
k-core 0. The algorithm is the standard peeling method adapted for
hypergraphs: an effective-degree bucket queue (`std::vector<std::list<int>>`
indexed by degree, one iterator per vertex for O(1) removal) drives an
O(n + pins) peel in non-decreasing degree order. Only the output plane is
written; the CSR topology is never mutated. See FLOW.md for the peeling
diagram.

### NESS neighborhood density (Spike C3)

Three label-weighted neighborhood metrics, all writing per-vertex `"hgm."`
int/double planes. Adjacency is the hypergraph relation: two vertices are one
hop apart iff they share a hyperedge. Each takes the hypergraph by
**non-const** reference because it writes a plane; the CSR topology is never
mutated.

- **`neighborhood_density(hg, alpha = 0.5, h = 2)`** — the NESS information-
  propagation score (Khan et al., SIGMOD 2011). For every vertex `u`:
  `A(u) = Σ_{i=1}^{h} alphaⁱ · Σ_{v: d(u,v)==i} degree(v)`, where `d` is the
  shortest-path hop distance and each neighbor's C1 degree (incident-hyperedge
  count) is its label weight. A single BFS per source (static helper
  `propagate_neighborhood`) walks out to `h` hops, accumulating decayed
  neighbor degree. High `A(u)` marks a vertex whose local neighborhood is
  dense with high-degree cells — a multi-hop routing-pressure proxy. Written
  to the `"hgm.neighborhood_density"` **double** plane. `alpha == 0` or
  `h == 0` (or `h < 0`) yields an all-zero plane.
- **`one_hop_neighborhood_size(hg)`** — the exact count of distinct vertices
  sharing at least one hyperedge with `u` (`u` excluded); equivalent to
  `neighborhood_density` with `h=1, alpha=1, label=1` but kept separate as a
  plain integer count. Single CSR pass, epoch-stamped so a neighbor reached
  through several nets counts once. Written to the
  `"hgm.neighborhood_size_1hop"` **int** plane.
- **`net_intersection_score(hg, logger?)`** — for every vertex `u`, the sum
  over all unordered pairs `(e1, e2)` of distinct hyperedges incident to `u`
  of `|V(e1) ∩ V(e2)| − 1` (discounting `u`, which is in every such
  intersection). Measures how much `u`'s nets overlap — a local routing-
  pressure signal. A vertex on fewer than two hyperedges scores 0. Written to
  the `"hgm.net_intersection_score"` **int** plane. The pair enumeration is
  quadratic in a vertex's degree; when a `logger` is attached it emits a
  warning (`utl::UKN`, id 130) for any vertex of degree
  `> kHighDegreeWarnThreshold` (64) that scoring it may be slow.

`DistributionStats { mean, p90, p99, max }` is defined once in
`congestion_metrics.h` and reused by `timing_metrics.h`. `p90`/`p99` are
nearest-rank percentiles over the sorted value set (`std::nth_element`,
index `floor(p * (n - 1))`); on an empty hypergraph all four fields default
to zero.

Every stats function takes an optional trailing `utl::Logger* logger =
nullptr`; when non-null it emits a one-line summary (mean/p99/max) via
`debugPrint` at verbosity level 3 (group `"hg_metrics"`), silent by default
— see "Logging" below. `vertex_degree_histogram`/`hyperedge_size_histogram`
accept the same trailing parameter for signature symmetry but do not
currently use it (the summary line is only emitted by the `*_stats`
functions).

## Input contract

The distribution functions read only the hypergraph's CSR topology
(`vertexOffsets()`, `hyperedgeOffsets()`, `numVertices()`, `numHyperedges()`)
and write no planes. `k_core_numbers` and the three NESS functions
additionally read `vertexPinList()` and `pinList()`, and each **writes** one
vertex plane — `"hgm.k_core"` (int), `"hgm.neighborhood_density"` (double),
`"hgm.neighborhood_size_1hop"` (int), `"hgm.net_intersection_score"` (int) —
as their only mutation; the CSR topology is left untouched.

## Control parameters

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `threshold` (`high_fanout_nets`) | `int` | none — required | inclusive minimum pin count for a hyperedge to be reported |
| `logger` (`*_stats` functions) | `utl::Logger*` | `nullptr` | optional; enables the verbosity-3 histogram summary line, group `"hg_metrics"` |
| `logger` (`k_core_numbers`) | `utl::Logger*` | `nullptr` | optional; enables a verbosity-2 (heartbeat) degeneracy/mean/max summary line, group `"hg_metrics"` |
| `alpha` (`neighborhood_density`) | `double` | `0.5` | per-hop decay factor in `(0,1)`; `0` yields an all-zero plane |
| `h` (`neighborhood_density`) | `int` | `2` | max hop depth of the propagation BFS; `0` (or negative) yields an all-zero plane |
| `logger` (`net_intersection_score`) | `utl::Logger*` | `nullptr` | optional; warns (id 130) for any vertex of degree `> 64`, whose quadratic pair enumeration may be slow |

## Timing metrics (`timing_metrics.h/.cpp`)

Stub only — header guard, `#include` of `congestion_metrics.h` for the
shared `DistributionStats` type, and a `TODO` for a later spike brief
(T0–T4). No declarations yet.

## How to run

```bash
cmake --build build
ctest --test-dir build -R hg_metrics_congestion_test --output-on-failure
# or, for a manual run (any output files land in run/, per repo convention):
cd run && ../build/hg_metrics_congestion_test
```

`test/hg_metrics_congestion_test.cpp` covers: an empty hypergraph (all
functions return empty/zeroed results without crashing); a single vertex
with no hyperedges (`{0: 1}` degree histogram, empty size histogram); a
known 4-vertex/3-hyperedge graph with sizes 2/3/4 (exact histogram counts,
`max == 4`, `p90`/`p99` within range); high-fanout thresholding (`>= 3`
returns the two larger hyperedges, `>= 99` returns nothing); and a star
hypergraph (one vertex in every hyperedge) confirming that vertex's degree
equals `numHyperedges()`.

The `KCoreTest` suite covers k-core decomposition: isolated vertices (all
k-core 0), a path graph (1-degenerate, all k-core 1), a 4-clique (all k-core
3), a single 5-pin star hyperedge (all k-core 1), a dense-core/sparse-
periphery graph (core vertices strictly above the periphery), the returned
degeneracy matching the plane's max, and a re-run overwriting the plane
cleanly rather than accumulating.

The NESS suites (`OneHopNeighborhoodTest`, `NeighborhoodDensityTest`,
`NetIntersectionScoreTest`, `NessPlanesTest`) cover: 1-hop size on a 5-pin
star (all 4), two disjoint 3-pin edges (all 2), and an isolated vertex (0);
`neighborhood_density` at `h=1, alpha=0.5` against a hand-computed star, the
`alpha=0`/`h=0` all-zero cases, and a 5-vertex chain where the middle vertex's
`h=2` density (2.5) strictly exceeds each end (1.5); `net_intersection_score`
on two hyperedges sharing three vertices (score 2 for the shared vertices, 0
for single-net and isolated vertices); and a check that all three planes exist
with the right type and one value per vertex.
