# Spike Brief — Stage A of 5: Foundation
## (directory move, library target structure, IoType refactor, regression gate)

**Target directory:** `src/engines/netlistgen/` → `src/engines/netlistgen/`
**Prerequisites:** none — this is the first stage.
**Blocks:** Stage B (LEF-backed generation + statistical cell mix)
**Do not proceed past this stage's acceptance gate (Section 4) in the same session.**

This is Stage A of a 5-stage plan to promote `netlistgen` from a Stage 1/2
test utility into a full engine. The full plan, for context Claude Code may
find useful but should not act on beyond this stage's scope:
- **Stage A (this brief)**: directory move, library-target CMake structure,
  IoType-based pin refactor, regression-safety proof.
- **Stage B**: LEF-backed generation, statistical cell mix (two modes,
  including the max-entropy solve). Net formation is not yet
  combinational-loop safe at the end of this stage.
- **Stage C**: combinational-loop avoidance — retrofits net formation to
  be provably acyclic among combinational cells.
- **Stage D**: DEF/`.odb` writers, standalone CLI executable, doc
  finalization. Completing this stage's gate unblocks Stage 3.
- **Stage E**: primary I/O ports + Verilog writer (separate brief,
  general-purpose EDA test-case use case, not required for Stage 3).

---

## 1. Goal for this stage

Get the engine relocated, properly structured as a reusable library, and
refactored to be name-agnostic about pin access — all **before** any new
generation capability is added. This stage is pure plumbing/refactor, no
new features, specifically so it can be validated in isolation against the
existing test suite.

## 2. Directory move + library target structure

- `git mv src/netlistgen src/engines/netlistgen`; update all
  `CMakeLists.txt` references (root + any engine-level lists) and
  `#include` paths.
- Move `test/netlistgen_test.cpp` accordingly (path/namespace updates only
  — no behavioral changes, see Section 4).
- **`netlistgen` must build as a library target** (static or object
  library, matching whatever convention `src/hypergraph` already uses for
  linking against `odb`/`utl`) — not just source files compiled directly
  into a test or CLI binary. This is what lets other engines (e.g. a
  future partitioner engine choosing between real LEF/DEF input and a
  synthetic netlist from this engine) do
  `target_link_libraries(<engine> PRIVATE netlistgen odb utl)` and call
  `NetlistBuilder`/`generateSynthetic` in-memory directly. Verify the
  current `CMakeLists.txt` explicitly — don't assume it already does this;
  fix it if it currently only produces a test binary.
- Add a **library-linkage smoke test**: confirm `test/netlistgen_test.cpp`
  (or a small dedicated target) links against `netlistgen` as an external
  consumer would (`target_link_libraries(... PRIVATE netlistgen odb utl)`),
  not by compiling netlistgen's sources directly into the test binary.

## 3. Core refactor: pin access by IoType, not by name

Current net-generation logic assumes synthetic masters' fixed pin-naming
convention (`i0..iN-1` / `o0..oM-1`). This must change to iterate each
master's `dbMTerm` list and branch on `dbMTerm::getIoType()`
(INPUT/OUTPUT/INOUT) instead of parsing names. This makes the same
generation algorithm work unmodified today (synthetic masters only — LEF
loading is Stage B) and sets up Stage B's LEF-backed masters (arbitrary pin
names/counts) to work without further changes to this logic.

Confirm exact `dbMTerm`/`dbIoType` API at the pinned OpenROAD SHA before
implementing — check `third_party/openroad/src/odb/include/odb/db.h`.

**Note for Stage B's benefit**: every combinational master is assumed to
have exactly **one** output pin and `(pin_count - 1)` input pins — this
isn't used by anything in Stage A, but confirm the refactor doesn't
implicitly assume something incompatible with that (e.g. don't hardcode
"first pin is driver" in a way that breaks for masters with a different
output-pin position).

## 4. Non-negotiable regression-safety requirement — acceptance gate

**Output for a given `(spec, seed)` must be bit-identical to current
behavior before and after this stage's changes.** The existing
`test/netlistgen_test.cpp` suite must pass unmodified (only path/namespace
updates allowed, no behavioral changes) — this is the proof.

**Do not start Stage B in this session.** Report back once:
- The directory move is committed and the build is green from a fresh
  clone/container rebuild.
- The library-linkage smoke test (Section 2) passes.
- `test/netlistgen_test.cpp` passes unmodified with identical output for
  known `(spec, seed)` pairs.

## 5. Docs for this stage

- Create **`src/engines/netlistgen/README.md`** (initial version — will be
  extended in Stages B/C/D per the project's same-commit FLOW.md/README
  convention): describe the engine's current scope as of this stage (fast
  synthetic-only netlist generation for Stage 1/2 test fixtures), note that
  LEF-backed generation, statistical cell mix, loop avoidance, and output
  writers are landing in subsequent stages (list them so a reader
  mid-migration understands the state).
- Create **`src/engines/netlistgen/FLOW.md`** per the FLOW.md convention:
  per-file Mermaid diagrams for `NetlistBuilder`/`generateSynthetic` as
  they exist after the IoType refactor, + one engine-level flow diagram.
- Update `test/README.md` to note netlistgen's relocation to
  `src/engines/` (full "promoted to engine status" narrative can wait for
  Stage D once the engine actually has its full feature set).

## 6. Explicitly out of scope for this stage

- LEF loading, statistical cell mix, `SyntheticNetlistSpec` field
  additions — Stage B.
- Combinational-loop avoidance — Stage C.
- DEF/`.odb` writers, CLI executable — Stage D.
- Primary I/O ports, Verilog writer — Stage E.
