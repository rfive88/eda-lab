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
code as of Stage E1 (LEF-backed generation + statistical cell mix +
max-entropy solve + writers + validation + CLI + acyclic net formation + peak
fanout sub-clusters + primary I/O generation via Rent's rule).

**Combinational-loop freedom (Stage D).** Statistical-mix net formation is
**acyclic by construction**: `formNetsAcyclic` reuses the instance creation
index as a topological order and filters receiver eligibility so every
comb→comb edge goes strictly forward (see its section below). This requires
`sequential_ratio > 0` (fail-fast in `validateSpecConfig`) — sequential Q
outputs bootstrap the combinational DAG; **this is unaffected by Stage E1**
(below) since primary I/O ports run as a separate pass after formation
completes, never participating in the DAG bootstrap. The legacy weighted mix
keeps the original shuffled-pool `formNets` and makes no acyclicity
guarantee.

**Peak fanout sub-clusters** (optional, layered on Stage D — see its own
section below): `assignPeakClusters` groups a subset of instances into
`num_peak_clusters` clusters once per generation; `formNetsAcyclic`'s net
formation then biases receiver selection toward the driver's own cluster
for a fixed fraction of sink slots, but **only ever within the pools Stage D
already treats as eligible** — so cluster preference can bias *which*
eligible receiver is picked, never make an ineligible one eligible, and the
DAG/loop-freedom guarantee above is completely unaffected.

**Primary I/O generation via Rent's rule (Stage E1)**, `applyPrimaryIoStageE1`
— a separate pass over the already-formed `dbBlock`, run once
`formNetsAcyclic` returns (see its own section below). Sizes a target PI/PO
terminal count from `T = k·Gᵖ`, randomly samples that many nets to become
boundary-visible, and inserts combinational/buffered/registered pin types.
The one hard invariant this pass must preserve — "exactly one driver per
net" (`validateNetlist`, now `dbBTerm`-aware) — is why a PI **replaces**
its selected net's existing driver rather than being added "alongside" it
(see the section below for the full rationale); a PO has no such conflict
(one more sink/observer on an already-driven net is always fine). Reuses
Stage E1's already-assigned `cluster_id` (if peak fanout sub-clusters are
also engaged) for per-cluster + background Rent statistics. `netlistgen`
never touches the `Hypergraph` engine (unchanged since Stage A) — Stage E1
returns raw `dbNet*`/`dbInst*` lists via `RentStats` instead of hypergraph
planes; see its section below.

## `netlistgen.h` — API surface

Declares the two layers, the spec structs, and the shared statistical-mix
helpers (`signalPinCount`, `isSequentialMaster`, `validateSpecConfig`,
`maxEntropyDistribution`, `assignPeakClusters`). No logic in the header.

```mermaid
graph TD
  subgraph Types
    MS[MasterSpec<br/>legacy weighted mix]
    IOD[IoPinTypeDistribution<br/>combinational / buffered / registered]
    SP[SyntheticNetlistSpec<br/>num_insts / fanout / seed<br/>tech_lef_path / cell_lef_paths<br/>sequential_ratio<br/>combinational_pin_distribution OR target_avg_fanout<br/>distribution_tolerance_pct<br/>peak_avg_fanout / peak_cluster_pct / num_peak_clusters<br/>rent_k / rent_p / io_input_ratio / io_pin_type_distribution]
    MS -->|"vector<MasterSpec>"| SP
    IOD -->|"optional"| SP
    CRS[ClusterRentStats]
    RS[RentStats<br/>target+actual Rent, pin-type counts,<br/>pi_nets/po_nets/boundary_*_insts,<br/>cluster_rent / background]
    CRS -->|"vector"| RS
  end
  subgraph API
    NB[class NetlistBuilder<br/>makeMaster / makeInst / makeNet / connect<br/>loadLef / masters / estimateDieArea<br/>block / db / logger]
    GS["generateSynthetic(builder, spec,<br/>out_cluster_id?, out_rent_stats?) -> int<br/>(-1 on invalid spec)"]
    H[helpers: signalPinCount<br/>isSequentialMaster<br/>validateSpecConfig<br/>maxEntropyDistribution<br/>assignPeakClusters]
  end
  SP -->|input| GS
  NB -->|populated by| GS
  H -->|used by| GS
  GS -.->|"optional out-params"| RS
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
`loadLef` runs before any instance. Then the spec selects the legacy path
(ending in the shuffled-pool `formNets`) or the statistical path (ending in
the Stage D ordered `formNetsAcyclic`).

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
  legacy --> fn["formNets (shuffled pools,<br/>no acyclicity guarantee)"]
  stat --> fna["formNetsAcyclic (Stage D:<br/>ordered, loop-free by construction)"]
  fn --> ret["return nets_made"]
  fna --> ret
```

