# Flow: Partitioning Engine

The partitioning engine (`src/engines/partitioning/`) is a flat, no-coarsening
K-way Fiduccia–Mattheyses (FM) partitioner: `partitionFM()` splits an
`eda::Hypergraph`'s vertices into `num_parts` parts to minimize the weighted
**connectivity-1** objective — for hyperedge `e` with λ(e) distinct parts
touching it, `cut_cost = Σ weight(e) × (λ(e) − 1)` — subject to a per-part
vertex-weight balance window. For `num_parts == 2` this collapses to the
Stage 1 weighted spanning cut; the same code path (not a separate
implementation) handles both, which is why there is one FM source file, not
two. `random_hypergraph.h/.cpp` is a companion test-input generator, unrelated
to the FM algorithm itself. This document covers Stages 1–2 as they exist
today (flat FM only; no multilevel coarsening).

## `fm_partitioner.h` — API contract

Declares the pure-library surface: `FMParams` (K, `balance_tolerance`,
`max_passes`, `seed`, `initial` ∈ {`kRandom`, `kProvided`}, optional
`logger`), `FMResult` (`partition`, `cut_cost`, `passes_run`, `balanced`), and
the entry point `partitionFM(hg, params, initial_partition)`. No logic lives
here — it exists so later stages (multilevel driver, CLI/Python/GUI) can call
this API without depending on `fm_partitioner.cpp`'s internals.

```mermaid
graph TD
    A["const Hypergraph& hg"] --> D["partitionFM(hg, params, initial_partition)"]
    B["FMParams (num_parts, balance_tolerance,<br/>max_passes, seed, initial, logger)"] --> D
    C["initial_partition (optional, used only<br/>when params.initial == kProvided)"] --> D
    D --> E["FMResult (partition, cut_cost,<br/>passes_run, balanced)"]
```

## `fm_partitioner.cpp` — FM engine implementation

All logic lives in the anonymous-namespace class `FMEngine`, constructed with
`(hg, params)` and driven by `FMEngine::run()`.

### Setup: `run()` entry sequence

`run()` reads `hg.numVertices()`/`numHyperedges()` into `n_`/`m_`, then calls
four setup steps before the pass loop. When a `logger` is attached, `run()`
and `runPass()` emit **debug-gated** trace (`debugPrint`, group `"fm"`): run
setup + convergence + final at level 1, per-pass cut deltas at level 1, and
per-move gains at level 3 (capped). Nothing prints at verbosity 0, preserving
the pure-library "nothing prints" contract — see the README's verbosity note.

```mermaid
graph TD
    A["FMEngine::run(initial)"] --> B{"n_ == 0?"}
    B -- yes --> Z["return trivial FMResult<br/>(balanced = true)"]
    B -- no --> C["loadWeights():<br/>read 'area' vertex plane -> vertex_weight_,<br/>read 'weight' hyperedge plane -> edge_weight_,<br/>compute min_part_/max_part_ from total_weight_"]
    C --> D["buildIncidence():<br/>build distinct (edge, multiplicity) CSR per vertex<br/>and (vertex, multiplicity) CSR per hyperedge"]
    D --> E["setInitialPartition(initial):<br/>kProvided + valid -> use it;<br/>else seeded balanced random assignment;<br/>fill part_weight_"]
    E --> F["pass loop (max_passes times):<br/>call runPass(), stop early if it returns false"]
    F --> G["computeCountsAndCut(): fresh cnt_/lambda_/cut_<br/>from the final part_"]
    G --> H["FMResult{partition=part_, cut_cost=cut_,<br/>passes_run, balanced=isBalanced()}"]
```

`buildIncidence()` deserves its own note: the vertex-major CSR
(`vertexOffsets()`/`vertexPinList()`) is already sorted per vertex by the
`Hypergraph` build guarantee, so a single run-length pass collapses repeated
`(vertex, edge)` pins into `(edge, multiplicity)` entries (`v_edge_`,
`v_mult_`). The hyperedge-major CSR (`hyperedgeOffsets()`/`pinList()`) follows
`dbITerm` order, not sorted, so each edge's member list is copied and sorted
before the same run-length collapse produces `e_vert_`/`e_mult_`. Sorting also
fixes a deterministic per-edge member order used later when re-deriving
neighbor gains.

### Connectivity-1 objective: `cnt_`/`lambda_` and incremental updates

