# src/engines/partitioning/

Netlist partitioning engine. **Stage 1** of a planned multilevel K-way
multi-objective partitioner: what exists today is a flat (no coarsening)
2-way Fiduccia–Mattheyses partitioner with a weighted cut objective, plus
a procedural random hypergraph generator used to test it. K > 2,
multilevel coarsening, multi-objective blending, and CLI/Python/GUI
wrappers are future stages; the pure-library API here (const hypergraph
in, plain result struct out, no I/O, no exceptions) is shaped so those can
layer on top without changes.

## What the engine does

`partitionFM()` (`fm_partitioner.h`) splits the vertices of an
`eda::Hypergraph` into parts 0 and 1, minimizing the **weighted cut** —
the sum of weights of hyperedges with pins in both parts — subject to a
vertex-weight balance constraint. Classic FM mechanics:

- Repeated passes; within a pass every vertex moves at most once (moved
  vertices are locked), always the highest-gain acceptable move, even
  when that gain is negative.
- The best state seen during the pass is restored at pass end
  (best-prefix rollback), which is how a pass escapes local minima that
  greedy improvement cannot.
- Passes stop early when one fails to improve, or after `max_passes`.

Gain selection uses one max-heap per side keyed by the exact double gain
with lazy deletion (stamped entries), rather than the classic
integer-keyed bucket list — hyperedge weights are doubles, and the heap
avoids a quantization knob; see the tradeoff comment atop
`fm_partitioner.cpp`. Gain updates use an exact count-based formula that
is valid when a vertex holds several pins of one hyperedge (the
one-entry-per-`dbITerm` case), also documented there.

Balance: each part's vertex weight must stay within
`[(1 − t)·W/2, (1 + t)·W/2]` for tolerance `t` and total weight `W`. A
move that would leave tolerance is never accepted from a balanced state.
If a *provided* initial partition is unbalanced, only
imbalance-reducing moves are accepted until balance is restored.

Determinism: same hypergraph + same params ⇒ identical `FMResult`, every
run and platform (seeded `std::mt19937` raw draws, stable tie-breaking on
vertex index, fixed-order floating-point sums; IEEE-754 assumed).

## Input contract (attribute planes)

| Plane | Kind | Type | Read/Write | Meaning |
|---|---|---|---|---|
| `weight` | hyperedge | double | read (optional) | cut cost per hyperedge; 1.0 per edge when absent |
| `area` | vertex | double | read (optional) | balance weight per vertex; 1.0 per vertex when absent |

The engine writes no planes: results come back in `FMResult` (Stage 1
keeps the hypergraph strictly `const`; a later stage may adopt the
write-back-to-plane convention). Weights are read through the `const`
probes `findHyperedgeDoublePlane("weight")` / `findVertexDoublePlane("area")`,
so a plane bound to a non-double type is treated as absent. Weights are
expected to be positive; the engine does not sanitize them.

## Control parameters

`FMParams` (defaults in `fm_partitioner.h`):

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `balance_tolerance` | `double` | `0.10` | max fractional deviation of either part's weight from W/2. Must comfortably exceed the heaviest single vertex's share of W/2, or no single move is ever feasible and FM cannot leave the initial partition (relevant on tiny graphs — see the tolerance notes in the tests). |
| `max_passes` | `int` | `10` | outer FM passes; stops early on a non-improving pass |
| `seed` | `unsigned` | `1` | seeds the random initial partition and all tie-breaking |
| `initial` | `InitialPartition` | `kRandom` | `kRandom`: seeded balanced random split (random visit order, greedy lighter-side placement). `kProvided`: take `partitionFM`'s `initial_partition` argument (must be `numVertices()` entries of 0/1; anything else falls back to `kRandom` with a UKN-0102 warning). |
| `logger` | `utl::Logger*` | `nullptr` | optional; debug-level trace only (tool `UKN`, debug group `"fm"`) plus the UKN-0102 warning above |

`FMResult`: `partition` (local vertex index → 0/1), `cut_cost` (final
weighted cut, recomputed from scratch at return), `passes_run`,
`balanced` (whether the final solution meets the balance constraint).

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

`test/fm_partitioner_test.cpp` covers: generator determinism and
validity; FM determinism, balance, and improvement over a topology-blind
initial on random hypergraphs; recovery of a known-optimal cut (two
4-cliques joined by a bridge); the weighted objective following the
`weight` plane as the cheap edges rotate around an 8-cycle; and balance +
determinism + cut improvement on the Nangate45 gcd design.