Note `validateSpecConfig` (run first) also enforces the Stage D
bootstrap-source rule: a statistical spec with `sequential_ratio <= 0`
(unset counts as 0) fails fast — sequential Q outputs are the only signal
source that can start the combinational DAG until Stage E's primary inputs.

## `netlistgen.cpp` — statistical generation

`buildPlan` resolves the per-bucket master lists, anchors, and probabilities,
validating LEF buckets. `generateStatistical` then rolls each instance and
finishes with `formNetsAcyclic` (Stage D), the post-generation tolerance
check, and — if `rent_k`/`rent_p` are set — `applyPrimaryIoStageE1`
(Stage E1).

```mermaid
graph TD
  bp["buildPlan(builder, spec, plan)"] --> lm{"LEF mode?"}
  lm -->|yes| pop["populateLefBuckets:<br/>clock gate (GCK/GCLK/ECK out) -> drop (log)<br/>clock-pin (sig type or name) -> seq class<br/>latch (G/GN gate, no clock) -> drop (log)<br/>1 output + bucket by signalPinCount<br/>multi-output / no-bucket -> exclude (log)<br/>anchors = measured bucket means"]
  lm -->|no| anc["anchors = {2,3,4,5,7}<br/>(6+ bucket = 7-pin rep, fanout 6)"]
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
  gen --> fn2["formNetsAcyclic (Stage D)"]
  fn2 --> tol["empirical seq ratio / bucket shares /<br/>Mode-B mean vs targets<br/>-> warn if beyond tolerance (never fail)"]
  tol --> e1["applyPrimaryIoStageE1(plan, cluster_id)<br/>(no-op unless rent_k/rent_p set)"]
  e1 --> ret2["return nets_made<br/>(+ out_cluster_id / out_rent_stats if requested)"]
```

The cell mix is decided entirely before net formation, so Stage D's ordered
formation cannot disturb the empirical proportions the tolerance check
measures; Stage E1 runs even later still (after that check), so it cannot
disturb them either.

## `netlistgen.cpp` — `formNets()` (legacy path only)

The legacy weighted mix ends here. Terminals are bucketed into driver/sink
pools by IoType, with power/ground excluded by `dbSigType`. Each net gets one
driver plus `fanout` sinks, where `fanout` is the load count (driver
excluded). Every iterm is popped at most once, so the netlist is valid (each
pin on ≤ 1 net) — but the pairing is unconstrained, so **this path makes no
acyclicity guarantee** (it predates Stage D and supports multi-output
masters with no sequential/combinational classification).

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

## `netlistgen.cpp` — `formNetsAcyclic()` (Stage D, statistical path)

The statistical mix ends here instead. Instance creation order (`u0..u{n-1}`)
doubles as a topological order over the combinational cells; drivers are
processed in that same order and receiver *eligibility* is filtered while the
receiver *count* stays governed by `[min_fanout, max_fanout]`. Three receiver
pools carry the eligibility state:

- `seq_pool` — unused sequential-instance inputs (D/CK). Eligible for
  **every** driver: register inputs accept any source (feedback through a
  register is sequential, not combinational).
- `comb_active` — unused combinational inputs whose owner instance has **not
  been processed yet** (index > current `i`). Eligible for every driver.
