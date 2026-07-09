# Flow: netlistgen Engine

The netlistgen engine (`src/engines/netlistgen/`) constructs `dbBlock`
netlists directly through the OpenDB API — no LEF/DEF — for use as
Stage 1/2 test/benchmark fixtures. It has two pieces in one translation
unit (`netlistgen.h` / `netlistgen.cpp`): `NetlistBuilder`, a thin owner of
a fresh `dbDatabase` that wraps the create/connect calls, and the free
function `generateSynthetic()`, which fills a builder's block from a
`SyntheticNetlistSpec`. This document reflects the code as of Stage A (the
IoType-based pin refactor); LEF-backed masters, statistical mixes, loop
avoidance, and writers arrive in Stages B–E.

## `netlistgen.h` — API surface

Declares the two layers plus the two spec structs. No logic lives in the
header.

```mermaid
graph TD
  subgraph Types
    MS[MasterSpec<br/>name / num_inputs / num_outputs / weight]
    SP[SyntheticNetlistSpec<br/>masters / num_insts / num_nets<br/>min_fanout / max_fanout / seed]
    MS -->|"vector<MasterSpec>"| SP
  end
  subgraph API
    NB[class NetlistBuilder<br/>makeMaster / makeInst / makeNet / connect<br/>block / db]
    GS["generateSynthetic(builder, spec) -> int"]
  end
  SP -->|input| GS
  NB -->|"populated by"| GS
```

## `netlistgen.cpp` — `NetlistBuilder`

Owns the `dbDatabase` lifetime and enforces OpenDB's create ordering. The
key constraint is the **master-freeze protocol**: every `dbMTerm` must be
created before `setFrozen()`, and a master must be frozen before any
`dbInst::create`. `makeMaster` creates `INPUT` pins `i0..iN-1` then `OUTPUT`
pins `o0..oM-1` and freezes; `connect` resolves a pin by name via
`findITerm` (a builder convenience used by hand-built tests, not by the
generator).

```mermaid
graph TD
  ctor["NetlistBuilder(design_name)"] --> mk["dbDatabase::create<br/>dbTech / dbLib / dbChip / dbBlock::create"]

  mm["makeMaster(name, nIn, nOut)"] --> c1["dbMaster::create"]
  c1 -->|nullptr| ret0[return nullptr]
  c1 --> setcore["setType(CORE)"]
  setcore --> pins["dbMTerm::create i0..iN-1 (INPUT)<br/>then o0..oM-1 (OUTPUT)"]
  pins --> frz["setFrozen()"]
  frz --> retm[return master]

  mi["makeInst(master, name)"] --> ci["dbInst::create(block, master, name)"]
  mn["makeNet(name)"] --> cn["dbNet::create(block, name)"]

  cn2["connect(inst, pin, net)"] --> fit["inst->findITerm(pin)"]
  fit -->|null| retf[return false]
  fit --> con["iterm->connect(net)"]
  con --> rett[return true]
```

## `netlistgen.cpp` — `generateSynthetic()`

The generator seeds one `std::mt19937(spec.seed)`, materializes masters and
instances, then forms nets from two shuffled pin pools. The pin
classification is the Stage A refactor: each `dbITerm` is bucketed by
`getIoType()` — **never by pin name** — so LEF-backed masters (Stage B) work
without touching this code. Every iterm is popped from a pool at most once,
so the netlist is always valid (each pin on ≤ 1 net).

```mermaid
graph TD
  seed["rng = mt19937(spec.seed)"] --> mkm["for each MasterSpec:<br/>makeMaster(...), collect weight"]
  mkm --> dd["discrete_distribution(weights)"]
  dd --> mki["for i in [0, num_insts):<br/>makeInst(masters[pick_master(rng)], u{i})"]
  mki --> classify

  subgraph classify["Classify terminals by IoType (not name)"]
    loop["for each inst, for each iterm:<br/>switch iterm.getIoType()"]
    loop -->|OUTPUT| drv["drivers.push_back"]
    loop -->|INPUT or INOUT| snk["sinks.push_back"]
    loop -->|FEEDTHRU| skip["ignore"]
  end

  classify --> shuf["shuffle(drivers, rng)<br/>shuffle(sinks, rng)"]
  shuf --> whilecond{"drivers & sinks non-empty<br/>AND (num_nets<0 or made<num_nets)?"}
  whilecond -->|no| done["return nets_made"]
  whilecond -->|yes| net["makeNet(n{made})<br/>connect drivers.back(); pop"]
  net --> fan["k = pick_fanout(rng) - 1"]
  fan --> sinkloop["for s in [0,k) while sinks non-empty:<br/>connect sinks.back(); pop"]
  sinkloop --> incr["++nets_made"]
  incr --> whilecond
```

## Engine-level flow: spec → block → hypergraph

End to end, netlistgen turns a declarative spec into a `dbBlock` that the
downstream `Hypergraph` consumes. netlistgen writes no attribute planes;
the hypergraph and later engines (e.g. partitioning) layer planes on top.

```mermaid
sequenceDiagram
  participant Caller
  participant GS as generateSynthetic
  participant NB as NetlistBuilder
  participant ODB as OpenDB (dbBlock)
  participant HG as Hypergraph

  Caller->>NB: construct (tech/lib/chip/block)
  Caller->>GS: generateSynthetic(nb, spec)
  loop masters
    GS->>NB: makeMaster (freeze protocol)
    NB->>ODB: dbMaster + dbMTerms, setFrozen
  end
  loop num_insts
    GS->>NB: makeInst(weighted pick)
    NB->>ODB: dbInst::create
  end
  GS->>ODB: read iterms, bucket by IoType
  loop until pools drain / num_nets
    GS->>NB: makeNet + connect driver + sinks
    NB->>ODB: dbNet + iterm->connect
  end
  GS-->>Caller: nets_made
  Caller->>HG: buildFromBlock(nb.block())
  HG-->>Caller: hypergraph view (vertices=insts, edges=nets)
```
