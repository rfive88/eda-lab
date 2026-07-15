# Flow: hg_metrics

`src/hg_metrics/` computes read-only metrics over an `eda::Hypergraph`'s CSR
topology. This brief (Spike C1) implements the congestion metric group —
vertex degree distribution, hyperedge size (fanout) distribution, and
high-fanout net identification — plus a stub `timing_metrics.h/.cpp` so the
build is complete for a later brief. Nothing here mutates the hypergraph or
writes attribute planes.

## `congestion_metrics.h` — API contract

Declares `DistributionStats` (`mean`, `p90`, `p99`, `max`; shared with
`timing_metrics.h`), the `HyperedgeId` alias (a local hyperedge index — this
module has no dedicated stable id type, only the snapshot-local CSR index),
and six functions: `vertex_degree_histogram`/`vertex_degree_stats`,
`hyperedge_size_histogram`/`hyperedge_size_stats`, and `high_fanout_nets`.

```mermaid
graph TD
    A["const eda::Hypergraph& hg"] --> B["vertex_degree_histogram(hg)<br/>-> map&lt;degree, count&gt;"]
    A --> C["vertex_degree_stats(hg, logger?)<br/>-> DistributionStats"]
    A --> D["hyperedge_size_histogram(hg)<br/>-> map&lt;pin_count, count&gt;"]
    A --> E["hyperedge_size_stats(hg, logger?)<br/>-> DistributionStats"]
    A --> F["high_fanout_nets(hg, threshold)<br/>-> vector&lt;HyperedgeId&gt;"]
```

## `congestion_metrics.cpp` — implementation

Three private helpers do the real work; the six public functions are thin
wrappers over them.

```mermaid
graph TD
    A["csrSliceSizes(offsets)"] --> B["for i in [0, offsets.size()-1):<br/>sizes[i] = offsets[i+1] - offsets[i]"]
    B --> C["returns per-vertex degree (from vertexOffsets())<br/>or per-hyperedge size (from hyperedgeOffsets())"]

    D["histogramOf(values)"] --> E["++histogram[v] for each v in values"]

    F["computeStats(values, logger, what)"] --> G{"values.empty()?"}
    G -- yes --> H["return DistributionStats{} (all zero)"]
    G -- no --> I["sum, max via one linear scan"]
    I --> J["p90_idx = percentileIndex(n, 0.90);<br/>nth_element to that index; read values[p90_idx]"]
    J --> K["p99_idx = percentileIndex(n, 0.99);<br/>nth_element to that index; read values[p99_idx]"]
    K --> L{"logger != nullptr?"}
    L -- yes --> M["debugPrint(group='hg_metrics', level=kVerbosityTrace,<br/>'{what}: mean=.. p99=.. max=..')"]
    L -- no --> N["return stats"]
    M --> N
```

`percentileIndex(n, p)` is the nearest-rank position in a 0-indexed sorted
array of size `n`: `floor(p * (n - 1))`, clamped into `[0, n - 1]`.

### Function group: vertex degree

```mermaid
graph TD
    A["vertex_degree_histogram(hg)"] --> B["csrSliceSizes(hg.vertexOffsets())<br/>-> per-vertex degree"]
    B --> C["histogramOf(degrees)<br/>-> map&lt;degree, vertex_count&gt;"]

    D["vertex_degree_stats(hg, logger)"] --> E["csrSliceSizes(hg.vertexOffsets())<br/>-> per-vertex degree"]
    E --> F["computeStats(degrees, logger, 'vertex_degree_stats')<br/>-> DistributionStats"]
```

### Function group: hyperedge size (fanout)

```mermaid
graph TD
    A["hyperedge_size_histogram(hg)"] --> B["csrSliceSizes(hg.hyperedgeOffsets())<br/>-> per-hyperedge pin count"]
    B --> C["histogramOf(sizes)<br/>-> map&lt;pin_count, hyperedge_count&gt;"]

    D["hyperedge_size_stats(hg, logger)"] --> E["csrSliceSizes(hg.hyperedgeOffsets())<br/>-> per-hyperedge pin count"]
    E --> F["computeStats(sizes, logger, 'hyperedge_size_stats')<br/>-> DistributionStats"]
```

### Function group: high-fanout nets

```mermaid
graph TD
    A["high_fanout_nets(hg, threshold)"] --> B["for e in [0, numHyperedges()):<br/>size = offsets[e+1] - offsets[e]"]
    B --> C{"size >= threshold?"}
    C -- yes --> D["result.push_back(e)"]
    C -- no --> E["skip"]
    D --> B
    E --> B
    B --> F["return result: vector&lt;HyperedgeId&gt;"]
```

## `timing_metrics.h` / `timing_metrics.cpp` — stub

`timing_metrics.h` only pulls in `congestion_metrics.h` for the shared
`DistributionStats` type and declares no functions yet (`TODO` marker for a
later spike brief, T0–T4). `timing_metrics.cpp` includes the header and
compiles to an empty translation unit. Both exist purely so the CMake
target and build are complete from day one.

## Module-level: how a caller uses this

```mermaid
sequenceDiagram
    participant Caller
    participant hg_metrics
    participant Hypergraph

    Caller->>Hypergraph: buildFromBlock(block) or buildFromTopology(...)
    Caller->>hg_metrics: vertex_degree_stats(hg, logger)
    hg_metrics->>Hypergraph: vertexOffsets()
    hg_metrics-->>Caller: DistributionStats
    Caller->>hg_metrics: hyperedge_size_histogram(hg)
    hg_metrics->>Hypergraph: hyperedgeOffsets()
    hg_metrics-->>Caller: map<pin_count, hyperedge_count>
    Caller->>hg_metrics: high_fanout_nets(hg, threshold)
    hg_metrics->>Hypergraph: numHyperedges(), hyperedgeOffsets()
    hg_metrics-->>Caller: vector<HyperedgeId>
```

The hypergraph is never mutated and no attribute planes are read or
written — every arrow above is a read of the existing CSR arrays.
