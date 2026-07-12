# Spike Brief — Stage E of 5: Primary I/O Ports + Verilog Writer

**Target directory:** `src/engines/netlistgen/`
**Prerequisites:** Stage C (DEF/`.odb` writers, CLI executable) and Stage D
(combinational-loop avoidance) both complete and their acceptance gates
green — DEF/`.odb` writers and the CLI executable exist and are tested,
and net formation is now guaranteed loop-free. Phase 1 is
done; Stage 3 is unblocked independent of this stage.
**This stage is optional relative to Stage 3** — it exists to turn the
generator into a general-purpose standalone test-case producer for
OpenROAD or any other EDA tool (Verilog + DEF + the input LEF as a matched
triplet), not because Stage 3 needs it.

---

## 1. Goal for this stage

1. Add top-level primary I/O ports (`dbBTerm`) so generated designs support
   full synthesis/PnR-flow testing, not just internal partitioner testing.
2. Add a structural Verilog netlist writer, gated to LEF-backed mode only.
3. Extend the CLI and docs accordingly.

## 2. New: primary I/O ports (`dbBTerm`)

- New spec fields: `primary_input_count` (`int`, default 0),
  `primary_output_count` (`int`, default 0).
- **Primary inputs act as net drivers**, exactly like sequential Q outputs
  in Stage D's loop-avoidance scheme — they are valid sources from the
  start, for any combinational instance regardless of creation order. Each
  of the `primary_input_count` ports becomes the driver of one newly-formed
  net; receivers are drawn from the same eligible-candidate pool logic as
  Stage D, unchanged.
- **This relaxes Stage D's `sequential_ratio > 0` requirement**: once
  `primary_input_count > 0` is available as an alternate bootstrap source,
  `sequential_ratio == 0` becomes valid too. Update the spec-build
  validation to: fail fast only if **both** `sequential_ratio <= 0` **and**
  `primary_input_count <= 0` (no bootstrap source at all).
- **Primary outputs act as extra net sinks.** Each of the
  `primary_output_count` ports is appended as an additional terminal on an
  existing, already-formed net (selected uniformly at random, seeded).
  Unlike primary inputs, these do **not** consume an instance iterm — a
  `dbBTerm` is a block-level terminal, not tied to a master pin, so this
  doesn't interact with the "every iterm used at most once" invariant, and
  has no loop-avoidance implication (it's a pure sink, same as a sequential
  D pin in that respect).
- Naming convention: `pi0..N-1` for inputs, `po0..M-1` for outputs (adjust
  if project convention differs — flag as a small open choice, not a
  blocker).
- **DEF PINS section requires geometry** (placement/status) for each
  `dbBTerm`, similar to the DIEAREA situation for instances (Stage B).
  Verify the exact minimum required fields at the pinned SHA (check whether
  `UNPLACED` status is legal for `PINS`, same as instances, or whether pins
  specifically need a placed location) — do not assume; this is a
  do-not-guess item like the other ODB quirks already documented in project
  memory. Use a simple placement scheme (e.g. evenly spaced along one die
  edge) if placement turns out to be required.
- Defaults of `0`/`0` must preserve Phase 1 (Stages A–D) output exactly.

## 3. New: Verilog netlist writer (LEF-backed mode only)

- New output artifact: a structural Verilog netlist (`.v`), gated behind
  LEF mode — **fail fast at spec-build time if `output_verilog_path` is set
  but no `tech_lef_path`/`cell_lef_paths` are present**, with a clear
  message explaining Verilog requires real cell names to reference as
  module types. Synthetic-mode masters have no synthesizable-cell identity
  to emit.
- Format: plain structural Verilog —
  ```verilog
  module <top_module_name> (pi0, pi1, ..., po0, po1, ...);
    input pi0, pi1, ...;
    output po0, po1, ...;
    wire net_0, net_1, ...;

    BUF_X1 inst_0 ( .A(net_0), .Z(net_1) );
    AND2_X1 inst_1 ( .A1(net_1), .A2(net_2), .ZN(net_3) );
    ...
  endmodule
  ```
  Cell type = the real LEF master name; port names = the real `dbMTerm`
  names (both already resolved via Stage A's IoType refactor, so no new
  lookup logic needed here — the Verilog writer just walks the same
  `dbInst`/`dbITerm`/`dbNet` structures the DEF and odb writers already
  walk).
- **Name consistency is a design guarantee, not just a nice-to-have**:
  since `.v`, `.def`, and `.odb` are all written from the same in-memory
  `dbBlock`, instance names (`dbInst::getName()`) and net names
  (`dbNet::getName()`) are identical across all three by construction. This
  is what makes the output usable as a real LVS-checkable test-case triplet
  for other EDA tools — state this explicitly in the README and add a test
  verifying it (Section 5).
