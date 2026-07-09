# src/engines/partitioning/

Netlist partitioning engine. **Stages 1–2** of a planned multilevel K-way
multi-objective partitioner: what exists today is a flat (no coarsening)
K-way Fiduccia–Mattheyses partitioner with the weighted connectivity-1
(λ−1) objective, plus a procedural random hypergraph generator used to
test it. Multilevel coarsening, multi-objective blending, and
CLI/Python/GUI wrappers are future stages; the pure-library API here
(const hypergraph in, plain result struct out, no I/O, no exceptions) is
shaped so those can layer on top without changes.

Reference papers (hMETIS, TritonPart, FM 1982) are cataloged in
`docs/README.md`; the PDFs themselves are gitignored — fetch them per the
instructions there.

## What the engine does

`partitionFM()` (`fm_partitioner.h`) splits the vertices of an
`eda::Hypergraph` into `num_parts` parts, minimizing the **connectivity-1
cost**: with λ(e) = number of distinct parts holding at least one pin of
hyperedge e,

    cut_cost = Σ over hyperedges e of weight(e) × (λ(e) − 1)

so an uncut net costs 0, a net spanning two parts costs its weight, three
parts twice its weight, and so on — the standard objective for minimizing
total inter-part communication. For `num_parts == 2`, λ is 1 or 2 and
this is exactly the Stage 1 weighted spanning cut; the K = 2 path
performs the same floating-point operations as the Stage 1 implementation
and reproduces its results bit-for-bit (verified against captured Stage 1
outputs when Stage 2 landed). All of this is subject to a vertex-weight
balance constraint on every part. Classic FM mechanics:

- Repeated passes; within a pass every vertex moves at most once (moved
  vertices are locked), always the highest-gain acceptable move across
  all (vertex, target part) candidates, even when that gain is negative.
- The best state seen during the pass is restored at pass end
  (best-prefix rollback), which is how a pass escapes local minima that
  greedy improvement cannot.
- Passes stop early when one fails to improve, or after `max_passes`.

The core data structure is a per-hyperedge, per-part pin count
(`cnt[e][p]`) with the derived λ(e) maintained incrementally: moving a
vertex touches only the source and destination slots of each incident
edge, so cut deltas and gain updates are O(affected), never a recount.
Gain selection uses one max-heap per source part, one entry per
(vertex, target part), keyed by the exact double gain with lazy deletion
(stamped entries), rather than the classic integer-keyed bucket list —
hyperedge weights are doubles, and the heap avoids a quantization knob;
see the tradeoff comment atop `fm_partitioner.cpp`. Gain updates use an
exact count-based formula that is valid when a vertex holds several pins
of one hyperedge (the one-entry-per-`dbITerm` case) and any K, also
documented there.

Balance: each part's vertex weight must stay within
`[(1 − t)·W/K, (1 + t)·W/K]` for tolerance `t` and total weight `W`. A
move that would leave tolerance is never accepted from a balanced state.
If a *provided* initial partition is unbalanced (including parts left
empty), only moves that strictly shrink the total distance to the
feasible region are accepted until balance is restored.

Determinism: same hypergraph + same params ⇒ identical `FMResult`, every
run and platform (seeded `std::mt19937` raw draws, stable tie-breaking on
vertex index then target part, fixed-order floating-point sums; IEEE-754
assumed).

## Input contract (attribute planes)

| Plane | Kind | Type | Read/Write | Meaning |
|---|---|---|---|---|
| `weight` | hyperedge | double | read (optional) | cut cost per hyperedge; 1.0 per edge when absent |
| `area` | vertex | double | read (optional) | balance weight per vertex; 1.0 per vertex when absent |

The engine writes no planes: results come back in `FMResult` (the engine
keeps the hypergraph strictly `const`; a later stage may adopt the
write-back-to-plane convention). Weights are read through the `const`
probes `findHyperedgeDoublePlane("weight")` / `findVertexDoublePlane("area")`,
so a plane bound to a non-double type is treated as absent. Weights are
expected to be positive; the engine does not sanitize them.

