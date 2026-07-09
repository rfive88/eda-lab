# test/

GTest suites for eda-lab.

## Build and run

```bash
cmake -B build              # Debug tree; build-release/ is the Release tree
cmake --build build
ctest --test-dir build -R "hypergraph_test|netlistgen_test|fm_partitioner_test" --output-on-failure
```

The `-R` filter matters: a bare `ctest` also picks up the vendored OpenROAD
integration tests, which need the never-built `openroad` binary and fail.

Each test file builds into its own executable (top-level `CMakeLists.txt`:
`add_executable` + `add_test` per suite, linked against `GTest::gtest` /
`GTest::gtest_main`). The suites that load real design data get
`EDA_LAB_DATA_DIR` compiled in, pointing at `data/`.

The test binaries can also be run directly. Manual runs execute from the
`run/` directory so any output files land there instead of the repo root
(standing convention in `CLAUDE.md`):

```bash
cd run
../build/hypergraph_test
../build/netlistgen_test
../build/fm_partitioner_test
```

## Test files

- `hypergraph_test.cpp` — the hypergraph netlist model
  (`src/hypergraph/`). Two independent test groups:
  - `HypergraphTest` (fixture) loads Nangate45 LEF + the gcd DEF once per
    suite (via `EDA_LAB_DATA_DIR`). Covers: empty-block build, id↔index
    round trips, CSR connectivity checked against `dbNet::getITerms()`,
    vertex-major/edge-major transpose consistency, inst/net count parity
    with `hello_odb` (734 components), rebuild idempotence, and the
    attribute-plane semantics (on-demand creation, persistence, type
    conflict warning + separate storage, removal, rebuild/`clear()`
    invalidation, vertex/hyperedge namespace independence).
  - `HypergraphTopologyTest` (plain `TEST`s, no LEF/DEF, no dbBlock) covers
    `buildFromTopology`: exact CSR contents including a repeated pin
    (multi-pin membership) and the transpose, soft-fail dbId lookups in
    dbBlock-free mode, plane sizing/invalidation on procedural graphs,
    bad-pin skipping with a logged warning, and the const
    `findVertexDoublePlane`/`findHyperedgeDoublePlane` probes (nullptr on
    absent or wrong-typed planes, never create).
- `netlistgen_test.cpp` — programmatic netlist construction
  (`src/netlistgen/`), no data files needed. A hand-built 3-inst/2-net
  netlist asserting exact hypergraph CSR contents in both directions
  (`NetlistBuilderTest.ExactTopology`), and `generateSynthetic` conformance
  on a 2000-inst netlist (fanout bounds, one driver per net, no pin reuse),
  net-count limiting, and seed determinism (`SyntheticNetlistTest`).
- `fm_partitioner_test.cpp` — the Stage 1–2 partitioning engine
  (`src/engines/partitioning/`): flat K-way FM minimizing weighted
  connectivity-1. Reported costs are cross-checked against independent
  reference evaluators defined in the test (`computeCut`,
  `computeConnectivityCost`), and an `expectInvariants` helper recomputes
  from scratch, for any `FMResult`, the cost and the `area`-plane-weighted
  balance feasibility and checks both against what the engine reported.
  Three groups:
  - `RandomHypergraphTest` — `generateRandomHypergraph` determinism and
    validity (degree bounds, in-range pins, no duplicate pins per edge).
  - `FMPartitionerTest` — dbBlock-free hypergraphs only. Stage 1 (2-way):
    determinism, balance across seeds, improvement over a topology-blind
    alternating initial, a two-clique-plus-bridge known optimal, and a
    weighted 8-cycle where rotating the cheap edge pair drags the optimal
    split with it (proof the objective follows the hyperedge `weight`
    plane). Stage 2 (K-way): the λ−1 objective proof (a pinned triangle
    across 3 parts costs 2×weight, not 1×), a three-clique known optimal at
    K=3, determinism/balance/reported-cost consistency for K∈{3,4},
    improvement over a striped initial, recovery from an initial with empty
    parts, fallback to random when a provided initial has out-of-range
    values, K=2 matching the spanning cut (and the Stage 1 path) exactly,
    and K=1 as the trivial zero-cost case. Plane / honesty coverage:
    balance following the vertex `area` plane where count-balance and
    area-balance disagree (only a 5+1 vs 6×1 area split is feasible, so
    an engine ignoring the plane fails), `balanced == false` reported
    when no feasible partition exists (one vertex outweighs the upper
    bound), and golden cut-cost quality floors on fixed-seed random
    hypergraphs for K∈{2,4} (`GoldenCostRegression`; a silent quality
    regression fails, matching or improving passes — update a golden
    only for an intentional quality trade-off).
  - `FMOdbTest` (fixture) — the only cases here needing `EDA_LAB_DATA_DIR`:
    Nangate45 + gcd DEF loaded once per suite, then 2-way and 4-way runs
    checking balance, reported-cost consistency, determinism, and strict
    cut improvement over a topology-blind initial on real topology.

## Input sources

- Real ODB data (`data/nangate45/` LEF + `data/gcd_nangate45.def`):
  `HypergraphTest` and `FMOdbTest` fixtures only.
- Programmatic dbBlocks via `src/netlistgen/` (OpenDB API, no LEF/DEF):
  all of `netlistgen_test.cpp`.
- dbBlock-free hypergraphs via `buildFromTopology` /
  `generateRandomHypergraph`: `HypergraphTopologyTest`,
  `RandomHypergraphTest`, `FMPartitionerTest`.

## Convention

Every new engine under `src/engines/` adds its own test file here and
registers it in the top-level `CMakeLists.txt`.
