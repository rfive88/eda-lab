# test/

GTest suites for eda-lab.

## Build and run

```bash
cmake -B build              # Debug tree; build-release/ is the Release tree
cmake --build build
ctest --test-dir build -R "hypergraph_test|netlistgen_test|netlistgen_stageb_test|netlistgen_stagec_test|netlistgen_staged_test|netlistgen_peak_cluster_test|netlistgen_rent_test|netlistgen_link_smoke|fm_partitioner_test|cli_help_test|error_handling_test" --output-on-failure
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
- `netlistgen_test.cpp` — the synthetic netlist generation engine
  (`src/engines/netlistgen/`, relocated from `src/netlistgen/` in Stage A
  of its engine promotion), no data files needed. A hand-built 3-inst/2-net
  netlist asserting exact hypergraph CSR contents in both directions
  (`NetlistBuilderTest.ExactTopology`), and `generateSynthetic` conformance
  on a 2000-inst netlist (fanout bounds, one driver per net, no pin reuse),
  net-count limiting, and seed determinism (`SyntheticNetlistTest`).
- `netlistgen_stagec_test.cpp` — Stage C of the same engine: the DEF / `.odb`
  writers, net well-formedness validation, JSON config parsing, and the
  standalone `netlistgen_cli` executable. Needs `EDA_LAB_DATA_DIR` (Nangate45
  LEF) and the built `netlistgen_cli` binary (path compiled in via
  `NETLISTGEN_CLI_BIN`; `add_dependencies` ensures it is built first). Covers:
  `validateNetlist` passing on synthetic + LEF-backed output and flagging
  hand-built dangling / driverless / multi-driver / sinkless nets; the writers
  producing files; JSON parsing (Mode A/B valid, missing `instance_count`, no
  output path, malformed JSON); the CLI's validate-before-write fail-fast on a
  malformed block (nothing written); and a CLI smoke test that spawns the
  binary, reads the DEF back through `defin`, and confirms instance/net counts.
  (Well-formedness is a distinct guarantee from loop-freedom — the latter is
  Stage D's, covered by `netlistgen_staged_test.cpp`.)
- `netlistgen_staged_test.cpp` — Stage D of the same engine: combinational-
  loop freedom of the statistical net formation. Needs `EDA_LAB_DATA_DIR`
  and `NETLISTGEN_CLI_BIN`. Covers: cycle-detector sanity on a hand-built
  two-inverter loop (must flag) and a register feedback loop (must not);
  DFS cycle detection — sequential instances cut at the D/Q boundary — over
  synthetic runs at 300/5000/50000 instances x 3 seeds and LEF-backed runs
  x 2 seeds, plus the stronger construction invariant that every comb->comb
  edge follows instance creation order, and `Hypergraph::buildFromBlock` on
  the acyclic block; the `sequential_ratio > 0` bootstrap fail-fast (zero,
  unset, just-above-zero passes, legacy-mode exemption); the thin-pool edge
  case (loosened receiver counts / skipped tail drivers, never a sinkless
  net, deterministic on rerun); and a spawned-CLI DEF round-trip confirmed
  loop-free.
- `netlistgen_peak_cluster_test.cpp` — peak fanout sub-clusters (congestion
  hot-spot generation layered on the statistical mix + Stage D), no data
  files needed. Covers: no-peak-params leaves cluster bookkeeping empty;
  basic single-cluster generation (instance/cluster counts, Stage D DAG
  still valid, cluster-driven net fanout measurably above the global average
  and within 20% of `peak_avg_fanout`); `assignPeakClusters` directly
  (multi-cluster sizing, seed determinism) and via `generateSynthetic` for
  `num_peak_clusters=3`; all four validation failures (below-background
  target, out-of-range `peak_cluster_pct`, `num_peak_clusters=0`,
  legacy-mix rejection) and the "ignored when `peak_avg_fanout` absent"
  rule; and the `peak_cluster_pct`/`num_peak_clusters` defaults.
- `netlistgen_rent_test.cpp` — Stage E1 primary I/O generation via Rent's
  rule (`T = k·Gᵖ`), no data files needed but links `hypergraph` (the test
  itself performs the RentStats -> hypergraph-plane translation `netlistgen`
  deliberately doesn't — see its README's "Primary I/O generation
  (Stage E1)"). Covers: no-E1-params leaves `RentStats.engaged` false and
  every `hgm.*` plane absent; a basic 2000-inst run (target/actual Rent
  within tolerance, PI/PO split, all four planes populated correctly,
  `is_boundary_reg` never set on an internal FF, `p_actual` finite
  positive); a custom `io_input_ratio`; an all-combinational
  `io_pin_type_distribution` producing zero boundary cells; combining with
  peak fanout sub-clusters (per-cluster + background Rent stats); the
  strict no-dangling-instance invariant across several instance
  counts/seeds (`NoDanglingInstancesAfterE1` — zero fully-isolated
  instances before and after, pre-existing dead-output count never
  increases) plus a `validateNetlist(...).ok` check in every generating
  test; all validation failures (exactly-one-of `rent_k`/`rent_p`,
  `rent_p > 1.2`, the `(1.0, 1.2]` warn-and-clamp case, bad
  `io_pin_type_distribution` sum, out-of-range `io_input_ratio`, legacy-mix
  rejection); and a small-design `T`-capping run that completes without
  crashing.
- `netlistgen_link_smoke.cpp` — library-linkage guard for the same engine,
  no data files needed. A plain `main()` (no GTest) that links the
  `netlistgen` library as an external consumer would
  (`target_link_libraries(... PRIVATE netlistgen odb utl)`, sources not
  compiled in) and calls `generateSynthetic`; returns non-zero on failure.
  Fails to link if `netlistgen` ever stops being a real library target.
- `cli_help_test.cpp` — the repo CLI `--help`/usage convention
  (`src/support/cli.h`), no data files needed. Unit-tests the shared renderer
  (every option's one-line description appears in both `--help` and the
  missing-argument block — the single-source-of-truth guarantee), then spawns
  both `hello_odb` and `netlistgen_cli` (paths compiled in via `HELLO_ODB_BIN`
  / `NETLISTGEN_CLI_BIN`; `add_dependencies` builds them first) to confirm
  `--help` exits 0 and lists the options while a missing required argument
  exits nonzero repeating the same descriptions.
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
  `HypergraphTest` and `FMOdbTest` fixtures; `netlistgen_stageb_test` and
  `netlistgen_stagec_test` also use the Nangate45 LEF (no DEF).
- Programmatic dbBlocks via `src/engines/netlistgen/` (OpenDB API, no
  LEF/DEF): `netlistgen_test.cpp` and `netlistgen_link_smoke.cpp`.
- dbBlock-free hypergraphs via `buildFromTopology` /
  `generateRandomHypergraph`: `HypergraphTopologyTest`,
  `RandomHypergraphTest`, `FMPartitionerTest`.

## Convention

Every new engine under `src/engines/` adds its own test file here and
registers it in the top-level `CMakeLists.txt`.
