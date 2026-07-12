# Spike Brief — Stage C of 5: DEF/`.odb` Writers, Standalone CLI, Doc Finalization

**Target directory:** `src/engines/netlistgen/`
**Prerequisites:** Stage B complete and its acceptance gate green —
LEF-backed generation and statistical cell mix (both modes, including the
max-entropy solve) are implemented and tested.
**Blocks:** Stage D (combinational-loop avoidance)
**Important caveat for this stage**: combinational-loop avoidance has
**not** landed yet — that's Stage D, which follows this one. Net formation
still uses Stage B's unconstrained random driver/receiver pairing. DEF/`.odb`
files produced by this stage's CLI **may contain combinational loops** and
should be treated as preview/manual-inspection artifacts for getting a feel
for the generator's output shape, not yet as valid fixtures for Stage 3 or
any other downstream use. This stage does **not** complete "Phase 1" —
Stage D still needs to land first. (This stage was deliberately sequenced
ahead of Stage D at the user's request, to get hands-on CLI output sooner;
the loop-avoidance algorithm itself only depends on Stage B's work, not on
this stage, so the reordering is safe — it just means the completion gate
moves to the end of Stage D instead of the end of this one.)

---

## 1. Goal for this stage

1. Add DEF and `.odb` write capability to the engine (in-memory `dbBlock*`
   generation already works from Stage B).
2. Add a net well-formedness validation check to the core library.
3. Add a standalone CLI executable driven by a JSON config.
4. Document the current state clearly, including the loop-avoidance caveat
   above.

## 2. New: DEF + `.odb` output

Add write capability to the engine (already a library target per Stage A —
these writers are additional engine functions, not CLI-only code):

- DEF: `DefOut` (capital D, capital O — already-known quirk).
- `.odb`: `dbDatabase::write` takes a stream, not a filename (already-known
  quirk) — wrap in a small helper that opens an `ofstream` and checks it.
- Both writers should be callable independently of the CLI, so Stage E's
  Verilog writer and future pybind11 bindings or Stage 3 test code can call
  them directly if needed.
- DEF and `.odb` are available in **both** synthetic and LEF-backed mode.
- No `PINS` section needed yet — no primary ports exist until Stage E.

## 3. New: net well-formedness validation

A defensive, structural correctness check — independent of and in addition
to Stage D's combinational-loop-freedom guarantee. This checks a more basic
invariant: **every net has exactly one driver and at least one sink; no net
is dangling.** This should hold today (Stage B's driver/receiver pairing
already intends this) — the point of adding an explicit check now is to
catch any generation bug before it's masked by more complex logic in Stage D,
and to have it in place as file output starts getting manually inspected.

- New function in the core library (e.g. `NetlistValidation.h/.cpp`,
  exposed the same way as the DEF/odb writers — callable independently of
  the CLI): walk every `dbNet` in the block and confirm, for each:
  - **Exactly one driver**: exactly one connected `dbITerm` with
    `IoType::OUTPUT` (using the IoType-based access from Stage A's
    refactor, not name-based). Zero drivers or more than one driver on a
    single net is a failure.
  - **At least one sink**: at least one connected `dbITerm` with
    `IoType::INPUT`. Zero sinks is a failure (a driver with nothing to
    drive — dangling).
  - **No dangling nets**: a net with zero connected iterms entirely (should
    never occur given how nets are formed, but check anyway — this is
    exactly the kind of thing a subtle generation bug could produce).
  - This stage has no primary ports yet (Stage E), so the check only needs
    to consider `dbITerm`s. Structure the function so Stage E can extend it
    to also treat primary-input `dbBTerm`s as valid drivers and
    primary-output `dbBTerm`s as valid sinks, rather than needing a
    rewrite.
- **Run this validation automatically after generation, before writing any
  output** — in both the CLI (fail fast with a clear diagnostic naming the
  offending net if validation fails, don't write partial/invalid DEF/`.odb`
  files) and expose it so Stage 3 test code or other callers can invoke it
  on their own generated blocks if they want the same guarantee.
- This is a hard failure, not a warning — unlike the statistical
  `distribution_tolerance_pct` checks (Stage B), well-formedness is a
  structural correctness property, not a sampling-noise concern.

## 4. New: standalone CLI executable