- `comb_retired` — unused combinational inputs whose owner **has** been
  processed (index ≤ `i`). Eligible for **sequential (Q) drivers only**: a
  combinational driver may never feed an earlier-or-own instance.

At step `i` the instance's own unused inputs are retired *before* its net is
formed (so it can never drive itself), then each of its output pins draws
receivers uniformly from the union of its eligible pools (swap-remove
sampling; `active_pos` keeps `comb_active` O(1) under removal). Every
comb→comb edge therefore satisfies `driver index < receiver index` — a DAG by
construction. Thin-pool behavior is deterministic and documented in
README.md: a net whose eligible pool cannot cover `min_fanout` is loosened
(≥ 1 receiver, counted + debug-logged); a driver with zero eligible
receivers is skipped entirely in THIS pass (no net — never a sinkless net,
never a stall) — `has_connection[i]` records whether *any* output pin of
instance `i` got a net here, feeding the repair pass below.

If `spec.peak_avg_fanout` is set, `assignPeakClusters` runs once up front
(see its own diagram below) and the per-net logic gains one more branch —
covered separately below so this diagram stays the un-clustered baseline.

```mermaid
graph TD
  fill["for each inst i, for each iterm:<br/>skip POWER/GROUND + non-INPUT/INOUT<br/>seq inst -> seq_pool<br/>comb inst -> comb_active (+active_pos, +owner_index)"] --> loop{"i = 0..n-1, net cap not hit?"}
  loop -->|done| repairpass["no-dangling-instance repair pass<br/>(see below)"]
  loop --> retire["retire inst i's unused comb inputs:<br/>comb_active -> comb_retired"]
  retire --> outs["for each OUTPUT pin of inst i"]
  outs --> elig["eligible = seq_pool + comb_active<br/>+ comb_retired if seq driver"]
  elig --> zero{"eligible == 0?"}
  zero -->|yes| skipd["skip driver (no net); ++skipped"]
  zero -->|no| draw["want = pick_fanout(rng)<br/>k = min(want, eligible)"]
  draw --> mk["makeNet n{k}; connect driver<br/>has_connection[i] = true"]
  mk --> sample["k times: uniform index into pool union<br/>swap-remove; connect sink"]
  sample --> loose{"k < min_fanout?"}
  loose -->|yes| cnt["++loosened (trace-logged)"]
  loose -->|no| next["++nets_made"]
  cnt --> next
  skipd --> loop
  next --> loop
```

### No-dangling-instance repair pass

Runs once the loop above finishes, before `formNetsAcyclic` returns. See
README.md's "Guaranteed instance connectivity" section for the full
rationale (including a discarded earlier design — an up-front per-instance
reservation — and the measurement that ruled it out: it dragged a
peak-cluster design's achieved fanout down to roughly half its target by
permanently shrinking the general sampling pool for every instance, not
just the ones actually at risk).

Two data structures make the repair itself efficient at the instance counts
this engine targets (an O(n) scan per repaired instance, times up to n
repairs, would be O(n²)):

- `comb_by_owner` — every remaining `comb_active`/`comb_retired` iterm,
  keyed by owner instance index in a `std::set`. "Does a combinational input
  at index `> i` still exist" is then a single lookup at the set's maximum.
- `net_sink_count` + `comb_stealable`/`seq_stealable` — a one-time,
  O(total pins) scan builds a live per-net sink count and two candidate
  lists (owner-sorted comb / plain-list sequential) of every currently
  *connected* input pin, for the stealing fallback. Candidates are
  validated lazily at pop time against `net_sink_count` (which only ever
  decreases) — a stale candidate (its donor net already stolen down to one
  sink) is discarded permanently, never rescanned.

The outer scan runs `i = n-1` down to `0` — **reverse** creation order — so
the most constrained instance (the very last one, which has zero
higher-index candidates and so depends on `seq_pool` alone) gets first pick
of the shared, universally-usable `seq_pool` before earlier, less
constrained dangling drivers are even considered.

