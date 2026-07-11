# Flow: netlistgen Engine

The netlistgen engine (`src/engines/netlistgen/`) constructs `dbBlock`
netlists through the OpenDB API — synthetically or backed by real LEF cells —
for use as test/benchmark fixtures. Core pieces: `NetlistBuilder`, owner of a
fresh `dbDatabase` that wraps create/connect and LEF loading, and the free
function `generateSynthetic()`, which fills a builder's block from a
`SyntheticNetlistSpec` (`netlistgen.h` / `netlistgen.cpp`). Stage C adds output
and a driver: the DEF / `.odb` writers (`netlist_writers.h/.cpp`), the net
well-formedness check (`netlist_validation.h/.cpp`), and a standalone
JSON-driven CLI (`cli_config.h/.cpp`, `netlistgen_cli.cpp`). This reflects the
code as of Stage C (LEF-backed generation + statistical cell mix + max-entropy
solve + writers + validation + CLI).

**Loop-avoidance caveat.** Combinational-loop avoidance is **Stage D** (not yet
landed): net formation still pairs drivers/sinks from shuffled pools, so
generated blocks — and any DEF/`.odb` the CLI writes — are structurally
well-formed but **may contain combinational loops**. Treat them as
preview/manual-inspection artifacts, not yet valid downstream fixtures.
Primary I/O ports and a Verilog writer are **Stage E**.

## `netlistgen.h` — API surface

Declares the two layers, the spec structs, and the shared statistical-mix
helpers (`signalPinCount`, `isSequentialMaster`, `validateSpecConfig`,
`maxEntropyDistribution`). No logic in the header.

```mermaid
graph TD
  subgraph Types
    MS[MasterSpec<br/>legacy weighted mix]
    SP[SyntheticNetlistSpec<br/>num_insts / fanout / seed<br/>tech_lef_path / cell_lef_paths<br/>sequential_ratio<br/>combinational_pin_distribution OR target_avg_fanout<br/>distribution_tolerance_pct]
    MS -->|"vector<MasterSpec>"| SP
  end
  subgraph API
    NB[class NetlistBuilder<br/>makeMaster / makeInst / makeNet / connect<br/>loadLef / masters / estimateDieArea<br/>block / db / logger]
    GS["generateSynthetic(builder, spec) -> int<br/>(-1 on invalid spec)"]
    H[helpers: signalPinCount<br/>isSequentialMaster<br/>validateSpecConfig<br/>maxEntropyDistribution]
  end
  SP -->|input| GS
  NB -->|populated by| GS
  H -->|used by| GS
```

## `netlistgen.cpp` — `NetlistBuilder`

