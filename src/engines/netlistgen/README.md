# src/engines/netlistgen/

Synthetic netlist generation engine. Builds `dbBlock`s through OpenDB API
calls — optionally backed by real LEF cells — so tests and benchmarks can
create netlists of any size with exactly known or statistically controlled
topology, then feed them to `Hypergraph::buildFromBlock()`.

> **Migration status (Stage E1 of 5+ — "Phase 1" complete since Stage D).**
> Stage A promoted `netlistgen` from a Stage 1/2 test utility into an engine
> and made pin access IoType-based. Stage B added LEF-backed masters (real
> cells) and a statistical cell mix (forward + max-entropy modes). Stage C
> added DEF / `.odb` writers, a net well-formedness validation pass, and a
> standalone JSON-driven CLI executable (`netlistgen_cli`). Stage D made
> statistical-mix net formation **combinational-loop-free by construction**
> (see "Combinational-loop avoidance" below), completing Phase 1: generated
> DEF / `.odb` output is genuinely valid — well-formed *and* acyclic. **Stage
> E1 (this stage, `docs/briefs/spike-netlistgen-E1-io-rent.md`)** adds
> primary input/output port generation governed by **Rent's rule**
> (see "Primary I/O generation (Stage E1)" below). The **well-formedness
> audit** (`docs/briefs/spike-netlistgen-wellformed-audit.md`) then added
> the D/Q-only sequential pin constraint (see "The D/Q-only sequential pin
> convention" below), hardened `validateNetlist` with a control-pin check,
> and made a too-low `num_nets` cap a hard error. Still landing later:
> - **Stage E2** — structural Verilog output (a separate brief).
>
> Note: Stage E1 does **not** relax the `sequential_ratio > 0` bootstrap
> requirement noted in earlier revisions of this doc — PI/PO ports are added
> in a separate pass *after* Stage D's net formation completes and never
> participate in its DAG bootstrap, so that requirement is unchanged.
>
> **Peak fanout sub-clusters** (`docs/briefs/
> spike-netlistgen-peak-fanout-clusters.md`) landed on top of Stage D:
> optional, additive-only congestion-hot-spot generation for validating
> downstream metrics tooling (`hg_metrics`, not yet implemented in this
> repo) — see "Peak fanout sub-clusters" below. Stage E1's sub-cluster Rent
> statistics (Step 5) build directly on it.
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
    list. Deterministic for a given `(spec, seed)`. (Net shapes shifted by
    one sink when `fanout` was redefined from pins-per-net to loads, so
    output is no longer identical to the original Stage A.)
  - **Statistical mix (Stage B).** Engaged when any statistical field (or a
    LEF path) is set. Each instance is first rolled sequential vs
    combinational by `sequential_ratio`; a combinational instance then rolls
    a pin-count bucket from the effective distribution and a master uniformly
    among that bucket's cells.

  Net formation classifies terminals **by IoType, with power/ground pins
  excluded by `dbSigType`** (drivers = `OUTPUT`, sinks = `INPUT`/`INOUT`),
  and every iterm lands on at most one net. On the statistical path, pin
  eligibility is further restricted by the **D/Q-only sequential pin
  convention** (see the section below): only `isDataPin` pins ever join a
  net. The two regimes then differ:
  - **Statistical mix** uses the Stage D **ordered, acyclic-by-construction**
    formation (`formNetsAcyclic` — see "Combinational-loop avoidance" below).
  - **Legacy weighted mix** keeps the original shuffled-pool pairing
    (`formNets`): one driver plus `fanout` sinks popped from shuffled pools.
    **No acyclicity guarantee on this path** — it exists for Stage A
    compatibility, supports multi-output masters, and has no
    sequential/combinational classification to build a DAG order from.

  Returns the net count, or **`-1`** if the spec fails validation.

## The D/Q-only sequential pin convention

**Only the data pins of a sequential (flip-flop) cell participate in
data-net connectivity: its D input(s) as sinks, its Q output as driver.
Every other pin — clock, async set/reset, scan-enable/test control,
scan-out, the inverted output QN — is left permanently unconnected in the
generated netlist.** Clock/reset/scan are global signals in a real flow;
they must never appear as signal-net endpoints in a synthetic netlist.
Enforced by construction everywhere pins are selected, and as a hard
`validateNetlist` gate (see "Net well-formedness validation" below).

The single source of truth is **`isDataPin(dbMTerm*)`** (`netlistgen.h`),
shared by the Stage B/D receiver pools and driver picks, the Stage D
dangling-instance repair pass, Stage E1's PI/PO target-pin pools, and
`validateNetlist`'s control-pin check. Pools are filtered through it
**before any random sampling begins** — an ineligible pin can never be
drawn, stolen, or repaired onto a net. A pin is a data pin iff:

1. **Sig-type gate:** its `dbSigType` is `SIGNAL`. `CLOCK`/`RESET`/`SCAN`/
   `ANALOG`/`POWER`/`GROUND`/`TIEOFF` pins are never connected, regardless
   of direction.
2. **Name rule, sequential masters only:** if its master is sequential
   (`isSequentialMaster`), its name must not be a conventional control-pin
   name. Needed because libraries like Nangate45 tag *every* pin
   `USE SIGNAL`, so a flip-flop's CK/RN/SN/SE/QN are indistinguishable from
   data by sig type alone. Excluded (case-insensitive): clock
   (`CK`/`CLK`/`CLOCK`/`CP`), async set/reset (`RN`/`SN`/`R`/`S`/`RESET`/
   `SET`/`CLR`/`CLEAR`/`PRE`/`PRESET`), scan-enable/test (`SE`/`TE`/`TM`),
   scan-out (`SO`), and the inverted output (`QN`). **Scan-in `SI` is NOT
   excluded** — it is a data path (muxed behind scan-enable) and is treated
   the same as D. Because the name rule applies to sequential masters only,
   combinational pins that reuse these letters (e.g. `FA_X1`'s sum output
   `S`) are unaffected.

Per mode:

- **LEF-backed mode (Nangate45):** a DFF's data endpoints are exactly `D`,
  `SI` (scan variants), and `Q`; `CK`, `RN`, `SN`, `SE`, and `QN` stay
  unconnected.
- **Synthetic mode:** the sequential representative (`makeMaster` with
  `clocked = true`) has pins `i0` (its clock, `dbSigType::CLOCK` — caught by
  the sig-type gate alone), `i1` (its D pin, the only data sink) and `o0`
  (its Q pin, the only data driver). Synthetic FFs model no other pins —
  they are structural skeletons, not real cells.

## Combinational-loop avoidance (Stage D)

Statistical-mix netlists are **guaranteed free of combinational loops by
construction** — no post-hoc cycle detection, no repair pass. The design
reuses the instance creation index (`u0..u{n-1}`) directly as a topological
(DAG) order; `formNetsAcyclic` processes drivers in that same order and
filters receiver *eligibility* (the receiver *count* per net is still drawn
uniformly from `[min_fanout, max_fanout]` — the Stage B statistical
machinery is untouched, so Stage E can add primary ports as one more
driver/receiver class without a rewrite):

- **Sequential (Q) outputs are always-valid drivers**, regardless of order —
  a register output is the loop-breaker.
- **Sequential DATA inputs (D, and SI where a library has one) are
  always-valid sinks** — a D pin may depend on nets that transitively depend
  on its own Q; that is a legitimate sequential feedback loop, not a
  combinational one. (Clock and other control pins are never sinks at all —
  see "The D/Q-only sequential pin convention" above.)
- **A combinational output at index `i` may only drive** sequential-instance
  inputs (any index) or combinational-instance inputs at indices **`> i`**
  (instances not yet processed). Never an earlier combinational instance,
  never itself. Every comb→comb edge therefore goes strictly forward in
  index order, so no combinational cycle can exist. (Stage B's
  multi-output-exclusion work makes "the single combinational output"
  assumption safe.)

**Bootstrap-source requirement: `sequential_ratio > 0` is mandatory** in
statistical mode (an unset ratio counts as 0) and fails fast at spec-build
time in `validateSpecConfig`: sequential Q outputs are the only valid signal
source to start the combinational DAG from — no primary-input ports exist
until Stage E, which relaxes this back to "at least one of
`sequential_ratio > 0` or `primary_input_count > 0`".

**Thin-pool behavior (deterministic, chosen over fail-fast):** where the
eligible pool runs thin — typically the *tail* of the creation order, where
a combinational driver's only remaining candidates are unused sequential
inputs — the net is **loosened**: it is formed with fewer receivers than
`min_fanout` (always ≥ 1), counted and debug-logged. A driver with **zero**
eligible receivers forms **no net at all** in this pass (skipped, counted
and debug-logged) — never a sinkless net, never a stall or retry loop. That
skip is not the end of the story for that driver, though — see "Guaranteed
instance connectivity" below, which repairs every such driver afterward so
it never actually ends up dangling. Loosening was chosen over fail-fast
because a thin tail is a structural certainty of the ordered filter (the
last combinational instance in order *never* has later combinational
candidates), so failing would make many perfectly reasonable configs
unusable; a shorter tail net keeps the run deterministic, well-formed, and
honest about what was feasible.

**Expected distribution shift vs the Stage B baseline** (known structural
effect, not a regression): the *cell mix* is untouched (instances are rolled
before net formation — Stage B's statistical-validation test still passes
within `distribution_tolerance_pct` unchanged), and per-net fanout is still
uniform over `[min_fanout, max_fanout]` for the body of the run. What
shifts: **fewer nets for the same config** (tail drivers get skipped once
only-sequential candidates remain and are consumed — e.g. the 1500-inst
LEF-backed manual config produced 1007 nets with every fanout in [2, 5] and
mean 3.55, where unconstrained pairing had run the sink pool to exhaustion),
a possible handful of **below-`min_fanout` tail nets** at small scale, and
correspondingly **more input pins left unconnected** (until the repair pass
below claims some of them).

### Guaranteed instance connectivity

**Every instance ends up with at least one connected DATA output — a hard,
universal invariant, exactly as strict as "no multiply-driven nets" or "no
sinkless nets."** ("Data output" per the D/Q-only convention above: a
sequential instance is alive only through its Q — a connected QN would not
count, and could not occur anyway since generation never connects one.)
`validateNetlist` (see "Net well-formedness validation"
below) enforces this as a gate independent of net-formation itself: a
driver whose data output(s) are all unconnected is dead logic (in a real
flow it would be deleted, cascading backward through anything that fed only
it, silently shrinking the design away from whatever Rent's-rule/instance-
count targets the run asked for) — a stricter requirement than mere
combinational-loop-freedom, since the "thin-pool" skip above, left alone,
can and does leave a driver with no net at all.