- Top module name: derive from a spec field or CLI config
  (`top_module_name`, optional, default something reasonable like
  `"generated_top"`) rather than hardcoding.
- Verilog writer is a new file in the engine (e.g. `VerilogOut.h/.cpp`),
  exposed as an engine function like the DEF/odb writers — callable
  independently of the CLI.
- Behavioral/functional Verilog is out of scope — structural connectivity
  only, matching what the generator actually knows (topology, not
  function).

## 4. CLI extension

**Note on the established CLI-help convention**: the repo-wide `--help`/usage
convention (see `repo-wide-cli-help-convention.md`) chose Option B — an
internal shared helper, not an external CLI-parsing library. If this
stage's CLI extension adds any new `argv`-level options (as opposed to
just new JSON config fields, which is what most of the additions below
actually are), use that same shared helper rather than hand-rolling
`--help`/usage text for the new options. Most of what follows is JSON
schema extension, not new `argv` flags, so this may not end up applying —
just don't reintroduce a second convention if it does.

- Add `output_verilog_path` (optional) — independently optional alongside
  `output_def_path`/`output_odb_path`; DEF and `.odb` have no gating
  condition relative to each other or to LEF mode, `output_verilog_path`
  additionally requires LEF mode per Section 3.
- Add `primary_input_count`, `primary_output_count`, `top_module_name` to
  the config schema.

  Full example (all three outputs, primary ports, LEF-backed):
  ```json
  {
    "seed": 42,
    "instance_count": 5000,
    "net_count": null,
    "fanout_range": { "min": 2, "max": 6 },
    "tech_lef_path": "data/nangate45/NangateOpenCellLibrary.tech.lef",
    "cell_lef_paths": ["data/nangate45/NangateOpenCellLibrary.lef"],
    "sequential_ratio": 0.15,
    "combinational_pin_distribution": {
      "2": 20,
      "3": 30,
      "4": 20,
      "5": 20,
      "6+": 10
    },
    "distribution_tolerance_pct": 2.0,
    "primary_input_count": 16,
    "primary_output_count": 16,
    "top_module_name": "generated_top",
    "output_def_path": "run/generated.def",
    "output_odb_path": "run/generated.odb",
    "output_verilog_path": "run/generated.v"
  }
  ```

## 5. Tests — acceptance gate for this stage

- **Primary I/O port tests**: correct `dbBTerm` count and `IoType` for
  given `primary_input_count`/`primary_output_count`; confirm defaults of
  `0`/`0` reproduce Phase 1 output exactly; confirm primary-input-driven
  nets don't double-count against the "every iterm used once" invariant
  (ports aren't iterms); confirm relaxed bootstrap validation (Section 2) —
  `sequential_ratio == 0` now valid when `primary_input_count > 0`, still
  fails when both are zero.
- **Combinational-loop-freedom test, extended**: re-run Stage D's
  cycle-detection test with primary ports in play, treating primary inputs
  as sources (no incoming edge) and primary outputs as sinks (no outgoing
  edge) in the graph — confirm still no cycle.
- **Verilog writer tests**: gating (fails fast without LEF); generated
  module parses as syntactically valid Verilog (a lightweight check is
  fine — balanced `module`/`endmodule`, correct instance count, no need for
  a full parser); cell types in the `.v` match real LEF master names.
- **Name-consistency test (Section 3's core guarantee)**: generate once
  with all three outputs requested, confirm every instance/net name
  appearing in the `.v` also appears identically in the `.def`.
- CLI smoke test: invoke `netlistgen_cli` with a small LEF-backed config
  requesting all three outputs, verify DEF + `.odb` + `.v` all exist,
  round-trip-read the DEF back via `defin` and confirm instance/net counts
  match the config.

## 6. Docs

- Update **`src/engines/netlistgen/README.md`**: document primary port
  generation, the Verilog writer and its LEF-mode gating, the
  name-consistency guarantee, and the general-purpose standalone-test-case
  use case explicitly (Verilog + DEF + input LEF as a triplet for other EDA
  tools).
- Update **`src/engines/netlistgen/FLOW.md`**: add diagrams for primary
  port generation and the Verilog writer.

## 7. Explicitly out of scope for this stage

- pybind11 binding / `edalab run netlistgen` typer CLI integration.
- Actual placement (instances and ports stay `UNPLACED`/minimal-geometry).
- Behavioral/functional Verilog.
- Verilog for synthetic (no-LEF) mode — gated off entirely.
- YAML config support.
