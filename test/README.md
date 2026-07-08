# test/

GTest suites for eda-lab.

## Build and run

```bash
cmake -B build              # Debug tree; build-release/ is the Release tree
cmake --build build
ctest --test-dir build -R "hypergraph_test|netlistgen_test" --output-on-failure
```

The `-R` filter matters: a bare `ctest` also picks up the vendored OpenROAD
integration tests, which need the never-built `openroad` binary and fail.

The test binaries can also be run directly. Manual runs execute from the
`run/` directory so any output files land there instead of the repo root
(standing convention in `CLAUDE.md`):

```bash
cd run
../build/hypergraph_test
../build/netlistgen_test
```

## Test files

- `hypergraph_test.cpp` — loads Nangate45 LEF + the gcd DEF once per suite
  (via `EDA_LAB_DATA_DIR`, which CMake points at `data/`). Covers:
  empty-block build, id↔index round trips, CSR connectivity checked against
  `dbNet::getITerms()`, transpose consistency, inst/net count parity with
  `hello_odb`, and the attribute-plane semantics (on-demand creation,
  persistence, type conflict, removal, rebuild invalidation,
  vertex/hyperedge namespace independence).
- `netlistgen_test.cpp` — no data files needed. A hand-built 3-inst/2-net
  netlist asserting exact hypergraph CSR contents, spec conformance on a
  2000-inst synthetic netlist, net-count limiting, and seed determinism.

## Convention

Every new engine under `src/engines/` adds its own test file here and
registers it in the top-level `CMakeLists.txt`.
