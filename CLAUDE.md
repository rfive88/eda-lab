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

## CLI `--help` / usage (STANDING CONVENTION)

Every standalone CLI exposes `--help`/`-h` and, on a missing required
argument, prints a usage/error block — both rendered from **one**
registered option list so each option's one-line description is written
exactly once in code and cannot drift between the two paths.

**Mechanism — Option B, a small internal header (`src/support/cli.h`),
not a CLI-parsing library.** Rationale: the repo's CLIs are trivial
(≤ 4 argv-level options), and the load-bearing requirement — the *same*
one-line description appears in both `--help` and the missing-argument
error — is exactly what general CLI libraries (CLI11 etc.) don't give
out of the box (they print a generic error on missing required args). A
~120-line header yields precisely that behaviour with no new dependency.
Each CLI builds an `eda::CliSpec { program, summary, vector<CliOption> }`
(one `CliOption` = `name`, `metavar`, `required`, one-line `description`)
and calls `eda::wantsHelp` / `printHelp` / `printUsageError`.

Rules:

- The one-line description lives **once** in the `CliSpec`; `printHelp`
  and `printUsageError` both render from it. `eda::verbosityOption()`
  provides the shared `-verbosity` wording so it is identical across every
  CLI, not just within one file.
- `--help`/`-h` prints to stdout and exits **0**, regardless of what other
  args were supplied. A missing required argument prints to stderr (naming
  the offending option), exits **nonzero**, and points at `--help`.
- The CLI stays terse: only one-line descriptions. Depth (defaults, ranges,
  interactions, examples) lives in the engine's `README.md`
  "Command-Line Options" section — **not** printed by the CLI. This
  convention covers argv-level options only; JSON config-field validation
  keeps its own fail-fast path (see netlistgen `cli_config`).

Additive-only, same as the logging convention: `--help`/usage output is
new, valid invocations are unchanged. `test/cli_help_test.cpp` enforces
the single-source-of-truth guarantee (unit-tests the renderer and spawns
both CLIs). There is no partitioner CLI yet (library/test-only); when one
is added it uses this helper from the start.

## Error handling (STANDING CONVENTION)

Expected, recoverable failures — a file path that does not exist, a
malformed config, a LEF/DEF that fails to parse, a missing output
directory — are surfaced as a **value that propagates to `main()` and
produces a clean message + nonzero exit**, never a crash. New code applies
this wherever possible. There are three layers; the first two are
load-bearing, the third is a backstop.

**Layer 1 — check expected failures at the point they occur.** Prefer to
prevent the failure over catching it: a function that can hit an expected
failure checks for it and returns a failure value rather than proceeding on
an assumed-valid pointer/stream. Two forms coexist and are both valid
"explicit return-value" propagation:

- `eda::Status` / `eda::ErrorCode` (`src/support/status.h`, header-only,
  `[[nodiscard]]`) — the go-forward standard for new code. A `Status`
  carries a coarse `ErrorCode` plus a human-readable `message`; callers
  check `.ok()` and early-return. Because it is `[[nodiscard]]`, an
  ignored failure is a compiler warning, not a silent bug. `hello_odb`
  uses it.
- `bool` + a `std::string& error` out-param (or an `int` process exit
  code at the CLI boundary) — the pre-existing idiom in netlistgen's
  `cli_config` / `runCliFromFile` and `NetlistBuilder`. Grandfathered and
  equally acceptable; a `bool`+message maps trivially onto a `Status`.

This applies to **library/engine code, not just CLI wrappers**: e.g.
`NetlistBuilder::loadLef` prechecks each LEF path with
`std::filesystem::exists` before handing it to `lefin`, and
`validateAndWrite` creates each output path's parent directory
(`create_directories`) if missing before writing, failing cleanly only
when a directory genuinely cannot be created.