The core state is one pin count per (hyperedge, part) — `cnt_[e*k_+p]` — and
the derived connectivity `lambda_[e]` (number of parts with `cnt_[e*k_+p] >
0`). `computeCountsAndCut()` builds both from scratch by scanning each edge's
pins once. `applyMove()` (called when a vertex actually moves) touches only
the `from`/`to` slots of each incident edge and adjusts `lambda_`/`cut_` in
O(1) from the before/after slot values — no recount.

```mermaid
graph TD
    A["computeCountsAndCut(): for each edge e,<br/>count pins per part -> cnt_[e*k_+p]"] --> B["lambda_[e] = number of parts p with cnt_[e*k_+p] > 0"]
    B --> C["cut_ = sum over e of edge_weight_[e] * (lambda_[e]-1)<br/>when lambda_[e] > 1"]

    D["applyMove(v, to): vertex v moves from part 'from'"] --> E["for each incident edge e of v (with multiplicity mult):<br/>oc_from=cnt_[e][from], oc_to=cnt_[e][to]<br/>nc_from=oc_from-mult, nc_to=oc_to+mult"]
    E --> F{"nc_from == 0? / oc_to == 0?"}
    F --> G["lambda_[e] adjusted by -1 / +1 accordingly;<br/>cut_ += edge_weight_[e] * (lambda_new - lambda_old)"]
    G --> H["cnt_[e][from]=nc_from, cnt_[e][to]=nc_to"]
```

Gain values are derived from these same counts. With `n_p(e)` = pins of `e` in
part `p` and vertex `u` holding `mult(u,e)` pins of `e`, `vertexGain(v, to)`
sums, over `v`'s distinct incident edges, `edge_weight_[e]` once if moving `v`
empties its source part (`cnt_[e][from] == mult`) and subtracts it once if the
move newly occupies the target (`cnt_[e][to] == 0`). This one formula replaces
the textbook T(e)==0/1 case split, and is valid for any K and any pin
multiplicity per vertex.

### Gain data structure: heap with lazy deletion

Because hyperedge weights are doubles, gains cannot use an integer bucket
list. Instead there is one max-heap (`GainHeap`, a `std::priority_queue` of
`HeapEntry` ordered by `EntryOrder`) per **source part**, holding one entry
per `(vertex, target part)` candidate move. A vertex in part `p` has `K-1`
live candidates, all in heap `p`. `gain_[v*k_+t]` and `stamp_[v*k_+t]` hold the
current true gain and a version stamp; every time a gain changes, `applyMove`
bumps the stamp and pushes a fresh `HeapEntry` — older heap entries for that
`(v,t)` become stale and are dropped, not erased, when popped.

```mermaid
graph TD
    A["gain_[v*k_+t] changes (in applyMove, via a<br/>neighbor's re-derived contribution)"] --> B["stamp_[v*k_+t]++"]
    B --> C["heaps[part_[v]].push(HeapEntry{gain_[v*k_+t], v, t, stamp_[v*k_+t]})"]

    D["Selection: pop top of heaps[side]"] --> E{"entry.stamp == stamp_[v*k_+t]<br/>and not locked_[v]?"}
    E -- no, stale/locked --> D
    E -- yes, alive --> F{"moveIsAcceptable(v,t)?"}
    F -- no --> G["push to 'deferred', keep scanning heap"]
    G --> D
    F -- yes --> H["candidate for this side; stop scanning this heap"]
```

`EntryOrder` breaks gain ties toward the smaller vertex index, then the
smaller target part, so the final selection across all heaps (see below) is a
pure function of the gain values — required for the determinism contract.

### FM pass: `runPass()` candidate selection and rollback

Each pass rebuilds counts, gains, and heaps from scratch (locks cleared), then
repeats "pick best legal move, apply it, track best-seen state" until no
vertex can move.

```mermaid
graph TD
    A["runPass() start:<br/>computeCountsAndCut(); clear locked_;<br/>for every (v,t) with t != part_[v]:<br/>gain_[v*k_+t]=vertexGain(v,t), push into heaps[part_[v]]"] --> B["best_prefix=0; best_cut=cut_;<br/>best_balanced=isBalanced(); best_infeas=infeasibility()"]
    B --> C{"step < n_ ?"}
    C -- no --> R["Roll back part_/part_weight_<br/>to the best_prefix move count"]
    R --> S["cut_ = best_cut; return best_prefix > 0"]
    C -- yes --> D["For each source part, scan its heap (skip dead/locked<br/>entries) for the first move where moveIsAcceptable() holds<br/>-> that part's local candidate, if any"]
    D --> E{"any part produced a candidate?"}
    E -- no --> R
    E -- yes --> F["Across parts' candidates, pick max gain<br/>(ties: smaller vertex, then smaller target)"]
    F --> G["Restore scanned-past ('deferred') entries<br/>and losing candidates to their heaps"]
    G --> H["applyMove(v, target): lock v, update cnt_/lambda_/cut_,<br/>re-derive and push neighbor gains"]
    H --> I{"new state better than best-prefix state?<br/>(lexicographic: balanced > lower cut;<br/>unbalanced > lower infeasibility)"}
    I -- yes --> J["best_prefix = moves so far; record<br/>best_cut/best_balanced/best_infeas"]
    I -- no --> C
    J --> C
```