```mermaid
graph TD
  setup["build comb_by_owner (owner-sorted)<br/>build net_sink_count + comb_stealable/seq_stealable"] --> loop{"i = n-1 downto 0<br/>net cap not hit?"}
  loop -->|done| done2["debug: nets/loosened/skipped/repaired counts<br/>return nets_made"]
  loop --> hasconn{"has_connection[i]?"}
  hasconn -->|yes| loop
  hasconn -->|no| findout{"inst i has a<br/>signal OUTPUT pin?"}
  findout -->|no| loop
  findout -->|yes| tryowner["comb_by_owner: take max entry<br/>if seq driver OR owner > i"]
  tryowner --> gotowner{"found?"}
  gotowner -->|yes| mkrep["makeNet; connect driver + receiver<br/>has_connection[i] = true; ++repaired"]
  gotowner -->|no| tryseq["seq_pool: pop back<br/>(skip self-owned entry if i is sequential)"]
  tryseq --> gotseq{"found?"}
  gotseq -->|yes| mkrep
  gotseq -->|no| steal["steal fallback:<br/>comb_stealable then seq_stealable,<br/>lazily discarding stale entries,<br/>decrement net_sink_count on success"]
  steal --> gotsteal{"found?"}
  gotsteal -->|yes| mkrep
  gotsteal -->|no| fail["warn + return -1<br/>(should be unreachable)"]
  mkrep --> loop
```

## `netlistgen.cpp` — peak fanout sub-clusters (optional, layered on Stage D)

`assignPeakClusters` (exposed for testing) runs once, at the very top of
`formNetsAcyclic`, only if `spec.peak_avg_fanout` is set — it consumes the
shared `rng` exactly once (a single shuffle), so it does not perturb
generation determinism when the feature is unused. Its result is pure
bookkeeping (`cluster_id`, index-aligned with creation order): never
attached to the `dbBlock`/`Hypergraph`, only optionally copied out through
`generateSynthetic`'s `out_cluster_id` parameter for tests.

```mermaid
graph TD
  chk{"spec.peak_avg_fanout set?"} -->|no| skip["cluster_id stays empty;<br/>driver_cluster always -1 (no behavior change)"]
  chk -->|yes| shuf["order = shuffle([0, n), rng)<br/>(one rng consumption)"]
  shuf --> size["cluster_size = floor(peak_cluster_pct * n / num_peak_clusters)"]
  size --> slice["for k in 0..num_peak_clusters-1:<br/>cluster_id[order[k*cluster_size .. (k+1)*cluster_size-1]] = k<br/>rest = -1 (background)"]
  slice --> out["optional: copy into out_cluster_id (tests only)"]
```

When a driver's instance has `cluster_id[i] >= 0`, `formNetsAcyclic`'s per-net
logic (from the baseline diagram above) gains this extra branch in place of
the plain "want = pick_fanout(rng)" / "uniform index into pool union" steps:

```mermaid
graph TD
  netstart["driver_cluster = cluster_id[i]<br/>cluster_net = driver_cluster >= 0"] --> fanout{"cluster_net?"}
  fanout -->|no| bgfanout["want = pick_fanout(rng)<br/>(background range, unchanged)"]
  fanout -->|yes| pkfanout["want = pick_peak_fanout(rng)<br/>(same width, re-centred on peak_avg_fanout)"]
  bgfanout --> perslot
  pkfanout --> perslot["per sink slot s in 0..k-1"]
  perslot --> intra{"cluster_net AND<br/>unit(rng) < p_intra (0.8)?"}
  intra -->|no| plain["takeEligible(uniform index into<br/>seq_pool+comb_active(+comb_retired if seq driver))"]
  intra -->|yes| reject["up to kPeakClusterPickAttempts:<br/>peek a uniform eligible index r;<br/>clusterOf(peekEligible(r)) == driver_cluster?"]
  reject -->|hit| take["takeEligible(r)"]
  reject -->|cap exhausted, no hit| plain
  take --> connect["sink.connect(net); --eligible"]
  plain --> connect
```