**Layer 2 — a boundary `try/catch` around OpenROAD reader calls.** This is
required because of a hard fact about the pinned SHA: `utl::Logger::error()`
**throws `std::runtime_error`** (and `critical()` calls `exit()`; see
`src/support/logging.h`). OpenROAD's `lefin`/`defin` call `error()` on a
malformed (but present) file, so the throw comes from *inside* odb.
**Empirically, that throw is NOT catchable from `main()`** — unwinding an
odb exception all the way up fails and calls `std::terminate` (or, with
utl's swig error path linked in, segfaults). It **is** catchable by a
`try/catch` placed right at the reader call, where the stack still unwinds
cleanly. So every call into an OpenROAD reader that can fail is wrapped in
a local `try/catch` that converts the exception to a `Status`/`bool`
failure (see `loadDesign` in `hello_odb.cpp` and `NetlistBuilder::loadLef`).
Missing-file prechecks (layer 1) keep the common case from ever throwing;
the boundary catch covers present-but-malformed files.

**Layer 3 — a top-level `try/catch` in every `main()`.** A catch-all
(`catch (const std::exception&)` + `catch (...)`) wrapping a thin
`runXxxCli(argc, argv)` worker. This is the backstop for ordinary
catchable exceptions (`std::bad_alloc`, an STL throw, a bug this audit
missed) — NOT for odb `error()` throws, which layer 2 already contains at
their source. `main()` stays a thin wrapper so the worker is unit-testable
by return code without spawning a subprocess.

Reconciliation with "no exceptions across engine APIs": eda-lab's own APIs
still never throw — they return `Status`/`bool`. The exception machinery
here exists only to contain OpenROAD's throwing `error()` at the odb
boundary and as a last-resort backstop; library code still signals its own
failures by return value and uses `warn()`, never `error()`/`critical()`.

Additive-only, like the other conventions: behavior on valid input is
unchanged. `test/error_handling_test.cpp` enforces it — in-process cases
call `runCliFromFile` (missing config, malformed JSON, missing output
dir); subprocess cases spawn the real binaries via `fork`/`execv` (so a
crash is detectable as a signal death, not masked by a shell) for
nonexistent and malformed LEF, confirming a clean nonzero exit rather than
an abort/segfault.

## Layout

- `src/dbio/hello_odb.cpp` — LEF/DEF round-trip smoke test against OpenDB.
- `src/support/logging.h` — the logging/verbosity convention above:
  confirmed `utl::Logger` API notes, level constants, `applyVerbosity()`.
- `src/support/cli.h` — the CLI `--help`/usage convention above: `CliSpec`,
  `CliOption`, `printHelp`/`printUsageError`/`wantsHelp`, `verbosityOption()`.
- `src/support/status.h` — the error-handling convention above: `Status`,
  `ErrorCode`, `okStatus()`/`makeError()` (header-only, `[[nodiscard]]`).
- `src/hypergraph/` — hypergraph netlist model (see below).
- `src/support/ord_shim.cpp` — inert `ord::getLogger`/`ord::OpenRoad::openRoad`
  definitions so links survive when utl.a's Tcl-wrapper objects get pulled in.
- `src/engines/netlistgen/` — programmatic netlist construction, synthetic
  or LEF-backed, with DEF/`.odb` output (see below).
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
calls — optionally backed by real LEF cells — so tests and benchmarks can
create netlists of any size with exactly known or statistically controlled
topology, then feed them to `Hypergraph::buildFromBlock()`. Promoted from a
Stage 1/2 test utility into a full engine: Stage A relocated it here and
made pin access IoType-based; Stage B added LEF-backed masters and the
statistical cell mix; Stage C added DEF/`.odb` writers, net well-formedness
validation, and the JSON-driven `netlistgen_cli`; Stage D made
statistical-mix net formation combinational-loop-free by construction,
completing Phase 1; Stage E1 adds primary I/O port generation via Rent's
rule (`T = k·Gᵖ`). Structural Verilog output is Stage E2 — see
`src/engines/netlistgen/README.md`. Two core layers:

- **`NetlistBuilder`** owns a fresh `dbDatabase` (tech, lib, chip, top block)
  and wraps master/inst/net creation and pin connection. It handles OpenDB's
  master-freeze protocol (`dbMTerm::create` all pins, then `setFrozen()`,
  required before `dbInst::create`). Synthetic masters are connectivity-only
  (no geometry) with pins named `i0..iN-1` / `o0..oM-1`; `loadLef()` loads a
  real tech + cell library instead.
- **`generateSynthetic(builder, spec, out_cluster_id?, out_rent_stats?)`**
  populates the block from a `SyntheticNetlistSpec`. Seeded `std::mt19937`
  makes a given (spec, seed) reproducible; fanout = load pins per net,
  driver excluded, drawn from `[min_fanout, max_fanout]`; every iterm lands
  on at most one net. Statistical mode (sequential/combinational ratio +
  pin-count-bucket mix) forms nets in instance-creation order with
  receiver-eligibility filtering so combinational cycles are impossible by
  construction (a comb output only drives sequential inputs or
  later-created comb instances); this requires `sequential_ratio > 0`
  (fail-fast; Stage E1's ports run as a later, separate pass and do not
  relax this). The legacy weighted `masters` mix keeps unconstrained
  shuffled-pool pairing (no acyclicity guarantee). Scale: ~500k insts /
  ~1.4M pins in about 2s.
  - **Peak fanout sub-clusters** (optional, congestion hot-spots for
    validating downstream metrics tooling): `assignPeakClusters` groups a
    subset of instances into clusters once per run, and cluster-driven nets
    bias receiver selection toward same-cluster cells — strictly *within*
    the pools Stage D already computed as eligible, so the DAG guarantee is
    untouched. Requires the statistical mix.
  - **Primary I/O generation via Rent's rule** (Stage E1, optional): sizes a
    PI/PO terminal count from `T = k·Gᵖ`, randomly samples that many
    already-formed nets, and inserts combinational/buffered/registered
    boundary cells, reusing the statistical mix's representative masters. A
    PI **replaces** its selected net's existing driver rather than adding a
    second one (`netlist_validation.cpp` now folds `dbBTerm`s into its
    driver/sink tally); a PO is just one more sink/observer. `netlistgen`
    still never touches the `Hypergraph` engine — `RentStats` returns raw
    `dbNet*`/`dbInst*` lists (`hgm.is_pi`/`is_po` are naturally hyperedge
    planes, `is_boundary_buf`/`is_boundary_reg` vertex planes, but building
    them is the caller's job). Requires the statistical mix.

Tests: `test/netlistgen_test.cpp` (Stage A, no data files),
`netlistgen_stageb_test.cpp` (statistical mix / LEF classification),
`netlistgen_stagec_test.cpp` (writers, validation, CLI),
`netlistgen_staged_test.cpp` (loop freedom, bootstrap fail-fast, thin-pool
behavior, CLI DEF round-trip cycle check),
`netlistgen_peak_cluster_test.cpp` (peak fanout sub-clusters),
`netlistgen_rent_test.cpp` (Stage E1 primary I/O generation).

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