# Flow: netlistgen Engine

The netlistgen engine (`src/engines/netlistgen/`) constructs `dbBlock`
netlists through the OpenDB API — synthetically or backed by real LEF cells —
for use as test/benchmark fixtures. Two pieces in one translation unit
(`netlistgen.h` / `netlistgen.cpp`): `NetlistBuilder`, owner of a fresh
`dbDatabase` that wraps create/connect and LEF loading, and the free function
`generateSynthetic()`, which fills a builder's block from a
`SyntheticNetlistSpec`. This reflects the code as of Stage B (LEF-backed
generation + statistical cell mix + max-entropy solve). Loop-free net
formation and writers arrive in Stages C–E.

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
`loadLef()` takes the LEF path instead: `lefin::createTechAndLib` builds the
tech (3-arg call), `createLib` adds each cell LEF, then chip+block are
created. LEF masters arrive already frozen from `lefin`; synthetic masters
are frozen explicitly. A builder is one path or the other (`tech_ready_`).

```mermaid
graph TD
  ctor["NetlistBuilder(design_name)"] --> dbonly["dbDatabase::create + Logger<br/>(no tech yet)"]

  mm["makeMaster / makeInst / makeNet"] --> est{"tech_ready_?"}
  est -->|no| synth["ensureSyntheticTech():<br/>dbTech + dbLib + dbChip + dbBlock<br/>tech_ready_ = true"]
  est -->|yes| use[use existing block]
  synth --> use

  ll["loadLef(tech_lef, cell_lefs)"] --> chk{"tech_ready_?"}
  chk -->|yes| fail0["warn + return false"]
  chk -->|no| ct["lefin.createTechAndLib(tech, tech_lib, tech_lef)"]
  ct -->|null| fail1["warn + return false"]
  ct --> cl["for each cell_lef: lefin.createLib(tech, ...)"]
  cl -->|null| fail1
  cl --> mkblk["dbChip::create + dbBlock::create<br/>tech_ready_ = true (masters pre-frozen)"]
  mkblk --> ok[return true]

  eda["estimateDieArea(n, util)"] --> site["site pitch from lib (LEF) or nominal"]
  site --> box["area = n*site_area/util<br/>snap to rows/cols<br/>block.setDieArea"]
```

## `netlistgen.cpp` — signal-pin counting & max-entropy solve

Shared helpers. `signalPinCount` and `signalOutputCount` count only
`SIGNAL`/`CLOCK` mterms, excluding `POWER`/`GROUND`. `bucketIndex` maps a
signal-pin count to bucket 0..4. `maxEntropyDistribution` bisects a single
`theta` so the tilted distribution's mean hits the target.

```mermaid
graph TD
  subgraph counting
    spc["signalPinCount(master)"] --> cnt["count mterms with<br/>SigType SIGNAL or CLOCK"]
    soc["signalOutputCount(master)"] --> cnt2["count OUTPUT-IoType<br/>signal mterms"]
    seqd["isSequentialMaster(master)"] --> anyck["any mterm SigType == CLOCK?"]
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
  mode -->|no| legacy["generateLegacy:<br/>makeMaster per MasterSpec<br/>discrete_distribution weighted picks<br/>(bit-identical to Stage A)"]
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
  lm -->|yes| pop["populateLefBuckets:<br/>CLOCK-pin -> seq class<br/>1 output + bucket by signalPinCount<br/>multi-output / no-bucket -> exclude (log)<br/>anchors = measured bucket means"]
  lm -->|no| anc["anchors = {2,3,4,5,6}"]
  pop --> rp
  anc --> rp["resolveProbabilities"]
  rp --> modeA{"Mode A?"}
  modeA -->|yes| dist["prob = distribution / 100"]
  modeA -->|no| maxe["target in anchor range?<br/>prob = maxEntropyDistribution(anchors, target)<br/>(log derived shape)"]
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
IoType, with power/ground excluded by `dbSigType` — for synthetic masters
(no power pins) this is identical to Stage A, keeping legacy output
bit-identical. Every iterm is popped at most once, so the netlist is valid
(each pin on ≤ 1 net) — **though not yet acyclic; Stage C adds that**.

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
  net --> fan["k = pick_fanout(rng) - 1"]
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
