# Spike Brief — Netlistgen Stage E2: Structural Verilog Writer

## Goal

Add a structural Verilog netlist writer to netlistgen, completing the LEF-backed output
triplet (`.v` + `.def` + `.odb`). The writer walks the in-memory `dbBlock` — including
the `dbBTerm` ports created by Stage E1 — and emits syntactically valid structural Verilog
whose instance and net names are identical to those in the DEF and ODB outputs.

This is the final milestone of the original Stage E spec
(`docs/briefs/spike-brief-netlistgen-stageE-ports-verilog.md`). That document's
primary-I/O section has been superseded by the Stage E1 brief (Rent's rule generation);
this brief covers everything remaining:

- Structural Verilog writer (VerilogOut)
- `output_verilog_path` and `top_module_name` JSON/CLI fields
- LEF-mode gating
- Bootstrap relaxation for `sequential_ratio == 0`
- Name-consistency guarantee across all three output formats
- CLI and doc updates

## Source-read requirement

**Before writing a single line of implementation**, read the following in full:

1. All source files under `src/engines/netlistgen/` — understand the complete A→E1 pipeline,
   especially how the DEF and ODB writers walk `dbInst`/`dbITerm`/`dbNet`/`dbBTerm`.
   The Verilog writer follows the same traversal pattern.
2. `src/engines/netlistgen/FLOW.md` and `README.md`.
3. The ODB API for `dbMTerm` — you need real master pin names for Verilog port connections.
   Confirm these are already resolved via Stage A's LEF-backed path (cell masters loaded).
4. `docs/briefs/spike-brief-netlistgen-stageE-ports-verilog.md` — the original Stage E
   spec. Sections 1–2 are superseded by E1; Sections 3–7 are the direct source for this
   brief. Read it before starting.

Only after completing this read should any implementation begin.

## Context

- Stage E1 is complete, tested, and pushed before this brief runs. `dbBTerm` ports (PIs
  and POs) already exist in the `dbBlock` with correct `IoType` and naming (`pi0..N-1`,
  `po0..M-1` per E1 convention — confirm exact names from E1 source).
- Verilog output is LEF-backed mode only. Synthetic-mode masters have no synthesizable
  cell identity — there is nothing to emit as a module type. Fail fast at spec-build time
  if `output_verilog_path` is set without `tech_lef_path`/`cell_lef_paths`.
- The writer must not modify `dbBlock` state — read only.
- The Verilog writer is called only after `validateNetlist` passes. If validation
  fails, generation aborts before any output files are written — including `.v`.
  This ordering was established in Stage E1 and the wellformed-audit brief; do not
  move the `writeVerilog` call to before the validation gate.
- **During source read, also verify that DEF and ODB output are equally gated** —
  `validateNetlist` must run before `writeDef`, `writeOdb`, and `writeVerilog` are
  called. If DEF/ODB are currently written before validation runs, fix the ordering
  so all three outputs are blocked until the netlist is confirmed well-formed. Add a
  test that confirms no output files are written (DEF, ODB, or Verilog) when
  `validateNetlist` returns a failure.
- All new JSON fields are optional. If `output_verilog_path` is absent, no `.v` is
  written; all prior behaviour is unchanged.

## New JSON config parameters

```json
{
  "output_verilog_path": "run/generated.v",
  "top_module_name": "generated_top"
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `output_verilog_path` | string | — | Path for the `.v` output. Requires LEF mode. |
| `top_module_name` | string | `"generated_top"` | Module name in the Verilog header. |

### Validation rules

- If `output_verilog_path` is set but no `tech_lef_path` / `cell_lef_paths` → non-OK
  Status at spec-build time, clear message: `"Verilog output requires LEF-backed mode"`.
- `top_module_name` must be a valid Verilog identifier (alphanumeric + underscore, not
  starting with a digit). Validate and error if malformed.

## Bootstrap relaxation

The original Stage D validation required `sequential_ratio > 0` as the only available
bootstrap source (registers seed the combinational driver pool). Stage E1 added PI ports
as an alternate bootstrap source.

**Verify during source read** whether Stage E1 already updated this validation. If not,
update it here:

```
fail fast only if:
    sequential_ratio <= 0  AND  rent_k / rent_p absent (no PI ports will be generated)
```

If `rent_k`/`rent_p` are set (PI ports will be created by E1), `sequential_ratio == 0`
must be accepted. Add a test for this case (see Section 5).

## Verilog writer implementation

### New files

- `src/engines/netlistgen/VerilogOut.h` — declares a single function:
  ```cpp
  namespace netlistgen {
  // Writes a structural Verilog netlist for the given block.
  // Returns non-OK Status on I/O or formatting error.
  utl::Status writeVerilog(odb::dbBlock*     block,
                           const std::string& path,
                           const std::string& top_module_name,
                           utl::Logger*       logger);
  }
  ```

- `src/engines/netlistgen/VerilogOut.cpp` — implementation. Do not expose any other symbols.

### Output format

```verilog
// Generated by eda-lab netlistgen
module <top_module_name> (
    <pi0>, <pi1>, ..., <po0>, <po1>, ...
);
    input  <pi0>;
    input  <pi1>;
    ...
    output <po0>;
    output <po1>;
    ...

    wire <net_0>;
    wire <net_1>;
    ...

    <CellMasterName> <inst_name> (
        .<mterm_name>(<net_name>),
        ...
    );
    ...

endmodule
```

**Rules:**

- Port list in module header: all `dbBTerm` names, inputs first then outputs, in creation
  order (or alphabetical — follow whatever order `dbBlock::getBTerms()` returns; document
  the choice in a comment).
- `wire` declarations: one per `dbNet`. Do not emit wires for nets that are directly a
  `dbBTerm` (top-level ports are already declared as input/output). Check via
  `dbNet::getBTerms().empty()` — if false, the net is a port net; skip the `wire`
  declaration for it.
- Instance instantiation: iterate `dbBlock::getInsts()`. For each instance:
  - Cell type = `dbInst::getMaster()::getName()`
  - Instance name = `dbInst::getName()`
  - Port connections = iterate `dbInst::getITerms()`. For each connected `dbITerm`:
    `.mterm_name(net_name)` where `mterm_name = dbITerm::getMTerm()::getName()` and
    `net_name = dbITerm::getNet()::getName()`. Skip unconnected iterms (net is null).
- Boundary buffer/FF cells created by E1 (marked with `hgm.is_boundary_buf` /
  `hgm.is_boundary_reg`) appear as regular instance instantiations. No special casing.
- Use `std::ofstream` for output. Flush and close before returning Status.
- On any write error, return non-OK Status with the OS error string.

### Name-consistency guarantee

Since `.v`, `.def`, and `.odb` are all written from the same in-memory `dbBlock`,
`dbInst::getName()` and `dbNet::getName()` are identical across all three outputs by
construction. **State this explicitly in the README** and add a test verifying it
(Section 5). This is the primary value of the triplet for external EDA tool validation.

## Full example JSON config (all three outputs, LEF-backed)

```json
{
  "seed": 42,
  "instance_count": 5000,
  "net_count": null,
  "fanout_range": { "min": 2, "max": 6 },
  "tech_lef_path": "data/nangate45/NangateOpenCellLibrary.tech.lef",
  "cell_lef_paths": ["data/nangate45/NangateOpenCellLibrary.lef"],
  "sequential_ratio": 0.15,
  "target_avg_fanout": 4.0,
  "rent_k": 2.5,
  "rent_p": 0.60,
  "io_input_ratio": 0.60,
  "top_module_name": "generated_top",
  "output_def_path": "run/generated.def",
  "output_odb_path": "run/generated.odb",
  "output_verilog_path": "run/generated.v"
}
```

## Files to modify

Based on your source read, identify the minimal set. Expected:
- New `src/engines/netlistgen/VerilogOut.h` and `VerilogOut.cpp`
- JSON config struct and parser (two new fields + LEF-mode gating validation)
- Main pipeline file — call `writeVerilog` after DEF/ODB writers when path is set
- Stage D spec-build validation (bootstrap relaxation — only if E1 didn't already handle it)
- `CMakeLists.txt` — add `VerilogOut.cpp` to the build target
- `README.md` and `FLOW.md` — updates per Section 6 below

## Tests

Add to the existing netlistgen test suite:

1. **No Verilog params** — all existing tests pass unchanged. No `.v` file created.

2. **LEF-mode gating** — `output_verilog_path` set without `tech_lef_path` → non-OK
   Status at spec-build time, no files written.

3. **Basic Verilog output** — LEF-backed config with `output_verilog_path`, default
   all-combinational `io_pin_type_distribution`:
   - File is created and non-empty.
   - Contains exactly one `module` and one `endmodule`.
   - Instance count in `.v` equals `instance_count` from config (no boundary buffer/FF
     cells with all-combinational pin type distribution). If `io_pin_type_distribution`
     has non-zero `buffered` or `registered`, instance count will be higher than
     `instance_count` by the number of boundary cells inserted by E1.
   - Every PI/PO name from E1 appears in the port list.
   - No syntax errors detectable by lightweight checks (balanced parens, no stray semicolons
     after `endmodule`, etc.).

4. **Name-consistency test** — generate with all three outputs (`output_def_path`,
   `output_odb_path`, `output_verilog_path`):
   - Parse all instance names from `.v` (lines matching `<MASTER> <inst> (`).
   - Parse all instance names from `.def` (lines matching `- <inst> <MASTER>`).
   - Assert the two sets are identical.
   - Repeat for net names (`wire <net>` in `.v` vs `- <net>` in `.def`).

5. **Cell master names** — all master names in `.v` are valid LEF master names present
   in the loaded cell library (iterate `dbBlock::getInsts()`, compare to `.v` output).

6. **Top module name** — custom `top_module_name: "my_block"` appears in `module my_block`.
   Invalid identifier `"0bad"` → non-OK Status.

7. **Bootstrap relaxation** — `sequential_ratio: 0.0` with `rent_k`/`rent_p` set:
   - No validation error (PI ports provide bootstrap).
   - Netlist generates successfully.
   - `sequential_ratio == 0.0` without `rent_k`/`rent_p` → error (no bootstrap source).

8. **CLI smoke test** — invoke `netlistgen_cli` with a small LEF-backed config requesting
   all three outputs; verify DEF + `.odb` + `.v` all exist; round-trip-read the DEF back
   via `defin` and confirm instance/net counts match the config.

All tests must pass under `ctest` before committing.

## Docs

- **`README.md`**: document `output_verilog_path`, `top_module_name`, LEF-mode gating,
  name-consistency guarantee, and the general-purpose standalone test-case use case
  (Verilog + DEF + LEF as a triplet for external EDA tools — OpenROAD, Yosys, etc.).
- **`FLOW.md`**: add the Verilog writer as the final step in the Stage E pipeline diagram,
  after DEF and ODB writers.

## Out of scope

- Behavioral / functional Verilog.
- Verilog for synthetic (no-LEF) mode.
- pybind11 binding / `edalab run netlistgen` typer CLI integration.
- Actual placement (instances and ports remain `UNPLACED`).
- YAML config support.

## Deliverables checklist

- [ ] Source read and original Stage E brief read before any implementation
- [ ] `VerilogOut.h` and `VerilogOut.cpp` implemented and added to CMakeLists
- [ ] `output_verilog_path` and `top_module_name` JSON fields parsed and validated
- [ ] LEF-mode gating enforced at spec-build time
- [ ] Bootstrap relaxation verified/implemented (sequential_ratio==0 valid when E1 PI ports present)
- [ ] Writer produces correct structural Verilog (ports, wires, instances, connections)
- [ ] Name-consistency guarantee holds across `.v`, `.def`, `.odb`
- [ ] All existing netlistgen tests green (no regression)
- [ ] All new E2 test cases green
- [ ] `README.md` and `FLOW.md` updated
- [ ] Committed with message `netlistgen: Stage E2 structural Verilog writer`

## Hard gate

This is the final netlistgen stage. All tests green, commit clean.
After this gate, the netlistgen engine is feature-complete through Stage E.
The next work stream is `src/hg_metrics/` (hg_metrics briefs C1→C4, T0→T4).
