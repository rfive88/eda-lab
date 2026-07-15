# Flow: hg_metrics

`src/hg_metrics/` computes metrics over an `eda::Hypergraph`'s CSR topology.
Spike C1 implements the congestion metric group — vertex degree distribution,
hyperedge size (fanout) distribution, and high-fanout net identification —
all read-only; Spike C2 adds `k_core_numbers`, which reads the same CSR
arrays but writes a structural-centrality result into the `"hgm.k_core"`
attribute plane. Spike C3 adds three label-weighted neighborhood metrics —
`neighborhood_density` (a NESS propagation BFS), `one_hop_neighborhood_size`,
and `net_intersection_score` — each reading the CSR arrays and writing its own
per-vertex `"hgm."` plane. A stub `timing_metrics.h/.cpp` keeps the build
complete for a later brief.

## `congestion_metrics.h` — API contract

Declares `DistributionStats` (`mean`, `p90`, `p99`, `max`; shared with
`timing_metrics.h`), the `HyperedgeId` alias (a local hyperedge index — this
module has no dedicated stable id type, only the snapshot-local CSR index),
the five read-only distribution functions —
`vertex_degree_histogram`/`vertex_degree_stats`,
`hyperedge_size_histogram`/`hyperedge_size_stats`, `high_fanout_nets` — and
(Spike C2) `k_core_numbers` and (Spike C3) `neighborhood_density`,
`one_hop_neighborhood_size`, `net_intersection_score` — the plane-writing
functions that take the hypergraph by non-const reference.

```mermaid
graph TD
    A["const eda::Hypergraph& hg"] --> B["vertex_degree_histogram(hg)<br/>-> map&lt;degree, count&gt;"]
    A --> C["vertex_degree_stats(hg, logger?)<br/>-> DistributionStats"]
    A --> D["hyperedge_size_histogram(hg)<br/>-> map&lt;pin_count, count&gt;"]
    A --> E["hyperedge_size_stats(hg, logger?)<br/>-> DistributionStats"]
    A --> F["high_fanout_nets(hg, threshold)<br/>-> vector&lt;HyperedgeId&gt;"]
    G["eda::Hypergraph& hg (mutable)"] --> H["k_core_numbers(hg, logger?)<br/>writes 'hgm.k_core' plane<br/>-> degeneracy (max k-core)"]
    G --> I["neighborhood_density(hg, alpha, h)<br/>writes 'hgm.neighborhood_density' (double)"]
    G --> J["one_hop_neighborhood_size(hg)<br/>writes 'hgm.neighborhood_size_1hop' (int)"]
    G --> K["net_intersection_score(hg, logger?)<br/>writes 'hgm.net_intersection_score' (int)"]
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

### Function group: NESS neighborhood density (Spike C3)

Adjacency here is the hypergraph relation — two vertices are one hop apart iff
they share a hyperedge, so a hop expands `v -> every member of every
hyperedge incident to v` (`vertexPinList()` then `pinList()`). `degree[v]` is
the C1 incident-hyperedge count (`vertexOffsets()` slice length), precomputed
once by `vertexDegrees` and reused as the NESS label weight.

`propagate_neighborhood` (static, not in the header) is the shared BFS core of
`neighborhood_density`. It runs one BFS per source `u`, accumulating decayed
neighbor degree into `out[u]`. A monotonic `stamp[w] = u` marks "visited by
source u at its shortest distance", so no per-source `O(n)` clear of the
marker array is needed; `dist[w]` carries each visited vertex's BFS depth.

```mermaid
graph TD
    A["propagate_neighborhood(hg, alpha, h, degree, out)"] --> B["alpha_pow[i] = alpha^i, i in [0,h]"]
    B --> C["for each source u in [0, n):"]
    C --> D["stamp[u]=u; dist[u]=0; queue={u}; acc=0"]
    D --> E{"queue empty?"}
    E -- yes --> F["out[u] = acc"]
    E -- no --> G["v = pop_front; dv = dist[v]"]
    G --> H{"dv >= h?"}
    H -- yes --> E
    H -- no --> I["for each incident edge e of v,<br/>for each member w of e:"]
    I --> J{"stamp[w] == u?<br/>(u itself or already visited)"}
    J -- yes --> I
    J -- no --> K["stamp[w]=u; dist[w]=dv+1;<br/>acc += alpha_pow[dv+1] * degree[w];<br/>queue.push_back(w)"]
    K --> I
    I --> E
    F --> C