Owns the `dbDatabase` lifetime and the two tech-setup paths. Synthetic
tech/lib/chip/block is created lazily by `ensureSyntheticTech()` on first
`makeMaster`/`makeInst`/`makeNet` (preserving Stage A's direct-use tests).
`loadLef()` takes the LEF path instead: it first prechecks each LEF path
with `std::filesystem::exists` (a missing file becomes `warn + return false`
rather than a thrown-and-crashing `lefin` error), then, inside a boundary
`try/catch`, `lefin::createTechAndLib` builds the tech (3-arg call),
`createLib` adds each cell LEF, and chip+block are created. The
`try/catch` contains a present-but-malformed LEF: OpenROAD's
`createTechAndLib` calls `logger->error()`, which throws, and catching it
here (close to the call) keeps it a `return false` instead of a segfault —
a catch further up in the CLI's `main()` does not work (see "Error
handling" in `CLAUDE.md`). LEF masters arrive already frozen from `lefin`;
synthetic masters are frozen explicitly. A builder is one path or the
other (`tech_ready_`).

```mermaid
graph TD
  ctor["NetlistBuilder(design_name, logger?)"] --> dbonly["dbDatabase::create<br/>Logger: external (shared) or owned<br/>(no tech yet)"]

  mm["makeMaster / makeInst / makeNet"] --> est{"tech_ready_?"}
  est -->|no| synth["ensureSyntheticTech():<br/>dbTech + dbLib + dbChip + dbBlock<br/>tech_ready_ = true"]
  est -->|yes| use[use existing block]
  synth --> use

  ll["loadLef(tech_lef, cell_lefs)"] --> chk{"tech_ready_?"}
  chk -->|yes| fail0["warn + return false"]
  chk -->|no| exist{"all LEF paths exist?"}
  exist -->|no| fail0
  exist -->|yes| ct["try: lefin.createTechAndLib(tech, tech_lib, tech_lef)"]
  ct -->|null| fail1["warn + return false"]
  ct -->|throws| fail1
  ct --> cl["for each cell_lef: lefin.createLib(tech, ...)"]
  cl -->|null / throws| fail1
  cl --> mkblk["dbChip::create + dbBlock::create<br/>tech_ready_ = true (masters pre-frozen)"]
  mkblk --> ok[return true]

  eda["estimateDieArea(n, util)"] --> site["site pitch from lib (LEF) or nominal"]
  site --> box["area = n*site_area/util<br/>snap to rows/cols<br/>block.setDieArea"]
```

## `netlistgen.cpp` — signal-pin counting & max-entropy solve

Shared helpers. `signalPinCount` and `signalOutputCount` count only
`SIGNAL`/`CLOCK` mterms, excluding `POWER`/`GROUND`. `isSequentialMaster` flags
a master as sequential if it has a clock pin — a `CLOCK` sig type, or (fallback
for libraries like Nangate45 that tag the clock pin `USE SIGNAL`) an input pin
whose name matches `isClockPinName` (`CK`/`CLK`/`CLOCK`/`CP`). `isLatchMaster`
flags a non-clocked master with a level-sensitive gate/enable pin
(`isLatchEnablePinName` = `G`/`GN`) — a latch, dropped entirely.
`isClockGateMaster` flags a master driving a gated-clock output
(`isGatedClockPinName` = `GCK`/`GCLK`/`ECK`) — a clock gate, also dropped even
though it has a clock pin. `bucketIndex` maps a signal-pin count to bucket 0..4.
`maxEntropyDistribution` bisects a single `theta` so the tilted distribution's
mean hits the target.

```mermaid
graph TD
  subgraph counting
    spc["signalPinCount(master)"] --> cnt["count mterms with<br/>SigType SIGNAL or CLOCK"]
    soc["signalOutputCount(master)"] --> cnt2["count OUTPUT-IoType<br/>signal mterms"]
    seqd["isSequentialMaster(master)"] --> anyck["any mterm SigType == CLOCK<br/>OR INPUT pin named CK/CLK/CLOCK/CP?"]
    latd["isLatchMaster(master)"] --> anyg["no clock pin AND<br/>INPUT pin named G/GN?"]
    cgd["isClockGateMaster(master)"] --> anygck["OUTPUT pin named GCK/GCLK/ECK?"]
    bi["bucketIndex(pins)"] --> bmap["pins<2 -> -1<br/>pins>=6 -> 4<br/>else pins-2"]
  end

  subgraph maxent["maxEntropyDistribution(anchors, target)"]
    init["lo=-50, hi=50"] --> loop["200x bisection:<br/>mean(theta) = Σ p_i·a_i via stable softmax"]
    loop --> cmp{"mean < target?"}
    cmp -->|yes| raise["lo = mid"]
    cmp -->|no| lower["hi = mid"]
    raise --> loop
    lower --> loop
    loop --> emit["theta = mid<br/>p_i = exp(theta·a_i)/Z"]
  end
```

## `netlistgen.cpp` — `generateSynthetic()` dispatch

`validateSpecConfig` runs first (config-only checks). If a LEF path is set,
`loadLef` runs before any instance. Then the spec selects the legacy or
statistical path; both end in the shared `formNets`.

```mermaid
graph TD
  gs["generateSynthetic(builder, spec)"] --> val{"validateSpecConfig?"}
  val -->|false| r1["return -1"]
  val -->|true| lef{"tech_lef_path set?"}
  lef -->|yes| load{"builder.loadLef ok?"}
  load -->|no| r1
  load -->|yes| mode
  lef -->|no| mode{"usesStatisticalMix()?"}
  mode -->|no| legacy["generateLegacy:<br/>makeMaster per MasterSpec<br/>discrete_distribution weighted picks<br/>(Stage A path; deterministic)"]
  mode -->|yes| stat["generateStatistical"]
  legacy --> fn["formNets"]
  stat --> fn
  fn --> ret["return nets_made"]
```

## `netlistgen.cpp` — statistical generation

`buildPlan` resolves the per-bucket master lists, anchors, and probabilities,
validating LEF buckets. `generateStatistical` then rolls each instance and
finishes with `formNets` and the post-generation tolerance check.

```mermaid
graph TD
  bp["buildPlan(builder, spec, plan)"] --> lm{"LEF mode?"}
  lm -->|yes| pop["populateLefBuckets:<br/>clock gate (GCK/GCLK/ECK out) -> drop (log)<br/>clock-pin (sig type or name) -> seq class<br/>latch (G/GN gate, no clock) -> drop (log)<br/>1 output + bucket by signalPinCount<br/>multi-output / no-bucket -> exclude (log)<br/>anchors = measured bucket means"]
  lm -->|no| anc["anchors = {2,3,4,5,6}"]
  pop --> rp
  anc --> rp["resolveProbabilities"]
  rp --> modeA{"Mode A?"}
  modeA -->|yes| dist["prob = distribution / 100"]
  modeA -->|no| maxe["target_pins = fanout+1 in anchor range?<br/>prob = maxEntropyDistribution(anchors, target_pins)<br/>(log derived shape)"]
  maxe -->|out of range| r2["return false -> -1"]
  dist --> chk
  maxe --> chk{"LEF mode?"}
  chk -->|yes| empty{"requested bucket empty<br/>or seq class empty?"}
  empty -->|yes| r2
  empty -->|no| okplan
  chk -->|no| reps["materialise representative<br/>COMB_k / SEQ masters"]
  reps --> okplan[plan ready]

  okplan --> gen["for each of num_insts:<br/>roll seq vs comb (sequential_ratio)<br/>comb: pick_bucket then uniform master<br/>makeInst"]
  gen --> fn2["formNets"]
  fn2 --> tol["empirical seq ratio / bucket shares /<br/>Mode-B mean vs targets<br/>-> warn if beyond tolerance (never fail)"]
```

## `netlistgen.cpp` — `formNets()` (shared)

Both regimes end here. Terminals are bucketed into driver/sink pools by
IoType, with power/ground excluded by `dbSigType`. Each net gets one driver
plus `fanout` sinks, where `fanout` is the load count (driver excluded). Every
iterm is popped at most once, so the netlist is valid (each pin on ≤ 1 net) —
**though not yet acyclic; Stage D adds that**.

```mermaid
graph TD
  classify["for each inst, for each iterm"] --> pg{"SigType POWER/GROUND?"}
  pg -->|yes| skip[ignore]
  pg -->|no| io{"IoType"}
  io -->|OUTPUT| drv["drivers.push_back"]
  io -->|INPUT/INOUT| snk["sinks.push_back"]
  io -->|FEEDTHRU| skip
  drv --> shuf
  snk --> shuf["shuffle(drivers); shuffle(sinks)"]
  shuf --> whilecond{"pools non-empty AND<br/>(num_nets<0 or made<num_nets)?"}
  whilecond -->|no| done["return nets_made"]
  whilecond -->|yes| net["makeNet; connect driver; pop"]
  net --> fan["k = pick_fanout(rng)  (fanout = loads, driver excluded)"]
  fan --> sinkloop["connect up to k sinks; pop"]
  sinkloop --> incr["++nets_made"]
  incr --> whilecond
```

## Engine-level flow: spec → block → hypergraph

End to end, netlistgen turns a declarative spec into a `dbBlock` (synthetic or
LEF-backed) that the downstream `Hypergraph` consumes. netlistgen writes no
attribute planes.

```mermaid
sequenceDiagram
  participant Caller
  participant GS as generateSynthetic
  participant NB as NetlistBuilder
  participant ODB as OpenDB (dbBlock)
  participant HG as Hypergraph

  Caller->>GS: generateSynthetic(nb, spec)
  GS->>GS: validateSpecConfig
  alt tech_lef_path set
    GS->>NB: loadLef(tech, cells)
    NB->>ODB: lefin tech + libs (frozen masters), chip, block
  end
  alt statistical mix
    GS->>GS: buildPlan (buckets, anchors, probs)
    loop num_insts
      GS->>NB: makeInst(rolled master)
      NB->>ODB: dbInst::create
    end
  else legacy weighted mix
    loop num_insts
      GS->>NB: makeInst(weighted pick)
      NB->>ODB: dbInst::create
    end
  end
  GS->>ODB: read iterms, bucket by IoType (skip power/ground)
  loop until pools drain / num_nets
    GS->>NB: makeNet + connect driver + sinks
    NB->>ODB: dbNet + iterm->connect
  end
  GS->>GS: empirical tolerance check (warn only)
  GS-->>Caller: nets_made (or -1)
  Caller->>HG: buildFromBlock(nb.block())
  HG-->>Caller: hypergraph view
```

## `netlist_validation.cpp` — well-formedness check (Stage C)

`validateNetlist(block)` walks every `dbNet` and tallies its connected
`dbITerm`s by IoType (power/ground skipped by `dbSigType`) via `tallyITerms`,
returning on the first net that breaks a structural invariant. This is a
distinct guarantee from Stage D's loop-freedom: a net can be perfectly
well-formed and still sit on a combinational cycle. The tally struct is
factored so Stage E can fold primary-input/-output `dbBTerm`s into the same
driver/sink totals before the verdict.

```mermaid
graph TD
  vn["validateNetlist(block)"] --> nul{"block null?"}
  nul -->|yes| okv["ok (nothing to check)"]
  nul -->|no| loop["for each dbNet"]
  loop --> tally["tallyITerms: for each iterm<br/>skip POWER/GROUND<br/>OUTPUT -> drivers<br/>INPUT/INOUT -> sinks<br/>++connected"]
  tally --> c0{"connected == 0?"}
  c0 -->|yes| fdangle["fail: 'net dangling'"]
  c0 -->|no| d1{"drivers != 1?"}
  d1 -->|yes| fdrv["fail: 'N drivers (expected 1)'"]
  d1 -->|no| s1{"sinks < 1?"}
  s1 -->|yes| fsnk["fail: 'no sinks'"]
  s1 -->|no| loop
  loop --> okv
```

## `netlist_writers.cpp` — DEF / `.odb` output (Stage C)

Two thin wrappers, callable independently of the CLI. `writeDef` drives
`odb::DefOut` at version 5.8 (no PINS section — no primary ports until Stage E);
it supplies a local `utl::Logger` when the caller passes none. `writeOdb` wraps
`dbDatabase::write`, which takes a `std::ostream`, in a checked `ofstream`.
(Synthetic-tech DBUs are set to 2000/µm in `ensureSyntheticTech` so DefOut's
def-units ÷ dbu-per-micron scaling is well-defined; LEF mode inherits real
units from the tech LEF.)

```mermaid
graph TD
  wd["writeDef(block, path, logger?)"] --> wdnul{"block null?"}
  wdnul -->|yes| wdf["return false"]
  wdnul -->|no| defo["DefOut(logger or local)<br/>setVersion(DEF_5_8)<br/>writeBlock(block, path)"]
  defo --> wdret["return writeBlock result"]

  wo["writeOdb(db, path)"] --> wonul{"db null?"}
  wonul -->|yes| wof["return false"]
  wonul -->|no| ofs["ofstream(path, binary)"]
  ofs --> ofsok{"open ok?"}
  ofsok -->|no| wof
  ofsok -->|yes| dbw["db->write(stream)"]
  dbw --> woret["return stream.good()"]
```

## `cli_config.cpp` / `netlistgen_cli.cpp` — the CLI (Stage C)

JSON is confined to the CLI layer — it never reaches `NetlistBuilder` /
`generateSynthetic`. `parseCliConfig` deserialises the JSON into a
`SyntheticNetlistSpec` plus CLI-only output paths, enforcing CLI-level rules
(well-formed JSON, required `instance_count`, ≥1 output path); spec-level rules
stay with `validateSpecConfig` at generation time. `runCliFromFile` is the one
pipeline: create a shared `utl::Logger` (verbosity from the `-verbosity` flag
via `applyVerbosity`) → parse → `generateSynthetic` (builder shares the logger)
→ `estimateDieArea` → `validateAndWrite` → `reportDesignSummary` (final
default-visible statistics block: cell counts comb/seq, combinational
pin-count histogram, net count, average fanout per net and a fanout histogram —
fanout meaning load pins, driver excluded, with `10-50`/`>50` bucketed) →
log done. Each step is an
`info` phase marker; `-verbosity` surfaces the library's `debugPrint` detail through
the same logger. `validateAndWrite` gates output on `validateNetlist` (a
malformed block writes **nothing**, fail-fast) and then creates each requested
output path's parent directory (with `create_directories`) if it is missing,
before writing; only a directory that genuinely cannot be created fails, with
no partial output. `main()` in `netlistgen_cli.cpp`
parses the positional config path and the optional `-verbosity <level>` flag,
then calls `runCliFromFile` inside a top-level `try/catch` backstop (see
"Error handling" in `CLAUDE.md`).

```mermaid
graph TD
  main["main(argc, argv)"] --> help{"wantsHelp (-h/--help)?"}
  help -->|yes| ph["printHelp(stdout, CliSpec); return 0"]
  help -->|no| scan["scan args: config_path + -verbosity <level>"]
  scan --> argc{"config_path set?"}
  argc -->|no| usage["printUsageError(stderr, CliSpec,<br/>'missing required argument'); return 1"]
  argc -->|yes| run

  run["runCliFromFile(path, err, verbosity)"] --> lg["utl::Logger + applyVerbosity('netlistgen', verbosity)"]
  lg --> mk1["info: Parsing JSON config"]
  mk1 --> rd{"open file?"}
  rd -->|no| e1["err; return 1"]
  rd -->|yes| parse["parseCliConfig(text)"]
  parse --> pok{"ok?"}
  pok -->|no| e1
  pok -->|yes| gen["info: Generating...<br/>generateSynthetic(builder(&logger), spec)"]
  gen --> gok{"nets >= 0?"}
  gok -->|no| e1
  gok -->|yes| die["info: Generation complete (counts)<br/>estimateDieArea(num_insts)"]
  die --> vaw["info: Running validation<br/>validateAndWrite(builder, config, err)"]
  vaw --> valid{"validateNetlist ok?"}
  valid -->|no| e1b["err 'validation failed'<br/>write nothing; return 1"]
  valid -->|yes| odir{"ensureOutputDir:<br/>create missing output dirs"}
  odir -->|create failed| e1c["err 'cannot create output directory'<br/>write nothing; return 1"]
  odir -->|ok| wdef["if output_def_path: writeDef (info: Wrote DEF)"]
  wdef --> wodb["if output_odb_path: writeOdb (info: Wrote .odb)"]
  wodb --> summ["reportDesignSummary (report):<br/>cells comb/seq · comb pin-count hist<br/>net count · avg fanout/net (driver excl)<br/>fanout hist (loads; 10-50 / &gt;50 bucketed)"]
  summ --> counts["info: Done."]
  counts --> ok0["return 0"]

  run -.->|any escaping std::exception| bck["main() catch-all:<br/>'Fatal error'; return 1"]
```

### CLI parse mapping

```mermaid
graph LR
  subgraph JSON
    j1["instance_count (required)"]
    j2["seed / net_count(null->-1)"]
    j3["fanout_range {min,max}"]
    j4["tech_lef_path / cell_lef_paths"]
    j5["sequential_ratio<br/>combinational_pin_distribution {2,3,4,5,6+}<br/>target_avg_fanout<br/>distribution_tolerance_pct"]
    j6["output_def_path / output_odb_path<br/>(CLI-only, >=1 required)"]
  end
  subgraph CliConfig
    s1["spec.num_insts"]
    s2["spec.seed / spec.num_nets"]
    s3["spec.min_fanout / max_fanout"]
    s4["spec.tech_lef_path / cell_lef_paths"]
    s5["spec.sequential_ratio / ...dist / target / tol"]
    s6["config.output_def_path / output_odb_path"]
  end
  j1 --> s1
  j2 --> s2
  j3 --> s3
  j4 --> s4
  j5 --> s5
  j6 --> s6
```
