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

### Local Rent exponent / tangle score (Spike C4)

`tangle_score(hg, k_hop_radius = 2, logger?)` writes a per-vertex **local Rent
exponent** into the `"hgm.tangle_score"` **double** plane (Alpert et al., DAC
2010). For each vertex `u` it inspects the k-hop induced subgraph centred on
`u` — **never materialized** as its own structure; `G` and `T` are computed
inline during the BFS:

- **G** = number of vertices within `k_hop_radius` hops of `u` (a BFS
  collecting the `internal` set, `u` included).
- **T** = boundary terminals: for every hyperedge with at least one internal
  **and** at least one external member, each of its internal members counts as
  one crossing pin. Hyperedges incident to the internal set are scanned once
  (deduplicated) — no double counting.
- **p** = `log(T) / log(G)`, clamped to `[0, 1]`; `G <= 1` or `T == 0` (a
  single-vertex or fully enclosed subgraph) yields `p = 0`.

`p ≈ 0` marks a well-encapsulated, datapath-like region; `p ≈ 1` marks a
cluster with as many terminals as cells — highly tangled, a routing-congestion
predictor. The upper clamp (`p > 1`) is pathological (more terminal pins than
cells, typical of very small induced subgraphs); when a `logger` is attached
and clamping fires on **more than 5%** of vertices, a warning (`utl::UKN`, id
131) flags that `k_hop_radius` may be too small for the netlist. Per the C4
brief, `tangle_score` takes the hypergraph by **non-const** reference (it
writes the plane) but never mutates the CSR topology.

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

## Composite congestion scoring (`congestion_scoring.h/.cpp`, Spike C5)

An aggregation layer that sits **above** C2/C3/C4 — kept in its own file so the
dependency direction is explicit (C5 consumes C2–C4; it never calls them). Two
entry points, both in namespace `hgm`:

- **`score_congestion(hg, report_out, weights = {}, scope = {}, logger = nullptr)`**
  → `eda::Status`. Reads the three required input planes `"hgm.k_core"` (int,
  C2), `"hgm.neighborhood_density"` (double, C3), `"hgm.tangle_score"` (double,
  C4); normalizes each to a **within-scope percentile rank** in `[0,1]`; blends
  the three ranks with `CongestionWeights` (default equal thirds, must sum to
  `1.0` ± `1e-6`); bins the composite onto a 1–5 score via quintiles
  (`[0,0.2)→1 … [0.8,1]→5`); and writes the result to the single new
  `"hgm.congestion_score"` **int** plane. Percentile ranking (rather than
  min-max) gives each metric equal influence regardless of raw scale — k_core
  is unbounded int, density unbounded double, tangle already `[0,1]` — and one
  dominant outlier cannot compress the rest into a narrow band. A tie in the
  scope (including a single-vertex scope) maps every rank to `0.5` → score 3.
- **`find_congestion_clusters(hg, min_score = 1, scope = {}, logger = nullptr)`**
  → `std::vector<CongestionCluster>`. BFS connected components over the vertices
  that are **eligible** — in `scope` (when non-empty) **and** with
  `"hgm.congestion_score" >= min_score`. Two vertices are adjacent iff they
  share a hyperedge. Clusters are returned sorted by `peak_score` descending,
  then `size` descending; `cluster_id` records the BFS discovery order and each
  cluster's `members` are in BFS discovery order from the seed. The BFS uses
  **both** CSR directions — vertex-major (`vertexOffsets()`/`vertexPinList()`)
  to enumerate a vertex's incident hyperedges, hyperedge-major
  (`hyperedgeOffsets()`/`pinList()`) to enumerate each hyperedge's members —
  avoiding an O(n) scan per hop.

