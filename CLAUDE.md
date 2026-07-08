# eda-lab

Experimental EDA playground built on OpenROAD's OpenDB (`odb`). C++17, CMake,
GTest. OpenROAD is vendored as a submodule at `third_party/openroad`, pinned to
`a3d4865`, and pulled in with `add_subdirectory(... EXCLUDE_FROM_ALL)` — only
the `odb` and `utl` targets are linked; never build the top-level `openroad`
target.

## Build & test

```bash
cmake -B build            # Debug tree (build-release/ is the Release tree)
cmake --build build
ctest --test-dir build    # or run build/hypergraph_test directly
```

## Layout

- `src/dbio/hello_odb.cpp` — LEF/DEF round-trip smoke test against OpenDB.
- `src/hypergraph/` — hypergraph netlist model (see below).
- `src/netlistgen/` — programmatic netlist construction, no LEF/DEF (see below).
- `src/support/ord_shim.cpp` — inert `ord::getLogger`/`ord::OpenRoad::openRoad`
  definitions so links survive when utl.a's Tcl-wrapper objects get pulled in.
- `src/engines/` — placeholder for future partitioning/clustering engines.
- `test/` — GTest suites; `EDA_LAB_DATA_DIR` points at `data/`.
- `data/` — Nangate45 LEF + `gcd_nangate45.def` test design.

## Hypergraph netlist model (Phase 0, Item 3)

`eda::Hypergraph` (`src/hypergraph/hypergraph.h`) is a rebuild-on-demand view
of a `dbBlock`'s netlist: vertices are `dbInst`s, hyperedges are `dbNet`s.
There is no incremental sync with the database — call `buildFromBlock()` again
after netlist changes.

Key design points:

- **Two identifier spaces.** Stable `dbId<T>` OpenDB ids survive rebuilds and
  save/load; dense `[0, n)` indices are assigned in dbSet iteration order at
  build time and are only valid until the next rebuild. Bidirectional maps
  translate between them (`vertexId`/`vertexIndex`, `hyperedgeId`/
  `hyperedgeIndex`). Bad lookups return an invalid `dbId` (0) or
  `kInvalidIndex` — no exceptions.
- **Dual CSR topology.** Two mirrored compressed-sparse-row arrays:
  hyperedge-major (edge → member vertices via `hyperedgeOffsets()`/`pinList()`)
  and vertex-major (vertex → incident edges via `vertexOffsets()`/
  `vertexPinList()`). Memory is doubled to get cache-friendly traversal in both
  directions, the access pattern partitioning/clustering kernels need.
- **One entry per `dbITerm`.** An instance with several pins on the same net
  appears once per pin, keeping each edge's pin slice in one-to-one
  correspondence with `dbNet::getITerms()`.
- **Three-pass build** (`hypergraph.cpp`): number vertices, stream the
  hyperedge-major CSR in net iteration order, then transpose to vertex-major
  with a counting sort (degree count, prefix sum, scatter) — O(pins), no
  per-vertex temporary lists, and each vertex's edge slice comes out sorted.

Tests (`test/hypergraph_test.cpp`) load Nangate45 + the gcd DEF once per suite
and cover: empty-block build, id↔index round trips, connectivity checked
against `dbNet::getITerms()`, and inst/net count parity with `hello_odb`.

## Programmatic netlist construction (netlistgen)

`src/netlistgen/netlistgen.h` builds `dbBlock`s through OpenDB API calls
only — no LEF/DEF — so tests and benchmarks can create netlists of any size
with exactly known or statistically controlled topology, then feed them to
`Hypergraph::buildFromBlock()`. Two layers:

- **`NetlistBuilder`** owns a fresh `dbDatabase` (tech, lib, chip, top block)
  and wraps master/inst/net creation and pin connection. It handles OpenDB's
  master-freeze protocol (`dbMTerm::create` all pins, then `setFrozen()`,
  required before `dbInst::create`). Masters are connectivity-only (no
  geometry) with pins named `i0..iN-1` / `o0..oM-1`.
- **`generateSynthetic(builder, spec)`** populates the block from a
  `SyntheticNetlistSpec`: weighted cell mix (`MasterSpec` name/inputs/outputs/
  weight), instance count, optional net count, and a fanout range
  `[min_fanout, max_fanout]` (pins per net, driver included). Seeded
  `std::mt19937` makes a given (spec, seed) reproducible. Each net takes one
  unused output pin (driver) and fanout−1 unused input pins from shuffled
  pools, so every iterm lands on at most one net — always a valid netlist.
  Generation stops when the requested net count is reached or a pin pool
  drains. Scale: ~500k insts / ~1.4M pins generate in about 2s.

Tests (`test/netlistgen_test.cpp`, no data files needed): a hand-built
3-inst/2-net case asserting exact hypergraph CSR contents, spec conformance
(fanout bounds, counts, pin uniqueness) on a 2000-inst netlist, net-count
limiting, and seed determinism.

Linking gotcha: any new object that makes the linker touch `utl.a`'s
swig/Tcl-wrapper members (they can satisfy stray weak std:: symbols) drags in
references to OpenROAD-application globals. `ord_shim` (linked via
`netlistgen`) defines those globals inertly; link it into any new target that
hits `undefined reference to ord::getLogger()` / `ord::OpenRoad::openRoad()`.

Note: the filesystem is case-insensitive here — `CLAUDE.md` and `Claude.md`
are the same file.