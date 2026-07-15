# Flow: hg_metrics

`src/hg_metrics/` computes metrics over an `eda::Hypergraph`'s CSR topology.
Spike C1 implements the congestion metric group — vertex degree distribution,
hyperedge size (fanout) distribution, and high-fanout net identification —
all read-only; Spike C2 adds `k_core_numbers`, which reads the same CSR
arrays but writes a structural-centrality result into the `"hgm.k_core"`
attribute plane. A stub `timing_metrics.h/.cpp` keeps the build complete for
a later brief.

## `congestion_metrics.h` — API contract

Declares `DistributionStats` (`mean`, `p90`, `p99`, `max`; shared with
`timing_metrics.h`), the `HyperedgeId` alias (a local hyperedge index — this
module has no dedicated stable id type, only the snapshot-local CSR index),
the five read-only distribution functions —
`vertex_degree_histogram`/`vertex_degree_stats`,
`hyperedge_size_histogram`/`hyperedge_size_stats`, `high_fanout_nets` — and
(Spike C2) `k_core_numbers`, the one function here that takes the hypergraph
by non-const reference because it writes the `"hgm.k_core"` int plane.

```mermaid
graph TD
    A["const eda::Hypergraph& hg"] --> B["vertex_degree_histogram(hg)<br/>-> map&lt;degree, count&gt;"]
    A --> C["vertex_degree_stats(hg, logger?)<br/>-> DistributionStats"]
    A --> D["hyperedge_size_histogram(hg)<br/>-> map&lt;pin_count, count&gt;"]
    A --> E["hyperedge_size_stats(hg, logger?)<br/>-> DistributionStats"]
    A --> F["high_fanout_nets(hg, threshold)<br/>-> vector&lt;HyperedgeId&gt;"]
    G["eda::Hypergraph& hg (mutable)"] --> H["k_core_numbers(hg, logger?)<br/>writes 'hgm.k_core' plane<br/>-> degeneracy (max k-core)"]
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

### Function group: k-core decomposition

`k_core_numbers(hg, logger?)` peels the hypergraph in non-decreasing
effective-degree order and writes each vertex's core number into the
`"hgm.k_core"` int plane. Effective degree counts a vertex's incident
hyperedges that still have >= 2 active members; a hyperedge stops
contributing degree the instant it drops to a single survivor. The
degeneracy (max core number) is returned. The bucket queue
(`std::vector<std::list<int>>` indexed by degree, one `node[v]` iterator per
vertex) gives O(1) degree-update removal, so the whole peel is O(n + pins).

```mermaid
graph TD
    A["k_core_numbers(hg, logger?)"] --> B["k_core = hg.vertexIntPlane('hgm.k_core');<br/>fill 0 (create or overwrite)"]
    B --> C{"numVertices() == 0?"}
    C -- yes --> Z["return 0"]
    C -- no --> D["active_count[e] = pin count of e"]
    D --> E["cur_deg[v] = #incident edges with active_count >= 2;<br/>max_deg = max over v"]
    E --> F["buckets[cur_deg[v]].push_front(v);<br/>node[v] = iterator"]
    F --> G["for d = 0 .. max_deg:"]
    G --> H{"buckets[d] empty?"}
    H -- yes --> G
    H -- no --> I["v = pop_front(buckets[d]);<br/>removed[v]=1; k_core[v]=d;<br/>degeneracy = max(degeneracy, d)"]
    I --> J["for each incident edge e of v"]
    J --> K{"active_count[e] < 2?"}
    K -- yes --> J
    K -- no --> L["--active_count[e]"]
    L --> M{"active_count[e] == 1?"}
    M -- no --> J
    M -- yes --> N["u = lone non-removed member of e"]
    N --> O["new_deg = max(cur_deg[u]-1, d);<br/>move u to buckets[new_deg] via node[u]"]
    O --> J
    J --> H
    G --> P["logger? debugPrint level 2:<br/>degeneracy / mean / max"]
    P --> Q["return degeneracy"]
```

The `max(cur_deg[u]-1, d)` clamp is the degeneracy-ordering invariant:
everything with core number below the current level `d` is already peeled, so
a survivor whose remaining degree dips under `d` still takes core number `d`.
`removed[]`, `cur_deg[]`, `active_count[]`, and the buckets are all local to
the call — the hypergraph's CSR structure is never mutated, only the output
plane is written.

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
    Caller->>hg_metrics: k_core_numbers(hg, logger)
    hg_metrics->>Hypergraph: vertexOffsets()/vertexPinList()/hyperedgeOffsets()/pinList()
    hg_metrics->>Hypergraph: vertexIntPlane("hgm.k_core") (write)
    hg_metrics-->>Caller: degeneracy
```

The distribution functions never mutate the hypergraph — every arrow for
them is a read of the existing CSR arrays. `k_core_numbers` is the one
exception: it reads the same CSR arrays but *writes* the `"hgm.k_core"` int
attribute plane (its only side effect; the CSR topology is untouched).