The mechanism is a **second pass, not a change to the statistical draw
above.** `formNetsAcyclic` runs the ordinary, unrestricted forward pass
exactly as described above (identical pool mechanics, identical skip
behavior for a driver with zero eligible receivers), tracking only one extra
bit per instance — did *any* of its output pins get connected. Once that
pass completes, a **repair pass** walks every instance that still has none
and gives it exactly one receiver, respecting the same DAG rule the forward
pass enforces (a combinational driver may only feed a combinational input
at index `> i`, or any sequential input; a sequential driver may feed
anything but itself) and never touching a live driver:

1. Prefer still-unconnected material — combinational inputs at index `> i`
   (whatever the forward pass left in `comb_active`/`comb_retired`), then
   any unconnected sequential input (`seq_pool`).
2. Falling back to **stealing** a non-last sink of an already multi-sink net
   (the donor net always keeps ≥ 1 remaining sink) only once no leftover
   material exists at all — the same "add or reuse, never delete-and-
   cascade" philosophy Stage E1's PI/PO wiring uses (see below).

**The invariant is unconditional — a `num_nets` cap cannot buy it off.** If
`spec.num_nets` is set so low that a dangling instance cannot be repaired
without exceeding the cap, generation **fails fast** (`generateSynthetic`
returns `-1`, with a warning naming the first unrepairable instance and the
cap), rather than silently leaving the instance dangling as "deliberate
truncation": a config that cannot produce a well-formed netlist is an
invalid config. The CLI then exits nonzero, writes no output files, and
prints no statistics. (The legacy weighted-mix path keeps its original
truncating cap — it makes no connectivity guarantee of any kind.)

**Two ordering rules make this provably safe rather than a race between
dangling drivers competing for the same scarce material:**
- Within a single repair, order-scarce candidates (combinational inputs at
  index `> i` — useful only to this driver and later ones) are always tried
  **before** `seq_pool` (universally usable by *any* dangling driver,
  combinational or sequential, any index) — so a driver with its own option
  never needlessly spends the one resource a more-constrained driver may
  depend on entirely.
- The repair pass itself walks instances in **reverse** creation order
  (`n-1` down to `0`). The last instance in the design is the most
  constrained one there is (zero later-index candidates, so `seq_pool` is
  its *only* option) — it must get first pick of `seq_pool` while it is
  fullest, before earlier — and therefore less constrained — dangling
  drivers are even considered.

