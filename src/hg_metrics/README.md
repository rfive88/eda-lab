# src/hg_metrics/

Read-only metrics computed over an `eda::Hypergraph`, grouped by what they
help diagnose: congestion (routability risk from degree/fanout distributions)
and, in a later brief, timing. Every function takes the hypergraph `const`
and reads its CSR topology directly — nothing here mutates the hypergraph or
writes attribute planes. Attribute planes this module writes in later briefs
carry the `"hgm."` prefix to keep the namespace distinct from other engines.

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

Reads only the hypergraph's CSR topology (`vertexOffsets()`,
`hyperedgeOffsets()`, `numVertices()`, `numHyperedges()`) — no attribute
planes are read or written by any function in this brief.

## Control parameters

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `threshold` (`high_fanout_nets`) | `int` | none — required | inclusive minimum pin count for a hyperedge to be reported |
| `logger` (`*_stats` functions) | `utl::Logger*` | `nullptr` | optional; enables the verbosity-3 histogram summary line, group `"hg_metrics"` |

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