```

`neighborhood_density(hg, alpha, h)` writes/zeroes the
`"hgm.neighborhood_density"` double plane, short-circuits to all-zero when
`numVertices()==0` or `h<=0` (and `alpha==0` naturally zeroes every term via
`alpha_pow`), then calls `propagate_neighborhood`.

```mermaid
graph TD
    A["neighborhood_density(hg, alpha, h)"] --> B["density = vertexDoublePlane('hgm.neighborhood_density'); fill 0"]
    B --> C{"numVertices()==0 or h<=0?"}
    C -- yes --> D["return (plane all-zero)"]
    C -- no --> E["degree = vertexDegrees(hg)"]
    E --> F["propagate_neighborhood(hg, alpha, h, degree, density)"]
```

`one_hop_neighborhood_size(hg)` is a single CSR pass, no BFS: for each `u`
walk its incident edges and their members, epoch-stamp each distinct neighbor
`w != u` once, and write the count to the `"hgm.neighborhood_size_1hop"` int
plane.

```mermaid
graph TD
    A["one_hop_neighborhood_size(hg)"] --> B["size = vertexIntPlane('hgm.neighborhood_size_1hop'); fill 0"]
    B --> C["for each u: count=0"]
    C --> D["for each incident edge e, each member w of e:"]
    D --> E{"w==u or stamp[w]==u?"}
    E -- yes --> D
    E -- no --> F["stamp[w]=u; ++count"]
    F --> D
    D --> G["size[u] = count"]
    G --> C
```

`net_intersection_score(hg, logger?)` sums, over each vertex `u`'s unordered
pairs of incident hyperedges `(e1,e2)`, `|V(e1) ∩ V(e2)| - 1` (discounting
`u`). A monotonic `epoch` stamps `e1`'s members, then `e2`'s members are
counted against that stamp — no per-pair set allocation or clear. A degree
above `kHighDegreeWarnThreshold` (64) triggers a `warn` (id 130) when a logger
is attached, since the inner pair loop is quadratic in degree.

```mermaid
graph TD
    A["net_intersection_score(hg, logger?)"] --> B["score = vertexIntPlane('hgm.net_intersection_score'); fill 0"]
    B --> C["for each u: deg = incident edge count"]
    C --> D{"logger && deg > 64?"}
    D -- yes --> E["warn(UKN, 130, 'vertex may be slow')"]
    D -- no --> F["for each incident edge e1 (index a):"]
    E --> F
    F --> G["++epoch; stamp all members of e1 with epoch"]
    G --> H["for each incident edge e2 (index b>a):"]
    H --> I["shared = count members of e2 with stamp==epoch"]
    I --> J["total += shared - 1"]
    J --> H
    H --> F
    F --> K["score[u] = total"]
    K --> C
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
    Caller->>hg_metrics: k_core_numbers(hg, logger)
    hg_metrics->>Hypergraph: vertexOffsets()/vertexPinList()/hyperedgeOffsets()/pinList()
    hg_metrics->>Hypergraph: vertexIntPlane("hgm.k_core") (write)
    hg_metrics-->>Caller: degeneracy
    Caller->>hg_metrics: neighborhood_density(hg, alpha, h)
    hg_metrics->>Hypergraph: CSR arrays (read) + vertexDoublePlane("hgm.neighborhood_density") (write)
    Caller->>hg_metrics: one_hop_neighborhood_size(hg)
    hg_metrics->>Hypergraph: CSR arrays (read) + vertexIntPlane("hgm.neighborhood_size_1hop") (write)
    Caller->>hg_metrics: net_intersection_score(hg, logger)
    hg_metrics->>Hypergraph: CSR arrays (read) + vertexIntPlane("hgm.net_intersection_score") (write)
```

The distribution functions never mutate the hypergraph — every arrow for
them is a read of the existing CSR arrays. `k_core_numbers` and the three C3
neighborhood functions are the exceptions: each reads the same CSR arrays but
*writes* one `"hgm."` attribute plane as its only side effect; the CSR
topology is untouched.