Moves are applied even when the chosen gain is negative — classic FM — and
`best_prefix` rollback at pass end discards the tail of moves after the last
improvement, which is how a pass escapes local minima. `moveIsAcceptable()`
accepts any move that keeps both endpoints' part weights inside
`[min_part_, max_part_]`; if the current state is already infeasible (only
reachable from a bad `kProvided` initial partition), it additionally accepts
moves that strictly reduce total `infeasibility()`, letting FM walk back to
feasibility instead of freezing.

## `random_hypergraph.h` / `random_hypergraph.cpp` — test input generator

`generateRandomHypergraph(params)` builds a dbBlock-free `Hypergraph` via
`Hypergraph::buildFromTopology`, independent of the FM algorithm — it exists
to give engine tests controlled-size, controlled-degree inputs without
LEF/DEF or `netlistgen`'s pin bookkeeping.

```mermaid
graph TD
    A["generateRandomHypergraph(params)"] --> B["n = max(params.num_vertices, 0)"]
    B --> C{"n >= 2 and num_hyperedges > 0?"}
    C -- no --> H["edges = {} (no hyperedges)"]
    C -- yes --> D["clamp [min_degree, max_degree] into [2, n]"]
    D --> E["draw degree in [lo, hi] via drawInRange(rng)<br/>for the next hyperedge e"]
    E --> F["rejection-sample 'degree' distinct vertices:<br/>draw v in [0,n), accept if stamp[v] != e,<br/>mark stamp[v] = e"]
    F --> G["edges.push_back(edge)"]
    G --> J{"num_hyperedges edges built yet?"}
    J -- no --> E
    J -- yes --> I["Hypergraph hg; hg.buildFromTopology(n, edges); return hg"]
    H --> I
```

All randomness is raw `std::mt19937` output mapped by modulo (`drawInRange`),
never `std::uniform_int_distribution`, so a given `(params, seed)` produces a
bit-identical hypergraph on every platform — the same determinism discipline
`fm_partitioner.cpp`'s `drawBelow` follows for the FM initial partition.

## Engine-level: end-to-end algorithm

```mermaid
sequenceDiagram
    participant Caller
    participant partitionFM
    participant FMEngine
    participant Hypergraph

    Caller->>partitionFM: partitionFM(hg, params, initial_partition)
    partitionFM->>FMEngine: FMEngine(hg, params).run(initial_partition)
    FMEngine->>Hypergraph: numVertices(), numHyperedges()
    FMEngine->>Hypergraph: findVertexDoublePlane("area"), findHyperedgeDoublePlane("weight")
    FMEngine->>Hypergraph: vertexOffsets()/vertexPinList(), hyperedgeOffsets()/pinList()
    FMEngine->>FMEngine: setInitialPartition(initial_partition)
    loop up to max_passes, stop early if a pass makes no move
        FMEngine->>FMEngine: runPass()
    end
    FMEngine->>FMEngine: computeCountsAndCut() (final, fresh)
    FMEngine-->>partitionFM: FMResult
    partitionFM-->>Caller: FMResult
```

```mermaid
graph TD
    A["Vertices as CSR incidence (buildIncidence)"] --> B["Initial partition<br/>(seeded random or provided)"]
    B --> C["cnt_[e][p] / lambda_[e] / cut_<br/>(computeCountsAndCut)"]
    C --> D["Per-(vertex,target) gains in<br/>per-source-part max-heaps"]
    D --> E["runPass: repeatedly apply the best<br/>legal move, track best-seen state"]
    E --> F["Roll back to best-seen prefix"]
    F --> G{"pass improved cut<br/>and passes remain?"}
    G -- yes --> C
    G -- no --> H["FMResult: partition, cut_cost,<br/>passes_run, balanced"]
```

For `num_parts == 2`, `lambda_[e]` is only ever 1 or 2, `cut_` reduces exactly
to the Stage 1 weighted spanning cut, and every floating-point operation above
runs in the same order as the Stage 1 implementation — this is what makes the
K = 2 path reproduce Stage 1 results bit-for-bit.