**Scope semantics.** An empty `scope` scores every vertex (full-design mode); a
non-empty `scope` scores only those indices, with all percentile normalization
**relative to the selection**, so the full 1–5 range is always used within the
scope. Vertices outside the scope are left untouched — their prior
`"hgm.congestion_score"` value is kept if the plane already existed, else it
stays at the freshly-created plane's default 0. `score_congestion` returns an
`eda::Status` error **without writing anything** if a required input plane is
missing, the weights do not sum to 1.0, or `scope` holds an out-of-range index.
(The error category is `eda::ErrorCode::InvalidConfig`; the enum has no
`kInvalidArgument` despite the brief's wording.)

**`CongestionReport`** (filled by `score_congestion`): `score_histogram`
(score → vertex count), `clusters` (all clusters at `min_score = 1`),
`num_vertices_scored`, `mean_composite` and `p90_composite` (raw `[0,1]`
composite stats). **`CongestionCluster`**: `cluster_id`, `peak_score`,
`mean_score`, `size`, `members`. Note `mean_score` is the mean **binned
score (1–5)** of the members, not the raw composite — `find_congestion_clusters`
only has the persisted int score plane to work from, since C5 writes exactly
one plane; the raw composite is not persisted. No C5 test asserts `mean_score`.

**Deviations from the brief, and why.** (1) `find_congestion_clusters` takes the
hypergraph by **non-const** reference: `eda::Hypergraph` exposes no `const`
reader for an int plane (only `findVertexDoublePlane` is `const`), and the brief
constrains changes to `src/hg_metrics/`, so widening the parameter is preferable
to adding a probe to the core hypergraph API. The plane and CSR are only read.
(2) `score_congestion` takes an optional trailing `utl::Logger*` (the brief's
signature omitted it while its "Logging" section required one), matching every
other hg_metrics function. (3) Logging is mapped onto the repo's authoritative
verbosity scheme (CLAUDE.md) rather than the brief's literal level numbers,
since library code stays silent at verbosity 0: an aggregate **warn** (`utl::UKN`
id 132) fires for any required plane that is all-zero over the scope (a "did you
run C2/C3/C4?" signal); the band-count summary is `debugPrint` at verbosity 1;
cluster count/largest-size is verbosity 2; per-cluster member traces are
verbosity 3, capped at `eda::kTraceCap`.

## Input contract

The distribution functions read only the hypergraph's CSR topology
(`vertexOffsets()`, `hyperedgeOffsets()`, `numVertices()`, `numHyperedges()`)
and write no planes. `k_core_numbers`, the three NESS functions, and
`tangle_score` additionally read `vertexPinList()` and `pinList()`, and each
**writes** one vertex plane — `"hgm.k_core"` (int),
`"hgm.neighborhood_density"` (double), `"hgm.neighborhood_size_1hop"` (int),
`"hgm.net_intersection_score"` (int), `"hgm.tangle_score"` (double) — as their
only mutation; the CSR topology is left untouched.

`score_congestion` (C5) reads the `"hgm.k_core"`, `"hgm.neighborhood_density"`,
and `"hgm.tangle_score"` planes and writes one new plane,
`"hgm.congestion_score"` (int). `find_congestion_clusters` reads that same
score plane plus the CSR topology and writes nothing.

## Control parameters

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `threshold` (`high_fanout_nets`) | `int` | none — required | inclusive minimum pin count for a hyperedge to be reported |
| `logger` (`*_stats` functions) | `utl::Logger*` | `nullptr` | optional; enables the verbosity-3 histogram summary line, group `"hg_metrics"` |
| `logger` (`k_core_numbers`) | `utl::Logger*` | `nullptr` | optional; enables a verbosity-2 (heartbeat) degeneracy/mean/max summary line, group `"hg_metrics"` |
| `alpha` (`neighborhood_density`) | `double` | `0.5` | per-hop decay factor in `(0,1)`; `0` yields an all-zero plane |
| `h` (`neighborhood_density`) | `int` | `2` | max hop depth of the propagation BFS; `0` (or negative) yields an all-zero plane |
| `logger` (`net_intersection_score`) | `utl::Logger*` | `nullptr` | optional; warns (id 130) for any vertex of degree `> 64`, whose quadratic pair enumeration may be slow |
| `k_hop_radius` (`tangle_score`) | `int` | `2` | hop radius of the induced subgraph whose local Rent exponent is scored |
| `logger` (`tangle_score`) | `utl::Logger*` | `nullptr` | optional; warns (id 131) when the `p > 1` clamp fires on more than 5% of vertices (`k_hop_radius` likely too small) |
| `weights` (`score_congestion`) | `CongestionWeights` | equal thirds | per-metric blend weights; must sum to `1.0` ± `1e-6` |
| `scope` (`score_congestion` / `find_congestion_clusters`) | `std::vector<int>` | `{}` (all) | vertex indices to score/cluster; empty = full design, else percentile-relative to the selection |
| `min_score` (`find_congestion_clusters`) | `int` | `1` | inclusive minimum `congestion_score` for a vertex to be cluster-eligible |
| `logger` (`score_congestion`) | `utl::Logger*` | `nullptr` | optional; warns (id 132) for an all-zero required plane, verbosity-1 band summary, verbosity-2/3 cluster stats via `find_congestion_clusters` |
| `logger` (`find_congestion_clusters`) | `utl::Logger*` | `nullptr` | optional; verbosity-2 cluster count/largest-size, verbosity-3 per-cluster member traces (capped at `kTraceCap`) |

## Timing metrics (`timing_metrics.h/.cpp`)

Stub only — header guard, `#include` of `congestion_metrics.h` for the
shared `DistributionStats` type, and a `TODO` for a later spike brief
(T0–T4). No declarations yet.

## How to run

```bash
cmake --build build
ctest --test-dir build -R hg_metrics --output-on-failure
# or, for a manual run (any output files land in run/, per repo convention):
cd run && ../build/hg_metrics_congestion_test
cd run && ../build/hg_metrics_congestion_scoring_test
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

The `TangleScoreTest` suite covers the local Rent exponent: an isolated vertex
(`G=1 → p=0`), a fully enclosed subgraph (`T=0 → p=0`), a construction that
genuinely yields `G=4, T=4 → p=1.0` under the BFS, a datapath-like bus
(`G=8, T=2 → p=log2/log8=1/3`), a dense region captured only at
`k_hop_radius=2` (radius-2 score `≥` radius-1 score, exact values checked), and
plane existence/type. The `p=1.0` case departs from the brief's literal
topology, which cannot reach `G=4` under a straightforward BFS (a query
vertex's own external neighbor enters the ball at hop 1); the equivalent
construction produces the same `G, T, p` the brief intends.

`test/hg_metrics_congestion_scoring_test.cpp` (C5) hand-populates the three
required input planes to stay self-contained. `ScoreCongestionTest` covers: a
missing input plane and out-of-sum weights (both non-OK `eda::Status`, no score
plane written); the known percentile→score mapping (k_core/density/tangle =
`{1..5}` → scores `{1,2,3,4,5}`, histogram `{1:1…5:1}`); all-identical values
(every score 3); scoped scoring (scope `{2,3,4}` → scores `{1,3,5}`, out-of-
scope vertices left at 0); custom weights `{0.6,0.2,0.2}` lifting a high-k_core
vertex's score above its equal-weights score; a single-vertex scope (score 3);
and the report summary fields (`num_vertices_scored=5`, `mean_composite≈0.5`,
histogram sums to 5). `FindClustersTest` covers: a single fully-adjacent
cluster (peak 5, size 4); two isolated clusters; the `min_score` filter (chain
scores `1..5`, `min_score=4` → one cluster `{3,4}`); the peak-then-size sort
order (three components → `(peak5,size6),(peak5,size2),(peak3,size4)`); and
scope restriction excluding an out-of-scope high-score vertex.
`EmptyHypergraphTest` confirms both functions return empty results (no crash)
on a vertex-free hypergraph.