Rejection sampling never mutates pool state on a miss (`peekEligible` reads
without removing), so a failed cluster-preferred attempt costs one extra
`rng` draw and nothing else; the eventual accepted pick (cluster-hit or
background fallback) is the only one that calls `takeEligible` and removes
from the pool. This is why cluster preference can never violate Stage D's
DAG invariant: it only ever chooses **among** the pools `formNetsAcyclic`
already computed as eligible for this specific driver — the eligibility
computation itself (`seq_pool` / `comb_active` / `comb_retired`,
`takeFromActive`, the retirement step) is completely unmodified by
clustering.

## `netlistgen.cpp` — `applyPrimaryIoStageE1()` (Stage E1, optional)

Runs once, called from `generateStatistical` right after `formNetsAcyclic`
returns — a separate pass over the already-formed `dbBlock`, not a change to
net formation. No-op (`RentStats{}`, `engaged = false`) unless
`spec.rent_k`/`spec.rent_p` are both set (`validateSpecConfig` already
enforced both-or-neither, `rent_k > 0`, `rent_p` in `(0, 1.2]` with the
`(1.0, 1.2]` warn-and-clamp case).

This is the SECOND revision of this function's Step 2/3 (see README.md's
"Two deliberate deviations" for the full story): the first revision
disconnected an existing net's driver for PI, which was found to leave some
instances fully dangling (zero connections at all) — a strict, non-negotiable
correctness bar this repo holds. Neither PI nor PO ever touches a live
driver in the shipped design.

```mermaid
graph TD
  chk{"rent_k and rent_p both set?"} -->|no| noop["return RentStats{} (engaged=false)"]
  chk -->|yes| t1["Step 1: G=spec.num_insts<br/>p_eff=min(rent_p,1.0)<br/>T=round(rent_k * G^p_eff)"]
  t1 --> t2["Step 2: build pi_pool = leftover unconnected<br/>INPUT/INOUT iterms + stealable (all-but-first)<br/>sinks of fanout>=2 nets; shuffle(rng)"]
  t2 --> t2b["build po_pool = leftover unconnected<br/>OUTPUT iterms; shuffle(rng)<br/>snapshot all_nets (PO fallback source)"]
  t2b --> cap{"T_in > pi_pool.size()?"}
  cap -->|yes| warn["warn 'E1: T_in (..) exceeds available<br/>PI target pins (..); capping'<br/>T_in = pi_pool.size()"]
  cap -->|no| pi
  warn --> pi["Step 3 (PI): for each of T_in ports,<br/>draw want=pick_fanout(rng), pop up to want<br/>pins from pi_pool (stolen ones disconnected<br/>from their current net first)"]
  pi --> piskip{"0 pins popped?"}
  piskip -->|yes, capped at kTraceCap| skippi["warn + skip port<br/>(not a failure — Stage D's own<br/>thin-tail-skip philosophy)"]
  piskip -->|no| pinet["build ONE fresh net: driver =<br/>PI bTerm (combinational) or reused<br/>buf/ff output (buffered/registered);<br/>sinks = popped pins"]
  skippi --> po
  pinet --> po["Step 3 (PO): for each of T_out ports,<br/>pop from po_pool if non-empty (fresh net,<br/>leftover pin as driver) else pick any<br/>existing net (fallback: add as extra sink)"]
  po --> s6["Step 6: p_actual=log(T_actual)/log(G)<br/>k_actual=T_actual/G^p_actual (always 1.0 — see README)"]
  s6 --> s5{"has_any_cluster(cluster_id)?"}
  s5 -->|no| ret["return stats"]
  s5 -->|yes| clu["Step 5: per-cluster G_c/T_c (skip if degenerate)<br/>+ background G_bg/T_bg (skip if degenerate)"]
  clu --> ret
```

**Step 3 pin-type dispatch.** PI always builds a fresh net from pool
material; PO either builds a fresh net (leftover pin) or augments an
existing one (fallback) — either way, an existing net's *driver* is never
touched by either path:

