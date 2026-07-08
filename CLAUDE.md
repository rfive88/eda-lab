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

## Documentation Conventions

1. **STANDING CONVENTION: every directory in this repo MUST contain a
   README.md describing its contents.** This applies to all existing
   directories and any directory created in the future. Any session that
   creates a new directory MUST create its README.md in the same session —
   no exceptions. (Vendored submodule internals under
   `third_party/openroad/` are upstream's tree and are exempt.)

2. **Engine README requirement:** every engine subdirectory under
   `src/engines/` documents in its README.md:
   - What the engine does (algorithm, objective)
   - Input contract: which hypergraph attribute planes it reads and writes
   - All control parameters/options, with types and defaults
   - How to run the engine and its tests

3. **READMEs describe what actually exists** — read the code before writing
   or updating them. When code in a directory changes materially, its
   README.md is updated in the same commit.

4. **STANDING CONVENTION — run outputs go in `run/`:** all manual test runs
   and any command that writes output files (out.def, out.odb, logs, dumps,
   etc.) execute from the `run/` directory. Never write output files to the
   repo root or source tree. Future engine CLIs and test binaries that emit
   files must write under `run/` or a temp path. `ctest` runs from `build/`
   are fine as-is. `run/` contents are git-ignored (except its README.md and
   `.gitkeep`).

## Layout

- `src/dbio/hello_odb.cpp` — LEF/DEF round-trip smoke test against OpenDB.
- `src/hypergraph/` — hypergraph netlist model (see below).
- `src/netlistgen/` — programmatic netlist construction, no LEF/DEF (see below).
- `src/support/ord_shim.cpp` — inert `ord::getLogger`/`ord::OpenRoad::openRoad`
  definitions so links survive when utl.a's Tcl-wrapper objects get pulled in.
- `src/engines/partitioning/` — Stage 1 partitioning engine (see below).
- `test/` — GTest suites; `EDA_LAB_DATA_DIR` points at `data/`.
- `data/` — Nangate45 LEF + `gcd_nangate45.def` test design.
- `run/` — git-ignored working directory for manual runs and their output
  files (see convention 4 above).

## Hypergraph netlist model (Phase 0, Item 3)

`eda::Hypergraph` (`src/hypergraph/hypergraph.h`) is a rebuild-on-demand view
of a `dbBlock`'s netlist: vertices are `dbInst`s, hyperedges are `dbNet`s.
There is no incremental sync with the database — call `buildFromBlock()` again
after netlist changes. `buildFromTopology(num_vertices, hyperedges)` builds the
same structure from explicit vertex-index lists with no dbBlock behind it (used
by engine tests and the random generator); in that mode all dbId lookups fail
soft (invalid id / `kInvalidIndex`).

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
against `dbNet::getITerms()`, inst/net count parity with `hello_odb`, and the
attribute-plane semantics below.

### Attribute planes

Named per-vertex / per-hyperedge side arrays for engines, parallel to the CSR
arrays. Fixed type menu — `double`, `int`, `bool` — no templates. API:

- `vertexDoublePlane/vertexIntPlane/vertexBoolPlane(name)` →
  `std::vector<T>&` sized `numVertices()`; `hyperedge*Plane(name)` mirrors
  sized `numHyperedges()`. Created on first access, zero-initialized
  (0.0 / 0 / false).
- `hasVertexPlane`/`hasHyperedgePlane`, `removeVertexPlane`/
  `removeHyperedgePlane`, `clearAllPlanes()`.
- `findVertexDoublePlane`/`findHyperedgeDoublePlane` — `const` probes for
  engines taking the hypergraph read-only: plane pointer if it exists and is
  double-typed, else `nullptr`; never create, never warn.
- Vertex and hyperedge planes are independent namespaces (same name OK).
- A name is bound to its first-created type. A different-typed access is a
  caller bug: it logs a `utl::Logger` warning (UKN-0100; `warn`, not `error`,
  because `Logger::error()` throws at the pinned SHA and this API never
  throws) and returns separate valid storage of the requested type — the
  original plane is untouched. Pass a logger via `Hypergraph(utl::Logger*)`
  to see the diagnostic.

**Local-index rule:** planes are indexed by the dense local index
(`vertexIndex()`/`hyperedgeIndex()`), never by `dbId`.

**Rebuild invalidation rule:** planes are valid only for the topology
snapshot they were created against. `buildFromBlock()` and `clear()` destroy
every plane (enforced in `clear()`, the choke point both pass through) —
never cache a plane reference across a rebuild.

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

## Partitioning engine (Stage 1)

`src/engines/partitioning/` — flat 2-way Fiduccia–Mattheyses
(`partitionFM()` in `fm_partitioner.h`), Stage 1 of a planned multilevel
K-way multi-objective partitioner. Minimizes weighted cut under a
vertex-weight balance constraint; pure library (const hypergraph in,
`FMResult` by value out, no I/O, no exceptions, deterministic for fixed
(hypergraph, params)). Plane contract, both optional and read-only:
hyperedge double plane `weight` (cut cost per edge, else 1.0) and vertex
double plane `area` (balance weight, else 1.0); the engine writes no
planes. Gains are exact doubles in per-side max-heaps with lazy deletion
(not integer buckets). `FMParams`: `balance_tolerance` (0.10),
`max_passes` (10), `seed` (1), `initial` (kRandom | kProvided), optional
`logger` (debug group "fm"). Note the tolerance must exceed the heaviest
vertex's share of W/2 or no single move is feasible — matters on tiny
graphs.

`generateRandomHypergraph()` (`random_hypergraph.h`) makes dbBlock-free
test hypergraphs via `buildFromTopology`: seed-determined, bit-identical
across platforms (raw mt19937 draws, no `uniform_int_distribution`),
distinct pins per edge, degrees uniform in `[min_degree, max_degree]`
clamped to `[2, num_vertices]`.

Tests: `test/fm_partitioner_test.cpp` (only the gcd-design test needs
`EDA_LAB_DATA_DIR`). Full details in `src/engines/partitioning/README.md`.

Linking gotcha: any new object that makes the linker touch `utl.a`'s
swig/Tcl-wrapper members (they can satisfy stray weak std:: symbols) drags in
references to OpenROAD-application globals. `ord_shim` (linked via
`netlistgen`) defines those globals inertly; link it into any new target that
hits `undefined reference to ord::getLogger()` / `ord::OpenRoad::openRoad()`.

Note: the filesystem is case-insensitive here — `CLAUDE.md` and `Claude.md`
are the same file.