**Performance:** this repair must scale to the same hundreds-of-thousands-
of-instances designs the rest of this engine does, so it is built on O(log
n) / amortized-O(1) structures rather than a linear scan per repaired
instance: combinational candidates are held in one `std::set` keyed by
owner index (so "does anything with owner `> i` still exist" is a single
lookup at the tree's max), and the stealing fallback is backed by a
one-time-built, live per-net sink-count map plus two candidate pools (again
comb-owned/sequential-owned) that are validated **lazily** at pop time —
a candidate whose donor net has already been stolen down to a single sink
is discarded permanently (net sink counts only ever fall), never rescanned.

**Why not reserve a receiver for every instance up front instead** (the
first design tried here, discarded): a deterministic reverse scan can
provably guarantee a valid, order-respecting receiver for every
combinational instance *before* generation even starts, using nothing more
than the pre-existing `sequential_ratio > 0` bootstrap rule — no stricter
ratio floor is mathematically required, contrary to an earlier, buggy
implementation of that scheme that concluded one was. But reserving one
input pin **per instance** removes that much capacity from the general
statistical draw for the *entire* run, regardless of whether that specific
instance would ever actually have gone dangling — and most would not have.
Measured impact: on a 2000-instance peak-fanout-cluster design (see "Peak
fanout sub-clusters" above) targeting `peak_avg_fanout = 12`, the
reservation approach dragged the achieved cluster average down to ~6.3 —
roughly half the target, well outside even a generous tolerance — because
it permanently removed close to a third of the design's total input-pin
supply before a single statistical draw ran. The repair-pass design above
leaves the statistical draw completely untouched and only pays the
repair cost for instances that actually need it.

## Peak fanout sub-clusters

Optional congestion-hot-spot generation, layered on top of the statistical
mix and Stage D's acyclic net formation (`docs/briefs/
spike-netlistgen-peak-fanout-clusters.md`): a subset of instances is grouped
into `num_peak_clusters` clusters whose intra-cluster nets are driven at a
higher fanout than the background, giving localized dense subgraphs with
known structural properties as ground truth for downstream congestion/timing
detectors (`hg_metrics`, not yet implemented in this repo).

**JSON / spec fields** (all optional; absent entirely ⇒ generation identical
to before this feature):

| Field | Maps to | Rule |
|-------|---------|------|
| `peak_avg_fanout` | `spec.peak_avg_fanout` | Engages the feature. Must be `>` the background average fanout `(min_fanout + max_fanout) / 2` — this codebase has no literal "average fanout" scalar (the background range is `[min_fanout, max_fanout]`), so the range midpoint is the closest analog and what a "hot spot" must exceed. |
| `peak_cluster_pct` | `spec.peak_cluster_pct` | Fraction of instances assigned to peak clusters, split evenly. Must be in `(0, 1)` if set; defaults to `0.10` if unset. **Ignored** (no validation, no effect) if `peak_avg_fanout` is absent. |
| `num_peak_clusters` | `spec.num_peak_clusters` | Number of clusters. Must be `>= 1` if set; defaults to `1`. Ignored if `peak_avg_fanout` is absent. |

**Requires the statistical mix.** `peak_avg_fanout` set while the spec is
otherwise legacy weighted-mix is a **validation error**, not a silent no-op:
the legacy path has no per-instance sequential/combinational classification
to build cluster-safe eligibility from (see below).

**Cluster assignment** (`assignPeakClusters`, exposed for testing): runs once
per generation, consuming the shared seeded RNG exactly once (a single
`std::shuffle` over `[0, num_insts)`), so it stays reproducible for a given
`(spec, seed)` alongside the rest of generation. `cluster_size =
floor(peak_cluster_pct * num_insts / num_peak_clusters)`; the first
`cluster_size` shuffled indices become cluster 0, the next `cluster_size`
become cluster 1, and so on; everything else is background (`-1`).
**Cluster membership is a local bookkeeping vector, never attached to the
dbBlock/Hypergraph model** — `generateSynthetic` exposes it only through an
optional trailing `out_cluster_id` out-parameter (`nullptr` by default, so
every existing call site is unaffected) purely so tests can observe it.

**Cluster-aware net formation**, inside `formNetsAcyclic`: for a net driven
from an instance in cluster `c`,
- the receiver **count** is drawn from a distribution centred on
  `peak_avg_fanout` instead of the background range — same uniform *shape*
  and *width* as `[min_fanout, max_fanout]`, just re-centred (this codebase's
  fanout sampling is a uniform range rather than a Poisson/log-normal
  family, so "same distribution shape, new centre" means re-centring that
  same range);
- each receiver slot is filled from cluster `c` with probability `p_intra =
  0.8` (an internal constant, not exposed in the config) via **rejection
  sampling over the pools Stage D already treats as eligible** — draw a
  uniformly random eligible receiver, accept it if it belongs to cluster
  `c`, otherwise redraw (capped at a fixed attempt count) — else (or on the
  remaining `1 - p_intra` of slots) an ordinary uniform draw over the full
  eligible pool, exactly as before clustering existed.

**This is the one deliberate deviation from a literal reading of the spike
brief**, and it is load-bearing: the brief's own pseudocode picks "a random
cell from cluster `c`" with no eligibility filter, and separately assumes
Stage D is a post-hoc "DAG topological sort + cycle breaking" pass that
would clean up any cycles clustering introduces. Neither matches this
engine — Stage D **prevents** cycles by restricting which receivers are
eligible **during** formation (`formNetsAcyclic`'s `seq_pool` /
`comb_active` / `comb_retired`), there is no separate repair pass to fall
back on, and cluster membership (built from an arbitrary shuffle) is
uncorrelated with creation order, so drawing from a cluster **without**
respecting eligibility would reintroduce exactly the combinational cycles
Stage D exists to rule out. Restricting cluster preference to already-
eligible receivers keeps the brief's intent (a tunable, measurable
fanout-favoring bias toward same-cluster cells) while leaving Stage D
completely untouched, as the brief separately requires. One consequence:
for a combinational driver late in the creation order — where Stage D's
eligible pool is already thin (see "Combinational-loop avoidance" above) —
the effective intra-cluster hit rate can fall below the nominal 80% (the
rejection-sampling cap degrades gracefully to a background pick, per the
brief's own documented edge case, generalized from "cluster has fewer
cells" to "cluster has fewer **eligible** cells").

**Edge case (brief's own wording, satisfied by construction):** if cluster
`c` has fewer eligible cells than the requested fanout, the fallback to a
background pick is automatic and never produces a duplicate sink or a
stalled draw — the same swap-remove pool mechanics Stage D already uses.

## Primary I/O generation (Stage E1)

Optional primary-input (PI) / primary-output (PO) port generation, sized by
**Rent's rule** (`T = k · Gᵖ`, `docs/briefs/spike-netlistgen-E1-io-rent.md`):
runs as a separate pass, directly on the already-formed `dbBlock`, once
`formNetsAcyclic` (Stage D) completes — **not** a change to net formation
itself, and it requires the statistical mix (boundary buffer/FF cells reuse
its already-resolved representative masters, same requirement as peak
fanout sub-clusters). Engaged only when **both** `rent_k` and `rent_p` are
set (exactly one alone is a validation error).

- **Step 1 — target terminal count.** `G` = `spec.num_insts` (internal
  instances only, boundary cells not yet created). `T = round(k · Gᵖ)`;
  `T_in = round(T · io_input_ratio)` PIs, `T_out = T − T_in` POs.
- **Step 2 — safe target-pin pools** (revised from the brief's literal
  "select existing nets" model — see "Two deliberate deviations" below for
  why). `pi_pool` = every currently-unconnected `INPUT`/`INOUT` iterm
  (Stage D's own "thin tail" leftovers) plus, as a fallback, all-but-one
  sink of every net with fanout ≥ 2 (stealable without ever leaving that
  net's driver sinkless). `po_pool` = every currently-unconnected `OUTPUT`
  iterm (Stage D's own "skipped driver" leftovers). Both shuffled once with
  the shared seeded RNG. `T_in` is capped down to `pi_pool.size()` up front
  if it exceeds it (warned: `E1: T_in (...) exceeds available PI target
  pins (...); capping`); `T_out` has no equivalent hard cap (its fallback —
  see Step 3 — is always available as long as at least one net exists).
- **Step 3 — pin type + boundary cell insertion**, sampled independently per
  port from `io_pin_type_distribution` (combinational / buffered /
  registered). **PI**: draws a fanout-shaped count of target pins from
  `pi_pool` (same `[min_fanout, max_fanout]` draw Stage D itself uses) and
  builds one fresh net per port — driver = the PI `dbBTerm` (combinational)
  or a reused boundary buffer/FF's output (buffered/registered), sinks = the
  drawn pool pins; a port that draws zero pins (pool exhausted mid-run) is
  **skipped**, not a failure (logged, capped at `kTraceCap`, mirroring
  Stage D's own thin-tail-skip philosophy exactly). **PO**: prefers popping
  one pin from `po_pool` onto a brand-new dedicated net (repairing a
  pre-existing dead-output instance); once that pool is empty, falls back
  to picking any currently-formed net and simply adding one more sink onto
  it (`bTerm` directly, or a buffer/FF's input) — always safe, since a
  net's existing driver is never touched either way. Buffer/FF cells reuse
  the statistical mix's first non-empty combinational bucket and its
  sequential class (the latter guaranteed non-empty by Stage D's
  `sequential_ratio > 0` bootstrap rule) — works identically in synthetic
  and LEF mode, no extra master-sourcing logic. Naming continues the
  existing `u<i>`/`n<i>` sequences; **planes, not names**, distinguish a
  boundary cell from an internal one (same philosophy as
  `hgm.is_boundary_reg` vs. internal registers). `RentStats.T_in`/`T_out`/
  `T_actual` reflect what was actually achieved (may be less than Rent's
  rule requested if a port had to be skipped).
- **Step 5 — sub-cluster (+ background) Rent stats**, when the peak
  fanout sub-cluster feature is also engaged: per cluster, `G_c` = original
  internal instances in the cluster (boundary buf/FF cells excluded, same
  exclusion `G`/`G_actual` already apply), `T_c` = its cut nets (an
  instance-owned endpoint's cluster is looked up positionally in
  `cluster_id`; a connected `dbBTerm`, or a boundary buf/FF instance created
  by this same pass, always counts as "outside every cluster" — both are,
  definitionally, boundary/external). `G_c < 2` or `T_c < 1` is degenerate
  and skipped (logged). Background mirrors this with `cluster_id == -1`,
  plus nets entirely inside the background that reach a PI/PO bTerm.
- **Step 6 — actual Rent for the full design**: `p_actual = log(T_actual) /
  log(G)`, `k_actual = T_actual / G^p_actual`. Note this formula (the
  brief's own, and reused identically for `p_c`/`k_c` and `p_bg`/`k_bg`) is
  **tautological in `k`**: because `p` is defined as exactly the exponent
  that makes `Gᵖ = T`, `k = T / Gᵖ` is **always 1.0** for any `(T, G)`. This
  isn't a bug — the *actual p* is the informative number here; the reported
  `k_actual`/`k_c`/`k_bg` are a faithful implementation of the brief's
  literal formula, not evidence something is wrong.

### Two deliberate deviations from a literal reading of the brief

**1. PI/PO never touch a live driver at all — this is the SECOND revision
of this design, not the first.** The brief's own pseudocode says a PI
becomes "an additional driver" of a randomly-selected *existing* net — but
every net Stage D forms already has exactly one committed internal driver,
and this repo treats "exactly one driver per net" as a hard, enforced
invariant (`validateNetlist`). Two drivers on one net is also not how real
designs work: a net is driven either by a primary input or by an internal
instance's output, never both (confirmed as the intended semantics before
implementing).

The **first** revision fixed that by having a PI-selected net's existing
driver `dbITerm` **disconnect** (freed, "harmlessly" left unused) and the
PI's `dbBTerm(INPUT)` become the net's sole driver. That turned out to be
wrong too: **an instance whose output drives nothing is dead logic — not a
harmless side effect — and this repo treats "no dangling instances" as an
equally hard, strict invariant** (confirmed explicitly, after empirically
measuring the first revision's effect: on a 2000-instance run it left 23
instances with *zero* connections at all, on top of ~120 more with a merely
unused output — the former is a correctness bug, not a rounding error, and
"delete the dead instance" is not a safe fix either, since deletion can
cascade backward through anything that fed *only* that instance, shrinking
`G` unpredictably away from the very count Rent's rule is supposed to be
anchored to).

The design actually shipped (see Steps 2–3 above) instead **never
disconnects a live driver, for either PI or PO**:
- **PI** targets Stage D's own leftover, never-connected internal input
  pins first (abundant in practice — e.g. ~1600 of them on a 2000-inst run
  in one measurement, versus a typical `T_in` in the low hundreds), falling
  back to *stealing* a non-last sink of an already-multi-sink net (which by
  construction never leaves that net's driver sinkless) only once that pool
  runs dry.
- **PO** prefers claiming a leftover, never-connected internal output pin —
  which *repairs* what would otherwise be a dead-output instance by giving
  it a real destination — falling back to simply adding one more sink onto
  any existing, already-driven net (always safe; nets already support
  arbitrary fanout) once that pool is exhausted.

Neither path ever needs to disconnect, delete, or "salvage" anything, so
there is no cascading-deletion risk and `G` never drifts from
`spec.num_insts`. `test/netlistgen_rent_test.cpp`'s
`NoDanglingInstancesAfterE1` is the direct correctness test: across several
instance counts and seeds, it confirms zero fully-isolated instances both
before and after Stage E1 runs, and that the pre-existing
dead-output-instance count (Stage D's own, unrelated to this feature) never
*increases* — Stage E1 may only ever repair some of it.

**This is still why `netlist_validation.cpp` folds `dbBTerm`s into its
driver/sink tally** (an `INPUT` bTerm counts as a driver, `OUTPUT`/`INOUT`
as a sink) — additive and symmetric with the existing `dbITerm` rule (a net
with no bterms, i.e. every net before this stage, tallies identically to
before); this was already the documented "Stage E can fold
primary-input/-output dbBTerms into the same driver/sink counts" extension
point left in `netlist_validation.h` since Stage C, and it's what lets
every Stage E1-generated net — including the fresh ones built from pool
material — validate cleanly under the *same* "exactly one driver, ≥1 sink"
rule as any other net.

**2. netlistgen never touches the Hypergraph engine.** The brief's
pseudocode calls `hg.add_vertex()` / `hg.set_bool_attr(vertex, name, true)`
— but the real `eda::Hypergraph` has no incremental mutation API at all:
vertices are strictly `dbInst`s assigned fresh at `buildFromBlock()` time,
and every attribute plane is destroyed on rebuild (see
`src/hypergraph/hypergraph.h`). A bare (combinational) PI/PO port has no
backing `dbInst` in the first place, so it structurally cannot be a
"vertex" in this codebase's model. Rather than inventing a synthetic
instance purely to hang a plane on, or giving `netlistgen` a new,
previously-nonexistent dependency on the `Hypergraph` engine (its
"Input / output contract" has always been "no hypergraph attribute planes
read or written" — true since Stage A, unbroken by this stage too),
`generateSynthetic`'s optional `out_rent_stats` parameter (`RentStats`)
returns the raw artifacts a caller needs to build the planes itself, once
it has its own `Hypergraph::buildFromBlock()`:

| Plane (brief's name) | Realized as | Source in `RentStats` |
|---|---|---|
| `hgm.is_pi` | **hyperedge** (net) bool plane — a port is a property of the net it terminates, and has no vertex of its own | `pi_nets` (`vector<dbNet*>`) |
| `hgm.is_po` | hyperedge bool plane | `po_nets` (`vector<dbNet*>`) |
| `hgm.is_boundary_buf` | **vertex** bool plane — these are real `dbInst`s | `boundary_buf_insts` (`vector<dbInst*>`) |
| `hgm.is_boundary_reg` | vertex bool plane | `boundary_reg_insts` (`vector<dbInst*>`) |

`test/netlistgen_rent_test.cpp` demonstrates the exact translation
(`applyRentPlanes`) and verifies all four planes end up populated correctly
— the plane *names and semantics* the brief specifies are fully realized,
just assembled by the caller (or a future `hg_metrics` consumer) rather than
by `netlistgen` itself.

### JSON / spec fields

All optional; E1 activates only when **both** `rent_k` and `rent_p` are set.

| Field | Maps to | Rule |
|-------|---------|------|
| `rent_k` | `spec.rent_k` | Must be `> 0`. |
| `rent_p` | `spec.rent_p` | `(0, 1.0]` used as given; `(1.0, 1.2]` accepted but **warned + clamped to 1.0** for `T_target` (degenerate but tolerable); `> 1.2` is a hard error. |
| `io_input_ratio` | `spec.io_input_ratio` | Optional, `(0, 1)`; defaults to `0.60`. |
| `io_pin_type_distribution` | `spec.io_pin_type_distribution` | Optional object `{"combinational", "buffered", "registered"}`; each fraction in `[0, 1]`, sum `1.0 ± 0.01` (normalised silently if within tolerance but not exact); defaults to `{0.70, 0.20, 0.10}`. |

`rent_k`/`rent_p` set on an otherwise-legacy (weighted-`masters`) spec is a
validation error, same treatment as `peak_avg_fanout`.

## Statistical cell-mix contract

- **Pin counting excludes power/ground.** `signalPinCount(master)` counts
  only `dbMTerm`s with `dbSigType::SIGNAL` or `CLOCK`; `VDD`/`VSS`
  (`POWER`/`GROUND`) never inflate a bucket. One shared helper used by both
  the synthetic and LEF paths.
- **Five combinational buckets** by signal-pin count: **2, 3, 4, 5, 6-or-more**
  (2-pin = buffer/inverter, 3-pin = 2-input gate, …). In synthetic mode the
  "6+" bucket is pinned to a single 7-pin representative (`COMB_7`, fanout 6)
  — raised from 6 so Mode B can target average fanouts up to 6 inclusive.
- **Two mutually exclusive combinational modes** (exactly one required when
  the statistical mix is engaged; both or neither → fail fast):
  - **Mode A — forward:** `combinational_pin_distribution`, five percentages
    that must sum to 100.
  - **Mode B — inverse:** `target_avg_fanout`, the desired average
    **fanout** — load pins per net, i.e. a cell's signal pins **excluding its
    one driver/output pin** (`#pins − 1`). Since the bucket anchors `xᵢ` are
    signal-pin **counts** (driver included), the equivalent pin-count target is
    `fanout + 1`, and the generator back-solves the **maximum-entropy**
    distribution `p_i = exp(θ·xᵢ)/Σ exp(θ·xⱼ)` over the anchors whose weighted
    mean equals that — a single scalar `θ` found by bisection
    (`maxEntropyDistribution`, plain `<cmath>`). This spreads mass across all
    buckets as evenly as the mean constraint allows, with no user-supplied
    prior. The derived distribution is logged. The target must lie inside the
    fanout range — the anchor range minus one — with the lower bound
    exclusive and the upper bound **inclusive** (synthetic: `(1, 6]`). At
    exactly the upper bound the distribution degenerates to 100% top bucket
    (every combinational cell is the 7-pin representative).
- **Sequential cells** get one fixed representative profile this stage
  (synthetic: `D, CK, Q` = 3 signal pins, with `CK` a real `dbSigType::CLOCK`
  pin so the cell reads as sequential via `isSequentialMaster`). No pin-count
  distribution for the sequential side yet.
- **LEF-mode classification is auto-detected.** A master is *sequential* if it
  has a clock pin — either a `dbMTerm` carrying `dbSigType::CLOCK`, **or** an
  input pin with a conventional clock name (`CK`, `CLK`, `CLOCK`, `CP`,
  case-insensitive; `isClockPinName`). Everything else is combinational and
  bucketed by signal-pin count. At spec-build time a lookup is built — per
  bucket, the matching masters; separately, the sequential class.
  - **Level-sensitive latches are dropped entirely** — used as neither
    sequential nor combinational masters. A latch is a non-clocked master with
    a gate/enable pin (`INPUT` named `G`/`GN`; `isLatchMaster`), which in
    Nangate45 is exactly `DLH`/`DLL`/`TLAT`. Excluded with a log, so they never
    appear as instances.
  - **Clock-gating cells are dropped entirely too** — they carry a clock pin
    but are not counted as sequential (or any) masters. A clock gate drives a
    gated-clock `OUTPUT` pin (named `GCK`/`GCLK`/`ECK`; `isClockGateMaster`),
    which in Nangate45 is exactly `CLKGATE`/`CLKGATETST`. Checked before the
    sequential classification (which they would otherwise satisfy) and excluded
    with a log.
  - A combinational master must resolve to **exactly one output** and
    `(pin_count − 1)` inputs; **multi-output combinational masters are
    excluded** from bucket population (logged). This is a load-bearing
    assumption for Stage D's DAG net formation (one output pin per
    combinational instance), enforced since Stage B.
  - A **requested** bucket (positive probability) with no matching master —
    or `sequential_ratio > 0` with an empty sequential class — is a
    **hard failure at spec-build time**, naming the empty bucket/class. No
    silent skip or weight redistribution.
  - **Nangate45 note:** its DFF clock pins are tagged `USE SIGNAL`, not
    `CLOCK`. Relying on the `CLOCK` sig type alone would find **no** sequential
    cells and wrongly exclude the DFFs (with `Q`+`QN`) as multi-output
    combinational, so the clock-pin-name fallback above is what makes flip-flops
    (`CK` pin) classify as sequential. Genuine multi-output combinational cells
    (`FA_X1`, `HA_X1`) are still correctly excluded.
- **`distribution_tolerance_pct`** (default 2.0): after generation, empirical
  proportions (and, in Mode B, the mean combinational signal-pin count) are
  compared to the targets. A deviation past tolerance is a **logged warning
  only, never a failure** — stochastic draws won't hit exact percentages,
  especially at small counts. Contrast the spec-build-time checks
  (distribution sum, mode exclusivity, target range, non-empty buckets),
  which stay hard failures.

## Net well-formedness validation (Stage C, extended in Stage E1)

`validateNetlist(block)` (`netlist_validation.h`) is a defensive, structural
correctness check — **independent of and in addition to** Stage D's
combinational-loop-freedom guarantee. It walks every `dbNet`, then every
`dbInst`, and confirms:

- **Exactly one driver** per net — exactly one connected terminal that
  supplies a signal: a `dbITerm` with `IoType::OUTPUT`, or (since Stage E1)
  a `dbBTerm` with `IoType::INPUT` (a primary input feeds the design from
  outside, so it drives the net from the internal design's perspective).
  Zero or more than one is a failure.
- **At least one sink** per net — at least one connected terminal that
  consumes the signal: a `dbITerm` with `IoType::INPUT`/`INOUT`, or a
  `dbBTerm` with `IoType::OUTPUT`/`INOUT`. Zero sinks is a failure (a
  driver with nothing to drive — dangling).
- **No dangling nets** — a net with zero connected terminals (iterms or
  bterms) is a failure.
- **No dangling instances** — every instance's DATA output(s) must actually
  drive something. "Data output" follows the D/Q-only convention
  (`isDataPin`): a sequential instance is alive only through its Q — a
  connected QN or clock output does not count. An instance whose data
  `OUTPUT` iterm(s) are *all* unconnected (`dbITerm::getNet() == nullptr`)
  is dead logic and fails validation, exactly as strictly as the net-level
  checks above (see "Guaranteed instance connectivity" above for why this
  matters and how `formNetsAcyclic`'s repair pass keeps it from firing in
  the first place). Checked instance-by-instance, independently of the
  net-level tallies — those are net-centric and cannot see a driver pin
  that was never connected to any net at all. An instance with no data
  output pin at all trivially passes (nothing for this check to apply to).
- **No control pins on nets** (the D/Q-only sequential pin constraint,
  added by the well-formedness audit,
  `docs/briefs/spike-netlistgen-wellformed-audit.md`) — every `dbITerm`
  connected to any net must be a data pin per `isDataPin`; a connected
  clock, async set/reset, scan-enable, or other non-`SIGNAL`/control pin
  fails validation, naming the pin and its instance. Runs **last**, after
  the dangling-instance check, so an instance kept "alive" only through a
  control pin is reported as dangling (the more fundamental defect) first.

Power/ground terminals (`dbSigType::POWER`/`GROUND`) are ignored on both
iterms and bterms; classification is **IoType-based** (the Stage A
refactor) — only the two `isDataPin`-based checks are additionally
name-aware, because libraries like Nangate45 tag control pins `USE SIGNAL`.
Stage E1 folds `dbBTerm`s into the *same*
per-net tally as `dbITerm`s (`tallyITerms`/`tallyBTerms`, two small
symmetric helpers) rather than a parallel rule — a PI's net is always
freshly built from never-connected or stolen sink pins, never an existing
net's driver, so the single-driver rule holds unchanged (see "Two
deliberate deviations" above). It
returns a `NetlistValidation { bool ok; std::string message; }` naming the
first offending net or instance. This is a **hard** structural property —
unlike the statistical `distribution_tolerance_pct` checks, which are
sampling-noise warnings. The CLI runs this automatically after generation
and **refuses to write any output if it fails** (fail-fast, reported before
the final design-summary statistics print); Stage 3 test code or other
callers can invoke it directly on their own blocks.

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

### Command-Line Options

The **argv-level** options (distinct from the JSON config *fields* documented
under "Standalone CLI" below — those are validated separately). Per the repo
CLI `--help`/usage convention (`CLAUDE.md`, `src/support/cli.h`): `--help`/`-h`
lists these and exits 0; a missing `<config.json>` prints the same block to
stderr and exits nonzero.

| Option | Required | Description |
|--------|----------|-------------|
| `<config.json>` | yes | Path to the JSON generation config; its schema is the "JSON config schema" table below. |
| `-verbosity <level>` | no | Debug verbosity (`--verbosity=<level>` too); unset/0 = default `info` phase markers. Levels 1–3 add plan detail, heartbeats, and per-net trace — see "Logging & verbosity". |

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
| `net_count` | `spec.num_nets` | `null`/absent → `-1` (as many as pools allow). A cap too low to connect every instance is a hard generation error (nonzero exit, no output files). |
| `fanout_range` `{min,max}` | `spec.min_fanout` / `max_fanout` | Optional. Load pins per net (fanout), driver excluded. |
| `tech_lef_path` | `spec.tech_lef_path` | Optional; engages LEF mode. |
| `cell_lef_paths` | `spec.cell_lef_paths` | Optional array. |
| `sequential_ratio` | `spec.sequential_ratio` | **Required > 0** since Stage D (bootstrap-source rule; enforced at generation time). |
| `combinational_pin_distribution` | `spec.combinational_pin_distribution` | Object keyed `"2","3","4","5","6+"`, sum 100. Mode A. |
| `target_avg_fanout` | `spec.target_avg_fanout` | Mode B (mutually exclusive with the distribution). |
| `distribution_tolerance_pct` | `spec.distribution_tolerance_pct` | Optional (default 2.0). |
| `peak_avg_fanout` | `spec.peak_avg_fanout` | Optional; engages peak fanout sub-clusters (see below). Must be `>` `(min_fanout + max_fanout) / 2`. |
| `peak_cluster_pct` | `spec.peak_cluster_pct` | Optional, `(0, 1)`; defaults to `0.10`. Ignored if `peak_avg_fanout` absent. |
| `num_peak_clusters` | `spec.num_peak_clusters` | Optional, `>= 1`; defaults to `1`. Ignored if `peak_avg_fanout` absent. |
| `rent_k` | `spec.rent_k` | Optional; engages Stage E1 (with `rent_p`). Must be `> 0`. |
| `rent_p` | `spec.rent_p` | Optional; engages Stage E1 (with `rent_k`). `(0, 1.2]`; `(1.0, 1.2]` warns + clamps to 1.0. |
| `io_input_ratio` | `spec.io_input_ratio` | Optional, `(0, 1)`; defaults to `0.60`. Ignored if `rent_k`/`rent_p` absent. |
| `io_pin_type_distribution` | `spec.io_pin_type_distribution` | Optional object `{"combinational","buffered","registered"}`, sum `1.0 ± 0.01`. Ignored if `rent_k`/`rent_p` absent. |
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

**Graceful failure (see "Error handling" in `CLAUDE.md`).** Bad input never
crashes: it produces a clear message and a nonzero exit. `runCliFromFile`
checks the config file opens, `parseCliConfig` catches the whole
`nlohmann::json::exception` hierarchy (malformed JSON, wrong field types),
and before writing, `validateAndWrite` creates each output path's parent
directory if it does not exist yet (including missing parents) and only
fails — cleanly, with no partial output — when a directory genuinely cannot
be created. In LEF-backed mode, `NetlistBuilder::loadLef` prechecks each LEF
path (`std::filesystem::exists`) before calling `lefin`, and wraps the
`lefin` reader calls in a boundary `try/catch` so a present-but-malformed
LEF — which makes OpenROAD's `createTechAndLib` throw — is contained as a
`return false` rather than a segfault. `netlistgen_cli`'s `main()` adds a
top-level catch-all backstop over the whole run.

Mode A example (explicit distribution, LEF-backed, both outputs):

```json
{
  "seed": 42,
  "instance_count": 5000,
  "net_count": null,
  "fanout_range": { "min": 2, "max": 6 },
  "tech_lef_path": "data/nangate45/Nangate45_tech.lef",
  "cell_lef_paths": ["data/nangate45/Nangate45_stdcell.lef"],
  "sequential_ratio": 0.1,
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
phase markers (parse config → generate → validate → write → done) and, at the
end, a **design summary** (`report()`, no id/prefix): total cell count split
into combinational vs sequential, the combinational cells' signal-pin-count
distribution (the `2/3/4/5/6+` buckets), the net count, the average fanout per
net, and the net fanout distribution. Fanout is the number of **load (sink)
pins**, i.e. net pins **excluding the driver** (computed as total pins minus the
OUTPUT driver pins), for both the average and the distribution. The
distribution shows one row per fanout up to 9, then collapses `10-50` and `>50`
into single rows so large-fanout nets keep the table compact. Sequential cells
are counted via `isSequentialMaster`, so the split is correct in both LEF mode
(flip-flops by clock pin) and synthetic mode (the `SEQ` representative carries a
real clock pin). Hard errors go to stderr. `-verbosity <level>` (group
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
written — still true as of Stage E1 (see "Primary I/O generation (Stage E1)"
above for why that boundary held even though the brief's own pseudocode
assumes otherwise). Input is a `SyntheticNetlistSpec`; output is a populated
`dbBlock` owned by the `NetlistBuilder`, plus the net count (or `-1` on
invalid spec). Consumed downstream by `Hypergraph::buildFromBlock()`.
`generateSynthetic` also takes two optional trailing out-parameters, each
`nullptr` by default so every pre-existing call site is unaffected:
- `std::vector<int>* out_cluster_id` — filled with per-instance cluster
  membership when peak fanout sub-clusters are engaged; generation-time
  bookkeeping never itself part of the `dbBlock`/`Hypergraph` model.
- `RentStats* out_rent_stats` — filled with Stage E1's statistics and
  generated artifacts (target/actual Rent parameters, pin-type counts, the
  raw `dbNet*`/`dbInst*` lists a caller needs to build `hgm.is_pi` /
  `is_po` / `is_boundary_buf` / `is_boundary_reg` planes itself) when
  `rent_k`/`rent_p` are engaged.

## Control parameters (`SyntheticNetlistSpec`)

| Field | Type | Default | Meaning |
|-------|------|---------|---------|
| `masters` | `std::vector<MasterSpec>` | — | Legacy weighted mix (ignored in statistical mode). |
| `num_insts` | `int` | `0` (must be `> 0`) | Number of instances. |
| `num_nets` | `int` | `-1` | Net cap; `-1` = as many as the pin pools allow. In statistical mode a cap too low to give every instance a connected output is a **hard error** (`-1` returned), never silent truncation. |
| `min_fanout` / `max_fanout` | `int` | `2` / `4` | Load pins per net (fanout), driver excluded. |
| `seed` | `uint32_t` | `1` | RNG seed; fixes output for a given spec. |
| `tech_lef_path` | `std::optional<std::string>` | unset | If set, load real tech (and its macros) via `lefin`. |
| `cell_lef_paths` | `std::vector<std::string>` | `{}` | Extra cell LEF(s) against that tech. |
| `sequential_ratio` | `std::optional<double>` | unset (→ 0.0, **fails validation**) | Fraction of instances that are sequential; must be `> 0` in statistical mode (Stage D bootstrap-source rule). |
| `combinational_pin_distribution` | `std::optional<array<double,5>>` | unset | Mode A percentages `[2,3,4,5,6+]`, sum 100. |
| `target_avg_fanout` | `std::optional<double>` | unset | Mode B target mean fanout (signal pins minus the driver, `#pins−1`); synthetic range `(1, 6]` (upper bound inclusive). |
| `distribution_tolerance_pct` | `double` | `2.0` | Post-gen deviation warning threshold. |
| `peak_avg_fanout` | `std::optional<double>` | unset | Engages peak fanout sub-clusters; target mean fanout for intra-cluster nets. Must be `>` `(min_fanout + max_fanout) / 2`. Requires the statistical mix. |
| `peak_cluster_pct` | `std::optional<double>` | unset (→ `0.10` when engaged) | Fraction of instances in peak clusters. Must be in `(0, 1)` if set. Ignored if `peak_avg_fanout` unset. |
| `num_peak_clusters` | `std::optional<int>` | unset (→ `1` when engaged) | Number of peak clusters. Must be `>= 1` if set. Ignored if `peak_avg_fanout` unset. |
| `rent_k` | `std::optional<double>` | unset | Engages Stage E1 (with `rent_p`); Rent coefficient. Must be `> 0`. Requires the statistical mix. |
| `rent_p` | `std::optional<double>` | unset | Engages Stage E1 (with `rent_k`); Rent exponent. `(0, 1.2]`; `(1.0, 1.2]` warns + clamps to 1.0 for `T_target`. |
| `io_input_ratio` | `std::optional<double>` | unset (→ `0.60` when engaged) | Fraction of `T` assigned as PIs. Must be in `(0, 1)` if set. Ignored if `rent_k`/`rent_p` unset. |
| `io_pin_type_distribution` | `std::optional<IoPinTypeDistribution>` | unset (→ `{0.70, 0.20, 0.10}` when engaged) | Pin-type split (combinational/buffered/registered), sum `1.0 ± 0.01`. Ignored if `rent_k`/`rent_p` unset. |

`MasterSpec`: `name`, `num_inputs` (2), `num_outputs` (1), `weight` (1.0).
`IoPinTypeDistribution`: `combinational` (0.70), `buffered` (0.20),
`registered` (0.10).

The statistical mix is engaged when `tech_lef_path`, `sequential_ratio`,
`combinational_pin_distribution`, or `target_avg_fanout` is set
(`SyntheticNetlistSpec::usesStatisticalMix()`).

## Determinism

Output for a given `(spec, seed)` is reproducible across runs on a fixed
toolchain: the generator draws from a seeded `std::mt19937` and instance /
pin iteration order is fixed by OpenDB set order. Determinism carries through the CLI: a
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
spec.sequential_ratio = 0.15;     // DFFs detected via CK pin name
spec.target_avg_fanout = 3.5;     // avg fanout = signal pins minus the driver
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
      netlistgen_stagec_test netlistgen_staged_test netlistgen_peak_cluster_test \
      netlistgen_rent_test netlistgen_wellformed_test netlistgen_link_smoke \
      netlistgen_cli
ctest --test-dir build -R "netlistgen" --output-on-failure
```

- `test/netlistgen_test.cpp` — Stage A behavior (no data files): exact CSR on
  a hand-built 3-inst/2-net case, spec conformance, net-count limiting, seed
  determinism.
- `test/netlistgen_stageb_test.cpp` — Stage B (needs `EDA_LAB_DATA_DIR`):
  max-entropy solve correctness, spec-config validation (both/neither mode,
  bad sum, out-of-range target), signal-pin counting on a Nangate45 NAND2,
  LEF-backed generation, multi-output exclusion, empty-bucket/empty-sequential
  fail-fast (using `data/synth_cells/twobucket.lef` — plus `dff_seq.lef`
  where a non-empty sequential class is needed since Stage D), large-run
  statistical validation, Mode B mean, determinism, and die-area sizing.
- `test/netlistgen_stagec_test.cpp` — Stage C (needs `EDA_LAB_DATA_DIR` and the
  built `netlistgen_cli` binary): well-formedness passing on synthetic +
  LEF-backed output and flagging hand-built dangling / driverless /
  multi-driver / sinkless nets; DEF + `.odb` writers producing files; JSON
  parsing (Mode A/B valid, missing `instance_count`, no output path,
  malformed JSON); the CLI validate-before-write fail-fast on a malformed
  block; and a CLI smoke test that spawns `netlistgen_cli`, round-trips the
  DEF back through `defin`, and confirms instance/net counts.
- `test/netlistgen_staged_test.cpp` — Stage D (needs `EDA_LAB_DATA_DIR` and
  the built `netlistgen_cli` binary): detector sanity on a hand-built
  two-inverter loop (and a register feedback loop that must NOT count);
  loop-freedom via DFS cycle detection (sequential instances cut at the D/Q
  boundary) on synthetic runs at 300 / 5 000 / 50 000 instances × 3 seeds
  and LEF-backed runs × 2 seeds, plus the stronger construction invariant
  (every comb→comb edge follows creation order) and
  `Hypergraph::buildFromBlock` consuming the acyclic block unmodified; the
  `sequential_ratio > 0` fail-fast (zero, unset, just-above-zero, legacy
  exemption); the thin-pool bootstrap edge case (loosened/skipped tail,
  never a sinkless net, bit-identical on rerun); and a CLI spawn whose DEF
  output round-trips through `defin` loop-free.
- `test/netlistgen_peak_cluster_test.cpp` — peak fanout sub-clusters (no data
  files needed): no-peak-params leaves `out_cluster_id` empty; basic
  single-cluster generation (correct instance/cluster counts, Stage D DAG
  still valid, cluster-driven net average fanout measurably above the global
  average and within 20% of `peak_avg_fanout`); `assignPeakClusters` directly
  (multi-cluster sizing within ±1, determinism for a fixed seed) and threaded
  through `generateSynthetic` for `num_peak_clusters=3`; all four validation
  failures (below-background target, out-of-range `peak_cluster_pct`,
  `num_peak_clusters=0`, legacy-mix rejection) plus the "ignored when
  `peak_avg_fanout` absent" rule; and the `num_peak_clusters`/`peak_cluster_pct`
  defaults.
- `test/netlistgen_rent_test.cpp` — Stage E1 (no data files needed; links
  `hypergraph` too, since the test performs the plane translation
  `netlistgen` itself deliberately doesn't — see "Primary I/O generation
  (Stage E1)" above): no-E1-params leaves `RentStats.engaged` false and
  every `hgm.*` plane absent; a basic 2000-inst run (`T_target`/`T_actual`
  within tolerance, PI/PO split matching `io_input_ratio`, `hgm.is_pi`/
  `is_po`/`is_boundary_buf`/`is_boundary_reg` all populated correctly via
  `applyRentPlanes`, `is_boundary_reg` never set on an internal FF,
  `p_actual` finite and positive); a custom `io_input_ratio`; an
  all-combinational `io_pin_type_distribution` producing zero boundary
  cells; combining with peak fanout sub-clusters (per-cluster + background
  Rent stats, a non-asserting soft check that cluster `p_c` tends above
  background `p_bg`); the strict **no-dangling-instance** invariant
  (`NoDanglingInstancesAfterE1`, across several instance counts and seeds:
  zero fully-isolated instances before and after Stage E1, and the
  pre-existing dead-output-instance count only ever goes down, never up —
  see "Two deliberate deviations" above) plus a `validateNetlist(...).ok`
  check folded into every generating test; all validation failures
  (exactly-one-of `rent_k`/`rent_p`, `rent_p > 1.2`, the `(1.0, 1.2]`
  warn-and-clamp case, bad `io_pin_type_distribution` sum, out-of-range
  `io_input_ratio`, legacy-mix rejection); and a small-design `T`-capping
  run that completes without crashing.
- `test/netlistgen_wellformed_test.cpp` — the well-formedness audit
  (`docs/briefs/spike-netlistgen-wellformed-audit.md`; needs
  `EDA_LAB_DATA_DIR`): `isDataPin` unit coverage (synthetic
  representatives; a hand-built all-`USE SIGNAL` scan FF where only the
  name rule separates D/SI/Q from CK/RN/SE/QN; a combinational full-adder
  lookalike whose `S` output must be unaffected); D/Q-only end to end in
  LEF-backed and synthetic mode with Stage E1 engaged (every connected
  iterm is a data pin, sequential connections limited to D/SI/Q resp.
  i1/o0, clock pins never connected); the repair pass under an
  all-sequential tight-fanout config that forces it to run; the
  validateNetlist hardening (a connected QN does not save a dangling Q; a
  CLOCK-typed iterm on an otherwise valid net fails, naming pin and
  instance); and the `num_nets` cap policy (too-low cap → `-1` from
  `generateSynthetic`; the in-process CLI exits nonzero and writes no
  output file; a generous cap still succeeds).
- `test/netlistgen_link_smoke.cpp` — library-linkage guard.

Scale reference: ~500k insts / ~1.4M pins generate in about 2 s (synthetic).

## Open questions / follow-on

- **Stage E2 (not yet implemented)** — structural Verilog output
  (`output_verilog_path`), a separate brief. Note this repo's Stage E1 does
  **not** relax `sequential_ratio > 0` the way an earlier revision of this
  doc predicted ("... OR `primary_input_count > 0`") — PI/PO generation
  turned out to run as a pass *after* Stage D's DAG formation rather than
  participating in its bootstrap, so that requirement stands unchanged;
  Stage E2 (or a future revision of this note) should reassess.
- Custom prior distribution for Mode B's max-entropy tilt (currently uniform).
- YAML config support alongside JSON.
- Peak fanout sub-clusters: no LEF-mode-specific test coverage yet (the
  mechanism is LEF-agnostic — it only touches `dbITerm`/`dbInst`, not master
  origin — but the test suite currently only exercises synthetic mode); no
  inter-cluster bias (clusters never preferentially wire to each other, only
  to their own members or the shared background).
- Stage E1: no LEF-mode-specific test coverage yet (same LEF-agnostic
  argument as peak clusters — it only touches `dbITerm`/`dbBTerm`/`dbInst`).
  (`writeDef`'s generic `odb::DefOut` serializer already emits a correct
  `PINS` section for the new `dbBTerm`s with no code changes — verified
  manually: a Stage E1 DEF round-trips its ports with correct name / net /
  direction. No pin placement/geometry, since these bTerms carry none.)
  `hg_metrics` itself (the downstream consumer both this feature and peak
  fanout sub-clusters exist for) is not yet implemented in this repo.