```mermaid
graph TD
  subgraph PI["PI (targets popped from pi_pool)"]
    pikind["kind = pick_pin_type(rng)"] --> pic{"kind"}
    pic -->|combinational| pc1["net = makeNet(); targets.connect(net);<br/>bTerm(net, INPUT) — net's sole driver"]
    pic -->|buffered| pc2["reuse first non-empty plan.comb[] master<br/>target_net: buf.out drives it, targets are its sinks<br/>feed-net: bTerm(IN) -> buf.in"]
    pic -->|registered| pc3["reuse plan.seq[0] master (guaranteed non-empty)<br/>target_net: ff.Q drives it, targets are its sinks<br/>feed-net: bTerm(IN) -> ff.D"]
  end
  subgraph PO["PO (target_net from po_pool or fallback)"]
    pokind["kind = pick_pin_type(rng)"] --> poc{"kind"}
    poc -->|combinational| oc1["bTerm(target_net, OUTPUT)<br/>— fresh net's sole sink, or one more<br/>sink on an existing net"]
    poc -->|buffered| oc2["buf.in connects to target_net (added sink);<br/>buf.out drives a NEW feed-net with bTerm(OUT)"]
    poc -->|registered| oc3["ff.D connects to target_net (added sink);<br/>ff.Q drives a NEW feed-net with bTerm(OUT)"]
  end
```

`firstOutputIterm`/`firstDataInputIterm` locate a reused master's driver pin
and its first non-clock data input by IoType/SigType — never by pin name —
matching the rest of this file's low-level `dbITerm` manipulation style
(`isClockPinName` catches libraries like Nangate45 that tag `CK` `USE
SIGNAL`, same fallback `isSequentialMaster` already relies on). New
instances/nets continue the existing `u<i>`/`n<i>` naming sequences from the
internal counts — planes (via `RentStats`, not names) are what distinguish a
boundary cell from an internal one.

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
  loop drivers in creation order (statistical: formNetsAcyclic) / until pools drain (legacy: formNets)
    GS->>NB: makeNet + connect driver + eligible sinks
    NB->>ODB: dbNet + iterm->connect
  end
  GS->>GS: empirical tolerance check (warn only)
  GS-->>Caller: nets_made (or -1)
  Caller->>HG: buildFromBlock(nb.block())
  HG-->>Caller: hypergraph view
```

## `netlist_validation.cpp` — well-formedness check (Stage C; bTerm-aware since Stage E1; instance check added alongside Stage D's repair pass)

`validateNetlist(block)` walks every `dbNet` and tallies its connected
terminals by IoType (power/ground skipped by `dbSigType`) — `dbITerm`s via
`tallyITerms`, and, since Stage E1, `dbBTerm`s via `tallyBTerms` (a `dbBTerm`
with IoType `INPUT` counts as a driver — a primary input supplies the net
from outside; `OUTPUT`/`INOUT` counts as a sink) — both feeding one shared
`NetTally` before the verdict. This is a distinct guarantee from Stage D's
loop-freedom: a net can be perfectly well-formed and still sit on a
combinational cycle. Folding bTerms in here (rather than a parallel rule) is
exactly why Stage E1's primary-input realization *replaces* a selected net's
existing driver instead of adding the PI bTerm alongside it — see
`netlist_validation.h` and the netlistgen README's "Primary I/O generation
(Stage E1)" section.

Once every net passes, a second loop (`instanceHasConnectedOutput`) walks
every `dbInst` and confirms at least one of its signal `OUTPUT` iterms has a
non-null net — a check the net-centric tallies above cannot express, since
they never see a driver pin that was never connected to any net at all.
This is the hard gate `formNetsAcyclic`'s no-dangling-instance repair pass
(see its own section above) exists to satisfy.

```mermaid
graph TD
  vn["validateNetlist(block)"] --> nul{"block null?"}
  nul -->|yes| okv["ok (nothing to check)"]
  nul -->|no| loop["for each dbNet"]
  loop --> tally["tallyITerms: for each iterm<br/>skip POWER/GROUND<br/>OUTPUT -> drivers<br/>INPUT/INOUT -> sinks<br/>++connected"]
  tally --> tallyb["tallyBTerms: for each bterm<br/>skip POWER/GROUND<br/>INPUT -> drivers<br/>OUTPUT/INOUT -> sinks<br/>++connected"]
  tallyb --> c0{"connected == 0?"}
  c0 -->|yes| fdangle["fail: 'net dangling'"]
  c0 -->|no| d1{"drivers != 1?"}
  d1 -->|yes| fdrv["fail: 'N drivers (expected 1)'"]
  d1 -->|no| s1{"sinks < 1?"}
  s1 -->|yes| fsnk["fail: 'no sinks'"]
  s1 -->|no| loop
  loop --> instloop["for each dbInst"]
  instloop --> hasout{"has a signal<br/>OUTPUT iterm at all?"}
  hasout -->|no| instloop
  hasout -->|yes| anyconn{"any OUTPUT iterm<br/>has getNet() != null?"}
  anyconn -->|no| fdanglinst["fail: 'instance dangling'"]
  anyconn -->|yes| instloop
  instloop --> okv
