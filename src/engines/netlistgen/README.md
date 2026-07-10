# src/engines/netlistgen/

Synthetic netlist generation engine. Builds `dbBlock`s through OpenDB API
calls — optionally backed by real LEF cells — so tests and benchmarks can
create netlists of any size with exactly known or statistically controlled
topology, then feed them to `Hypergraph::buildFromBlock()`.

> **Migration status (Stage C of 5).** Stage A promoted `netlistgen` from a
> Stage 1/2 test utility into an engine and made pin access IoType-based.
> Stage B added LEF-backed masters (real cells) and a statistical cell mix
> (forward + max-entropy modes). **Stage C (this stage)** adds DEF / `.odb`
> writers, a net well-formedness validation pass, and a standalone
> JSON-driven CLI executable (`netlistgen_cli`). Still landing later:
> - **Stage D** — combinational-loop avoidance (provably acyclic net
>   formation). **Until it lands the generator can produce combinational
>   cycles** — net formation still pairs drivers/sinks from shuffled pools.
> - **Stage E** — primary I/O ports and a Verilog writer.
>
> ⚠️ **Loop-avoidance caveat (read before using generated files).** Stage C's
> writers and CLI produce output that is structurally **well-formed** (every
> net has exactly one driver, ≥1 sink, no dangling nets — enforced by the
> validation pass below) but **not yet guaranteed free of combinational
> loops**. That guarantee is Stage D. Treat DEF / `.odb` files from this stage
> as **preview / manual-inspection artifacts** for getting a feel for the
> generator's output shape — **not** as valid fixtures for Stage 3 or any
> other downstream flow. Stage C does **not** complete "Phase 1"; Stage D
> still needs to land first. (Stage C was deliberately sequenced ahead of
> Stage D to get hands-on CLI output sooner — the reordering is safe because
> loop avoidance depends only on Stage B's work, not on this stage.)
>
> See `FLOW.md` for algorithmic flow diagrams.

## What the engine does

Two layers, both in `netlistgen.h` / `netlistgen.cpp`:

- **`NetlistBuilder`** owns a fresh `dbDatabase`. Its tech/lib/chip/block are
  created either:
  - *synthetically* — connectivity-only masters (no geometry), input pins
    `i0..iN-1` / output pins `o0..oM-1`, built lazily on first `makeMaster`
    and frozen explicitly (OpenDB requires a frozen master before
    `dbInst::create`); or
  - *from LEF* — `loadLef(tech_lef, cell_lefs)` loads a real technology and
    cell library through `odb::lefin`. `lefin` freezes each MACRO as it
    parses, so no explicit freeze is needed on this path. A given builder is
    one or the other, never both.

  `estimateDieArea(num_insts, utilization)` auto-sizes a near-square
  placement region from the instance count and the loaded tech's site pitch
  (nominal pitch if none), records it on the block, and returns the box.
  Instances stay `UNPLACED`; the DEF writer consumes it for the `DIEAREA`.
  (Synthetic tech now sets 2000 DBU/µm so DefOut's unit scaling is defined;
  LEF mode inherits real units from the tech LEF.)

- **`generateSynthetic(builder, spec)`** populates the block from a
  `SyntheticNetlistSpec` using a seeded `std::mt19937`. Two mix regimes:
  - **Legacy weighted mix (Stage A).** When none of the statistical/LEF
    fields are set, cells are drawn from the explicit `masters` weighted
    list exactly as in Stage A — **output is bit-identical** for a given
    `(spec, seed)`.
  - **Statistical mix (Stage B).** Engaged when any statistical field (or a
    LEF path) is set. Each instance is first rolled sequential vs
    combinational by `sequential_ratio`; a combinational instance then rolls
    a pin-count bucket from the effective distribution and a master uniformly
    among that bucket's cells.

  Net formation is shared by both regimes: terminals are bucketed into a
  driver pool (`OUTPUT`) and a sink pool (`INPUT`/`INOUT`) **by IoType, with
  power/ground pins excluded by `dbSigType`**; each net takes one driver and
  `fanout−1` sinks from the shuffled pools, so every iterm lands on at most
  one net. Returns the net count, or **`-1`** if the spec fails validation.

## Statistical cell-mix contract

- **Pin counting excludes power/ground.** `signalPinCount(master)` counts
  only `dbMTerm`s with `dbSigType::SIGNAL` or `CLOCK`; `VDD`/`VSS`
  (`POWER`/`GROUND`) never inflate a bucket. One shared helper used by both
  the synthetic and LEF paths.
- **Five combinational buckets** by signal-pin count: **2, 3, 4, 5, 6-or-more**
  (2-pin = buffer/inverter, 3-pin = 2-input gate, …). In synthetic mode the
  "6+" bucket is pinned to exactly 6.
- **Two mutually exclusive combinational modes** (exactly one required when
  the statistical mix is engaged; both or neither → fail fast):
  - **Mode A — forward:** `combinational_pin_distribution`, five percentages
    that must sum to 100.
  - **Mode B — inverse:** `target_avg_fanout`, the desired average
    signal-pin count. The generator back-solves the **maximum-entropy**
    distribution `p_i = exp(θ·xᵢ)/Σ exp(θ·xⱼ)` over the bucket anchors `xᵢ`
    whose weighted mean equals the target — a single scalar `θ` found by
    bisection (`maxEntropyDistribution`, plain `<cmath>`). This spreads mass
    across all buckets as evenly as the mean constraint allows, with no
    user-supplied prior. The derived distribution is logged. The target must
    lie strictly inside the anchor range (synthetic: `(2, 6)`).
- **Sequential cells** get one fixed representative profile this stage
  (synthetic: `D, CK, Q` = 3 signal pins). No pin-count distribution for the
  sequential side yet.
- **LEF-mode classification is auto-detected.** A master is *sequential* if
  any `dbMTerm` carries `dbSigType::CLOCK`; everything else is combinational
  and bucketed by signal-pin count. At spec-build time a lookup is built —
  per bucket, the matching masters; separately, the sequential class.
  - A combinational master must resolve to **exactly one output** and
    `(pin_count − 1)` inputs; **multi-output combinational masters are
    excluded** from bucket population (logged). This is a load-bearing
    assumption for Stage C's DAG net formation, enforced now.
  - A **requested** bucket (positive probability) with no matching master —
    or `sequential_ratio > 0` with an empty sequential class — is a
    **hard failure at spec-build time**, naming the empty bucket/class. No
    silent skip or weight redistribution.
  - **Nangate45 caveat:** its DFF clock pins are tagged `USE SIGNAL`, not
    `CLOCK`, so the CLOCK auto-detection finds **no** sequential cells and
    the DFFs (with `Q`+`QN`) are excluded as multi-output combinational. LEF
    tests therefore use `sequential_ratio = 0.0`.
- **`distribution_tolerance_pct`** (default 2.0): after generation, empirical
  proportions (and, in Mode B, the mean combinational signal-pin count) are
  compared to the targets. A deviation past tolerance is a **logged warning
  only, never a failure** — stochastic draws won't hit exact percentages,
  especially at small counts. Contrast the spec-build-time checks
  (distribution sum, mode exclusivity, target range, non-empty buckets),
  which stay hard failures.

## Net well-formedness validation (Stage C)

`validateNetlist(block)` (`netlist_validation.h`) is a defensive, structural
correctness check — **independent of and in addition to** Stage D's
combinational-loop-freedom guarantee. It walks every `dbNet` and confirms:

- **Exactly one driver** — exactly one connected `dbITerm` with
  `IoType::OUTPUT`. Zero or more than one is a failure.
- **At least one sink** — at least one connected `dbITerm` with
  `IoType::INPUT` (`INOUT` counts as a sink too). Zero sinks is a failure
  (a driver with nothing to drive — dangling).
- **No dangling nets** — a net with zero connected iterms is a failure.

Power/ground iterms (`dbSigType::POWER`/`GROUND`) are ignored; classification
is **IoType-based** (the Stage A refactor), never name-based. It returns a
`NetlistValidation { bool ok; std::string message; }` naming the first
offending net. This is a **hard** structural property — unlike the statistical
`distribution_tolerance_pct` checks, which are sampling-noise warnings. This
stage considers only `dbITerm`s (no primary ports yet); the per-net tally is
factored so Stage E can fold primary-input/-output `dbBTerm`s into the same
driver/sink counts. The CLI runs this automatically after generation and
**refuses to write any output if it fails** (fail-fast); Stage 3 test code or
other callers can invoke it directly on their own blocks.

## DEF / `.odb` writers (Stage C)

`netlist_writers.h` exposes two thin wrappers, callable independently of the
CLI (Stage E's Verilog writer, pybind11 bindings, or Stage 3 test code can
call them directly). Available in **both** synthetic and LEF-backed mode:

- `writeDef(block, path, logger = nullptr)` — drives `odb::DefOut` at DEF 5.8.
  No `PINS` section (no primary ports until Stage E). Returns false on write
  failure; supplies a throwaway logger when the caller passes none.
- `writeOdb(db, path)` — wraps `dbDatabase::write` (which takes a
  `std::ostream`, not a filename) in a checked `ofstream`. Returns false if
  the file can't be opened or the stream goes bad.

## Standalone CLI (`netlistgen_cli`, Stage C)

A plain C++ executable (same pattern as `hello_odb`) that reads a JSON config,
generates through the **same** `generateSynthetic` in-memory callers use,
validates well-formedness, and writes the requested outputs:

```bash
build/netlistgen_cli path/to/config.json [-verbosity <level>]
```

**JSON is an input to this executable only** — it is not part of the in-memory
API. The in-memory path (`NetlistBuilder` + `generateSynthetic`) is driven by
constructing a `SyntheticNetlistSpec` in C++; JSON parsing lives in the
separate `cli_config` translation unit (linked into the CLI and the CLI tests,
never into the `netlistgen` library). The schema is a serialization of
`SyntheticNetlistSpec` plus CLI-only I/O fields:

| JSON field | Maps to | Notes |
|------------|---------|-------|
| `instance_count` | `spec.num_insts` | **Required**, `> 0`. |
| `seed` | `spec.seed` | Optional. |
| `net_count` | `spec.num_nets` | `null`/absent → `-1` (as many as pools allow). |
| `fanout_range` `{min,max}` | `spec.min_fanout` / `max_fanout` | Optional. |
| `tech_lef_path` | `spec.tech_lef_path` | Optional; engages LEF mode. |
| `cell_lef_paths` | `spec.cell_lef_paths` | Optional array. |
| `sequential_ratio` | `spec.sequential_ratio` | Optional. |
| `combinational_pin_distribution` | `spec.combinational_pin_distribution` | Object keyed `"2","3","4","5","6+"`, sum 100. Mode A. |
| `target_avg_fanout` | `spec.target_avg_fanout` | Mode B (mutually exclusive with the distribution). |
| `distribution_tolerance_pct` | `spec.distribution_tolerance_pct` | Optional (default 2.0). |
| `output_def_path` | CLI-only | Write DEF here if set. |
| `output_odb_path` | CLI-only | Write `.odb` here if set. |

**Output-path independence:** `output_def_path` and `output_odb_path` are each
independently optional — whichever are set are written, the rest skipped — but
**at least one must be set** (fail-fast otherwise). No cell is ever named in
config: the mix is fully determined by `sequential_ratio` plus exactly one of
`combinational_pin_distribution` or `target_avg_fanout`. With no LEF fields,
generation is synthetic-only and the DEF's `DIEAREA` is auto-sized via the
nominal pitch. On success the CLI logs instance / net / pin counts (all output
is `utl::Logger`, see "Logging & verbosity" below). JSON is parsed with
**`nlohmann::json`** (header-only, pulled via CMake `FetchContent` pinned to
`v3.11.3`).

Mode A example (explicit distribution, LEF-backed, both outputs):

```json
{
  "seed": 42,
  "instance_count": 5000,
  "net_count": null,
  "fanout_range": { "min": 2, "max": 6 },
  "tech_lef_path": "data/nangate45/Nangate45_tech.lef",
  "cell_lef_paths": ["data/nangate45/Nangate45_stdcell.lef"],
  "sequential_ratio": 0.0,
  "combinational_pin_distribution": {"2":20,"3":30,"4":20,"5":20,"6+":10},
  "distribution_tolerance_pct": 2.0,
  "output_def_path": "run/generated.def",
  "output_odb_path": "run/generated.odb"
}
```

Mode B example (target average fanout, synthetic-only, DEF-only):

```json
{
  "seed": 42,
  "instance_count": 5000,
  "fanout_range": { "min": 2, "max": 6 },
  "target_avg_fanout": 3.4,
  "output_def_path": "run/generated.def"
}
```

Per the run-outputs convention, point `output_*_path` under `run/` (or a temp
path) — never the repo root or source tree.

## Logging & verbosity

All output is `utl::Logger` (repo convention — see `CLAUDE.md` /
`src/support/logging.h`). The CLI narrates the run with default-visible `info`
phase markers (parse config → generate → validate → write → done) and a final
count summary; hard errors go to stderr. `-verbosity <level>` (group
`"netlistgen"`) raises detail across the whole run: **1** adds the resolved
plan (bucket probabilities, sequential masters) and achieved-vs-requested
statistics unconditionally (not just on a tolerance miss); **2** adds
instance/net progress heartbeats every 100k; **3** adds a per-net formation
trace, capped at `eda::kTraceCap`.

The library threads the logger so in-memory callers get the same trace:
`NetlistBuilder(name, logger*)` takes an **optional external logger** (shared,
not owned; a null logger makes the builder own a fresh one as before), and
`generateSynthetic` logs through `builder.logger()`. Its phase markers are
`debugPrint` (debug-gated), so an in-memory caller is silent at verbosity 0
until it calls `setDebugLevel` (or `eda::applyVerbosity`) on the shared logger.

## Input / output contract

Standalone in-memory library: **no hypergraph attribute planes** read or
written. Input is a `SyntheticNetlistSpec`; output is a populated `dbBlock`
owned by the `NetlistBuilder`, plus the net count (or `-1` on invalid spec).
Consumed downstream by `Hypergraph::buildFromBlock()`.

## Control parameters (`SyntheticNetlistSpec`)

| Field | Type | Default | Meaning |
|-------|------|---------|---------|
| `masters` | `std::vector<MasterSpec>` | — | Legacy weighted mix (ignored in statistical mode). |
| `num_insts` | `int` | `0` (must be `> 0`) | Number of instances. |
| `num_nets` | `int` | `-1` | Net cap; `-1` = as many as the pin pools allow. |
| `min_fanout` / `max_fanout` | `int` | `2` / `4` | Pins per net, driver included. |
| `seed` | `uint32_t` | `1` | RNG seed; fixes output for a given spec. |
| `tech_lef_path` | `std::optional<std::string>` | unset | If set, load real tech (and its macros) via `lefin`. |
| `cell_lef_paths` | `std::vector<std::string>` | `{}` | Extra cell LEF(s) against that tech. |
| `sequential_ratio` | `std::optional<double>` | unset (→ 0.0) | Fraction of instances that are sequential. |
| `combinational_pin_distribution` | `std::optional<array<double,5>>` | unset | Mode A percentages `[2,3,4,5,6+]`, sum 100. |
| `target_avg_fanout` | `std::optional<double>` | unset | Mode B target mean signal-pin count. |
| `distribution_tolerance_pct` | `double` | `2.0` | Post-gen deviation warning threshold. |

`MasterSpec`: `name`, `num_inputs` (2), `num_outputs` (1), `weight` (1.0).

The statistical mix is engaged when `tech_lef_path`, `sequential_ratio`,
`combinational_pin_distribution`, or `target_avg_fanout` is set
(`SyntheticNetlistSpec::usesStatisticalMix()`).

## Determinism

Output for a given `(spec, seed)` is reproducible across runs on a fixed
toolchain: the generator draws from a seeded `std::mt19937` and instance /
pin iteration order is fixed by OpenDB set order. The legacy path is
additionally bit-identical to Stage A. Determinism carries through the CLI: a
given JSON config produces the same block — and hence the same DEF / `.odb` —
each run. Scale reference: **~500k insts / ~1.4M pins in ~2 s** for synthetic
mode; LEF-backed mode has not been separately benchmarked at that scale yet.

## How to run

```cmake
target_link_libraries(<your_target> PRIVATE netlistgen odb utl)
```

Synthetic statistical mix (Mode A):

```cpp
eda::NetlistBuilder nb;
eda::SyntheticNetlistSpec spec;
spec.num_insts = 10000;
spec.sequential_ratio = 0.2;
spec.combinational_pin_distribution = std::array<double,5>{30,25,20,15,10};
const int nets = eda::generateSynthetic(nb, spec);   // -1 on bad spec
```

LEF-backed, Mode B (max-entropy):

```cpp
eda::NetlistBuilder nb("lefdesign");
eda::SyntheticNetlistSpec spec;
spec.tech_lef_path  = "data/nangate45/Nangate45_tech.lef";
spec.cell_lef_paths = {"data/nangate45/Nangate45_stdcell.lef"};
spec.num_insts = 500;
spec.sequential_ratio = 0.0;      // Nangate45 tags no CLOCK pins
spec.target_avg_fanout = 3.5;
const int nets = eda::generateSynthetic(nb, spec);
eda::Hypergraph hg;
hg.buildFromBlock(nb.block());
```

Write outputs and validate (writers + validation are library functions):

```cpp
eda::NetlistBuilder nb;
eda::SyntheticNetlistSpec spec;   // ... populate ...
eda::generateSynthetic(nb, spec);
nb.estimateDieArea(spec.num_insts);            // size DIEAREA for DEF
if (eda::validateNetlist(nb.block()).ok) {     // structural well-formedness
  eda::writeDef(nb.block(), "run/out.def", nb.logger());
  eda::writeOdb(nb.db(), "run/out.odb");
}
```

### Tests

```bash
cmake -B build
cmake --build build --target netlistgen_test netlistgen_stageb_test \
      netlistgen_stagec_test netlistgen_link_smoke netlistgen_cli
ctest --test-dir build -R "netlistgen" --output-on-failure
```

- `test/netlistgen_test.cpp` — Stage A behavior (no data files): exact CSR on
  a hand-built 3-inst/2-net case, spec conformance, net-count limiting, seed
  determinism.
- `test/netlistgen_stageb_test.cpp` — Stage B (needs `EDA_LAB_DATA_DIR`):
  max-entropy solve correctness, spec-config validation (both/neither mode,
  bad sum, out-of-range target), signal-pin counting on a Nangate45 NAND2,
  LEF-backed generation, multi-output exclusion, empty-bucket/empty-sequential
  fail-fast (using `data/synth_cells/twobucket.lef`), large-run statistical
  validation, Mode B mean, determinism, and die-area sizing.
- `test/netlistgen_stagec_test.cpp` — Stage C (needs `EDA_LAB_DATA_DIR` and the
  built `netlistgen_cli` binary): well-formedness passing on synthetic +
  LEF-backed output and flagging hand-built dangling / driverless /
  multi-driver / sinkless nets; DEF + `.odb` writers producing files; JSON
  parsing (Mode A/B valid, missing `instance_count`, no output path,
  malformed JSON); the CLI validate-before-write fail-fast on a malformed
  block; and a CLI smoke test that spawns `netlistgen_cli`, round-trips the
  DEF back through `defin`, and confirms instance/net counts.
- `test/netlistgen_link_smoke.cpp` — library-linkage guard.

Scale reference: ~500k insts / ~1.4M pins generate in about 2 s (synthetic).

## Open questions / follow-on

- **Stage D** — combinational-loop avoidance (the completion gate for Phase 1).
- **Stage E (not yet implemented)** — primary I/O ports (`PINS` section,
  primary-input `dbBTerm` drivers / primary-output `dbBTerm` sinks folded into
  `validateNetlist`) and a Verilog writer, plus `output_verilog_path` and
  primary-port fields on the CLI config.
- Custom prior distribution for Mode B's max-entropy tilt (currently uniform).
- YAML config support alongside JSON.