## Control parameters

`FMParams` (defaults in `fm_partitioner.h`):

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `num_parts` | `int` | `2` | number of parts K. Values below 1 are clamped to 1 (trivial single-part solution: all vertices in part 0, cost 0). |
| `balance_tolerance` | `double` | `0.10` | max fractional deviation of any part's weight from W/K. Must comfortably exceed the heaviest single vertex's share of W/K, or no single move is ever feasible and FM cannot leave the initial partition (relevant on tiny graphs — see the tolerance notes in the tests). |
| `max_passes` | `int` | `10` | outer FM passes; stops early on a non-improving pass |
| `seed` | `unsigned` | `1` | seeds the random initial partition and all tie-breaking |
| `initial` | `InitialPartition` | `kRandom` | `kRandom`: seeded balanced random split (random visit order, greedy lightest-part placement, ties to the smallest part index). `kProvided`: take `partitionFM`'s `initial_partition` argument (must be `numVertices()` entries in `[0, num_parts)`; anything else falls back to `kRandom` with a UKN-0102 warning). |
| `logger` | `utl::Logger*` | `nullptr` | optional; debug-level trace only (tool `UKN`, debug group `"fm"`) plus the UKN-0102 warning above |

`FMResult`: `partition` (local vertex index → part in `[0, K)`),
`cut_cost` (final connectivity-1 cost, recomputed from scratch at
return), `passes_run`, `balanced` (whether the final solution meets the
balance constraint).

## Random hypergraph generator

`generateRandomHypergraph()` (`random_hypergraph.h`) produces a
dbBlock-free `Hypergraph` (via `Hypergraph::buildFromTopology`) from
`RandomHypergraphParams`: `num_vertices`, `num_hyperedges`, pins per
hyperedge uniform in `[min_degree, max_degree]` (clamped to
`[2, num_vertices]`), `seed`. Vertices within a hyperedge are distinct.
Same params ⇒ bit-identical topology on every platform: all randomness is
raw `std::mt19937` output mapped with modulo (never
`std::uniform_int_distribution`, whose algorithm is
implementation-defined); the negligible modulo bias is documented in the
source. In generated graphs, `vertexId()`/`hyperedgeId()` return the
invalid id — see "Procedural mode" in `src/hypergraph/README.md`.

## How to run

Library only — no CLI yet (future stage). Tests:

```bash
cmake --build build
ctest --test-dir build -R fm_partitioner_test --output-on-failure
# or, for a manual run (outputs, if any ever appear, land in run/):
cd run && ../build/fm_partitioner_test
```

`test/fm_partitioner_test.cpp` covers, for K = 2 (Stage 1 behavior):
generator determinism and validity; FM determinism, balance, and
improvement over a topology-blind initial on random hypergraphs;
recovery of a known-optimal cut (two 4-cliques joined by a bridge); the
weighted objective following the `weight` plane as the cheap edges rotate
around an 8-cycle; and balance + determinism + cut improvement on the
Nangate45 gcd design. For K > 2 (Stage 2): the objective being λ−1 and
not spanning cut (a 3-pin net pinned across 3 parts costs 2×weight);
recovery of a known-optimal 3-way split (three 4-cliques, two bridges);
determinism, balance, and reported-cost consistency for K ∈ {3, 4};
improvement over a striped initial; recovery from a provided initial
with empty parts; fallback on out-of-range provided values; K = 2
spanning-cut equivalence; the trivial K = 1 case; and K = 4 on the gcd
design. Plane and reporting honesty: balance following the vertex `area`
plane in a case where count-balance and area-balance disagree (only the
5+1 vs 6×1 area split is feasible); `balanced == false` reported when no
feasible partition exists (one vertex heavier than the per-part upper
bound); and golden cut-cost quality floors on fixed-seed random
hypergraphs for K ∈ {2, 4}, backed by an `expectInvariants` helper that
recomputes cost and area-weighted feasibility from scratch and checks
them against the reported `FMResult`.