```

## `netlist_writers.cpp` — DEF / `.odb` output (Stage C)

Two thin wrappers, callable independently of the CLI. `writeDef` drives
`odb::DefOut` at version 5.8 — `netlistgen` never touches this file for
Stage E1's `dbBTerm`s at all: `DefOut::writeBlock` is a generic ODB
serializer that already emits a correct `PINS` section for whatever
`dbBTerm`s exist on the block (verified manually against Stage E1 output; no
pin placement/geometry, since these bTerms carry none); it supplies a local
`utl::Logger` when the caller passes none. `writeOdb` wraps
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
  pok -->|yes| gen["info: Generating...<br/>generateSynthetic(builder(&logger), spec,<br/>nullptr, &rent_stats)"]
  gen --> gok{"nets >= 0?"}
  gok -->|no| e1
  gok -->|yes| die["info: Generation complete (counts)<br/>estimateDieArea(num_insts)"]
  die --> vaw["info: Running validation<br/>validateAndWrite(builder, config, err)"]
  vaw --> valid{"validateNetlist ok?<br/>(bTerm-aware since Stage E1)"}
  valid -->|no| e1b["err 'validation failed'<br/>write nothing; return 1"]
  valid -->|yes| odir{"ensureOutputDir:<br/>create missing output dirs"}
  odir -->|create failed| e1c["err 'cannot create output directory'<br/>write nothing; return 1"]
  odir -->|ok| wdef["if output_def_path: writeDef (info: Wrote DEF)"]
  wdef --> wodb["if output_odb_path: writeOdb (info: Wrote .odb)"]
  wodb --> summ["reportDesignSummary (report):<br/>cells comb/seq · comb pin-count hist<br/>net count · avg fanout/net (driver excl)<br/>fanout hist (loads; 10-50 / &gt;50 bucketed)"]
  summ --> summ2["reportPrimaryIoSummary (report):<br/>no-op unless rent_stats.engaged —<br/>target/actual Rent, pin-type counts,<br/>boundary FF count; + per-cluster/<br/>background Rent if clusters engaged"]
  summ2 --> counts["info: Done."]
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
    j7["peak_avg_fanout / peak_cluster_pct<br/>num_peak_clusters"]
    j8["rent_k / rent_p / io_input_ratio<br/>io_pin_type_distribution<br/>{combinational,buffered,registered}"]
    j6["output_def_path / output_odb_path<br/>(CLI-only, >=1 required)"]
  end
  subgraph CliConfig
    s1["spec.num_insts"]
    s2["spec.seed / spec.num_nets"]
    s3["spec.min_fanout / max_fanout"]
    s4["spec.tech_lef_path / cell_lef_paths"]
    s5["spec.sequential_ratio / ...dist / target / tol"]
    s7["spec.peak_avg_fanout / peak_cluster_pct<br/>num_peak_clusters"]
    s8["spec.rent_k / rent_p / io_input_ratio<br/>io_pin_type_distribution"]
    s6["config.output_def_path / output_odb_path"]
  end
  j1 --> s1
  j2 --> s2
  j3 --> s3
  j4 --> s4
  j5 --> s5
  j7 --> s7
  j8 --> s8
  j6 --> s6
```
