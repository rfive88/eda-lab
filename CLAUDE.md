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

## Flow Diagrams (FLOW.md)

1. **STANDING CONVENTION:** every engine directory and every non-trivial
   multi-file component directory (e.g. `src/hypergraph`, `src/engines/*`)
   carries a single FLOW.md.

2. **FLOW.md contains Mermaid diagrams showing the algorithmic/logical flow
   of the code** — what it does and why — not merely call structure.
   Structure:
   - Short intro paragraph: what the component is.
   - Per-file sections: for each significant source file, brief prose plus
     a Mermaid diagram of that file's internal flow (key functions' control
     flow, or the data structure it manages).
   - Engine/component-level section: one or more Mermaid diagrams of the
     overall algorithm that spans the files, end to end.
   - Prose stays tight; diagrams carry the load. Prose must name real
     functions/types from the source so a reader can cross-check diagram
     vs code.
   - Use Mermaid flowcharts (`graph TD`) for control/algorithm flow,
     sequence diagrams (`sequenceDiagram`) for cross-component interaction.
     Prefer several small focused diagrams over one large one. Mermaid must
     be valid syntax.

3. **FLOW.md is text** (committed, pushed, rendered by Mermaid on GitHub /
   VS Code preview). It is not auto-extracted — it is maintained by
   discipline, via two maintenance paths:
   - **Same-commit rule:** any session that makes a functional/logic change
     to files in a component directory MUST update that directory's
     FLOW.md (the affected per-file section(s) and the engine-level
     diagram) in the same commit. Identical in spirit to the README
     same-commit rule. A FLOW.md that has drifted from the code is worse
     than none, because it misleads.
   - **On-demand regeneration:** FLOW.md can be regenerated at any time by
     re-reading the current source in the directory as ground truth and
     rebuilding the diagrams from scratch to match. Use this after manual
     edits, direct fixes, or any change that bypassed the same-commit rule.
     Regeneration does not assume the existing FLOW.md is correct — it
     reflects what the code actually is now.

4. **Cross-reference:** each directory's README.md should carry a one-line
   pointer — "See FLOW.md for algorithmic flow diagrams." — added as
   directories get their FLOW.md.

## Logging & runtime verbosity (STANDING CONVENTION)

All messages in this repo — human-written or Claude Code-written — go
through OpenROAD's `utl::Logger`. No custom stdout/stderr message scheme,
no ad hoc `printf`/`std::cout`/`std::cerr` debug statements. The single
source of truth for the API and level scheme is `src/support/logging.h`
(read its header comment before touching logging anywhere).

Confirmed `utl::Logger` API at the pinned SHA (`a3d4865`): severity
methods `report()`, `info(tool,id,…)`, `warn(tool,id,…)`,
`error(tool,id,…)` (**throws** `std::runtime_error` — never used here,
this repo signals failure by return value), `critical(…)` (exits). The
runtime-verbosity mechanism is the built-in debug tier: the
`debugPrint(logger, tool, group, level, …)` macro emits only when
`setDebugLevel(tool, group, L)` was called with `L >= level`. A `Logger`
writes to a **stdout** colour sink; DEF/`.odb`/Verilog/partition results
are written to their own file paths, so logging never mixes into
deterministic data output. (`debugPrint` dereferences its logger
argument with `->`, so guard a possibly-null logger with an explicit
`if (logger != nullptr)`, and pass a pointer *variable* — `&value`
misparses inside the macro.)

The convention:

- **Tool id `utl::UKN`** for every eda-lab message (the pinned `ToolId`
  enum has no eda-lab tool). `info()`/`warn()` message ids are
  partitioned so they stay unique across the shared UKN namespace:
  hypergraph 100–119, fm 120–129, hello_odb 200–209, netlistgen library
  300–319, netlistgen CLI 320–349. `debug()` takes no id.
- **One debug group per component** — `"hypergraph"`, `"fm"`,
  `"netlistgen"`, `"hello_odb"` — so `-verbosity` lifts a whole run.
- **Every executable takes an optional `-verbosity <level>`** flag
  (`--verbosity=<level>` too), mapped straight onto `setDebugLevel` via
  `eda::applyVerbosity()`. Levels: **0** (default) phase markers at the
  executable layer + final summary + warnings/errors; **1** per-phase
  detail (counts, achieved-vs-requested stats, library phase markers);
  **2** progress heartbeats; **3** per-item tracing, capped at
  `eda::kTraceCap` with an explicit "trace capped" note.