**JSON config is input to this CLI executable only** — it is not part of
the in-memory API surface. The in-memory path (`NetlistBuilder` +
`generateSynthetic`), which Stage 3 test code and other engines call
directly (per Stage A's library-target structure), is driven by
constructing a `SyntheticNetlistSpec` in C++ — no JSON, no file I/O. The
JSON schema below is a serialization of `SyntheticNetlistSpec`'s fields
plus CLI-only I/O fields (`output_def_path`, `output_odb_path`;
`output_verilog_path` and primary-port fields are added in Stage E) that
have no equivalent on the C++ struct. The CLI's job is: parse JSON →
populate a `SyntheticNetlistSpec` → call the same `generateSynthetic` that
in-memory callers use → write whichever of DEF/`.odb` were requested. One
generation code path underneath, multiple entry points into it — keep it
that way; don't let JSON parsing leak into
`NetlistBuilder`/`generateSynthetic` themselves. Structure the CLI's
config-parsing code so Stage E can add `output_verilog_path` and the
primary-port fields without restructuring this stage's work.

- New target: `netlistgen_cli` (plain C++ executable, same pattern as
  `hello_odb` — not dependent on the not-yet-built Python/typer CLI layer;
  links against the `netlistgen` library target from Stage A).
- Input: path to a JSON config file. Recommend **`nlohmann::json`**
  (header-only, pull via CMake `FetchContent` pinned to a tagged release
  from GitHub — already an allowed network domain). This is a
  recommendation, not a mandate — flag in the PR/commit message if a
  different choice is made.
- **Output path independence**: `output_def_path` and `output_odb_path`
  are each independently optional — write whichever are set, skip the
  rest. At least one must be set (spec-build-time validation, fail fast
  otherwise).
- Config schema mirrors the extended `SyntheticNetlistSpec` fields from
  Stage B, plus CLI-only I/O fields:

  Mode A example (explicit distribution):
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
    "output_def_path": "run/generated.def",
    "output_odb_path": "run/generated.odb"
  }
  ```

  Mode B example (target average fanout — mutually exclusive with
  `combinational_pin_distribution`; generator solves the max-entropy bucket
  distribution internally and logs it; DEF-only output, no LEF):
  ```json
  {
    "seed": 42,
    "instance_count": 5000,
    "net_count": null,
    "fanout_range": { "min": 2, "max": 6 },
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.4,
    "distribution_tolerance_pct": 2.0,
    "output_def_path": "run/generated.def"
  }
  ```
  No cell is ever named in config — the mix is fully determined by
  `sequential_ratio` plus exactly one of `combinational_pin_distribution`
  or `target_avg_fanout` (Stage B). (Field names are a starting proposal —
  confirm against actual current `SyntheticNetlistSpec` field names from
  Stage B before finalizing.)
- On success, print instance/net/pin counts to stdout (mirrors `hello_odb`'s
  existing sanity-check pattern).
- No LEF fields present in config → synthetic-only mode, `output_def_path`
  still valid (DIEAREA still auto-sized via nominal pitch).

## 5. Docs for this stage

- Update **`src/engines/netlistgen/README.md`**: document DEF/`.odb`
  writers and CLI usage, JSON config schema reference, determinism
  guarantee, scale reference (~500k insts / ~1.4M pins in ~2s for synthetic
  mode — note if LEF-backed mode has a different scale profile once
  measured), how it feeds `Hypergraph::buildFromBlock()`, CLI usage
  examples, and the net well-formedness guarantee (single driver, ≥1 sink,
  no dangling nets — a structural check, distinct from Stage D's
  combinational-loop-freedom guarantee). **Repeat the loop-avoidance
  caveat from the top of this brief prominently** — this is important
  enough to not bury: generated output is well-formed but not yet
  guaranteed free of combinational loops until Stage D lands. Keep the
  "Stage E (not yet implemented)" section listing primary ports and
  Verilog output as planned follow-on work. Open questions: custom prior
  distribution for Mode B's max-entropy tilt, YAML config support.
- Update **`src/engines/netlistgen/FLOW.md`**: add diagrams for the DEF/odb
  writers, the well-formedness validation pass, and the CLI entry point.
- Update `test/README.md` to note the CLI executable now exists, with the
  same loop-avoidance caveat — hold off on the "fully promoted to engine
  status, ready for Stage 3" narrative until Stage D lands.

## 6. Tests — acceptance gate for this stage

- CLI smoke test: invoke `netlistgen_cli` with a small config, verify DEF +
  `.odb` outputs exist, round-trip-read the DEF back via `defin` and
  confirm instance/net counts match the config.
- JSON config parsing tests: valid config for both Mode A and Mode B,
  missing required field, no output path set — fail fast.
- **Net well-formedness test**: generate a netlist (both synthetic and
  LEF-backed mode), run the validation function directly, confirm it
  passes cleanly on normal output. Also construct or synthesize at least
  one deliberately malformed case (e.g. a hand-built `dbBlock` with a
  driverless net, or a net with two drivers) and confirm the validation
  function correctly flags it — a validation check that's never been
  proven to actually catch a bad case isn't trustworthy.
- **CLI fail-fast test for well-formedness**: confirm the CLI refuses to
  write DEF/`.odb` output if validation fails, rather than writing a
  partial/invalid file.
- Full regression run: confirm all Stage A and Stage B tests still pass
  after this stage's additions (no accidental behavior drift from adding
  writers/CLI/validation on top).

**Report back once all of the above are green, then proceed to Stage D
(combinational-loop avoidance).** This stage does not by itself unblock
Stage 3 — see the caveat at the top of this brief.

## 7. Explicitly out of scope for this stage

- Combinational-loop avoidance — Stage D (comes next).
- Primary I/O ports, Verilog writer — Stage E.
- pybind11 binding / `edalab run netlistgen` typer CLI integration.
- YAML config support.
- Actual placement (instances stay `UNPLACED`).
- Any Stage 3 coarsening code itself — this brief only produces the
  benchmark-generation capability Stage 3 tests will consume.