- **Executable-layer phase markers are `info()`** (default-visible on
  stdout). **Library/engine phase markers are `debugPrint()`**
  (debug-gated) so in-memory callers — and existing tests that assert a
  captured logger sink is empty — stay silent at verbosity 0; a CLI that
  raises verbosity surfaces the library trace through the shared logger.
- **Library/engine code threads the logger directly**, not just CLI
  wrappers: `Hypergraph(utl::Logger*)`, `NetlistBuilder(name, logger*)`
  (external logger shared, not owned), `FMParams::logger`,
  `generateSynthetic` via `builder.logger()`.

Additive-only: adding logging must never change a test's results or a
generated file. Run the eda-lab tests after any logging change.

## Layout

- `src/dbio/hello_odb.cpp` — LEF/DEF round-trip smoke test against OpenDB.
- `src/support/logging.h` — the logging/verbosity convention above:
  confirmed `utl::Logger` API notes, level constants, `applyVerbosity()`.
- `src/hypergraph/` — hypergraph netlist model (see below).
- `src/support/ord_shim.cpp` — inert `ord::getLogger`/`ord::OpenRoad::openRoad`
  definitions so links survive when utl.a's Tcl-wrapper objects get pulled in.
- `src/engines/netlistgen/` — programmatic netlist construction, no LEF/DEF
  (see below).
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

`src/engines/netlistgen/netlistgen.h` builds `dbBlock`s through OpenDB API
calls only — no LEF/DEF — so tests and benchmarks can create netlists of any
size with exactly known or statistically controlled topology, then feed them
to `Hypergraph::buildFromBlock()`. It is being promoted from a Stage 1/2 test
utility into a full engine (Stage A of 5 relocated it here and made pin
access IoType-based; LEF-backed masters, statistical cell mix, loop
avoidance, and DEF/`.odb`/Verilog writers land in later stages — see
`src/engines/netlistgen/README.md`). Two layers:

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

## Partitioning engine (Stages 1–2)

`src/engines/partitioning/` — flat K-way Fiduccia–Mattheyses
(`partitionFM()` in `fm_partitioner.h`), Stages 1–2 of a planned
multilevel K-way multi-objective partitioner. Minimizes the weighted
connectivity-1 objective — Σ weight(e) × (λ(e) − 1), λ(e) = number of
distinct parts touching hyperedge e — under a per-part vertex-weight
balance constraint `[(1 − t)·W/K, (1 + t)·W/K]`; for `num_parts == 2`
this is exactly the Stage 1 weighted spanning cut and the K = 2 path
reproduces Stage 1 results bit-for-bit. Pure library (const hypergraph
in, `FMResult` by value out, no I/O, no exceptions, deterministic for
fixed (hypergraph, params)). Plane contract, both optional and
read-only: hyperedge double plane `weight` (cut cost per edge, else 1.0)
and vertex double plane `area` (balance weight, else 1.0); the engine
writes no planes. Core structure: per-hyperedge per-part pin counts with
λ maintained incrementally; gains are exact doubles, one entry per
(vertex, target part), in per-source-part max-heaps with lazy deletion
(not integer buckets). `FMParams`: `num_parts` (2), `balance_tolerance`
(0.10), `max_passes` (10), `seed` (1), `initial` (kRandom | kProvided,
provided values in `[0, num_parts)`), optional `logger` (debug group
"fm"). Note the tolerance must exceed the heaviest vertex's share of W/K
or no single move is feasible — matters on tiny graphs.

`generateRandomHypergraph()` (`random_hypergraph.h`) makes dbBlock-free
test hypergraphs via `buildFromTopology`: seed-determined, bit-identical
across platforms (raw mt19937 draws, no `uniform_int_distribution`),
distinct pins per edge, degrees uniform in `[min_degree, max_degree]`
clamped to `[2, num_vertices]`.

Tests: `test/fm_partitioner_test.cpp` (only the gcd-design tests need
`EDA_LAB_DATA_DIR`). Full details in `src/engines/partitioning/README.md`.

Linking gotcha: any new object that makes the linker touch `utl.a`'s
swig/Tcl-wrapper members (they can satisfy stray weak std:: symbols) drags in
references to OpenROAD-application globals. `ord_shim` (linked via
`netlistgen`) defines those globals inertly; link it into any new target that
hits `undefined reference to ord::getLogger()` / `ord::OpenRoad::openRoad()`.

Note: the filesystem is case-insensitive here — `CLAUDE.md` and `Claude.md`
are the same file.