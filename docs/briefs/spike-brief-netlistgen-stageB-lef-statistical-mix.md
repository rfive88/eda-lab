# Spike Brief — Stage B of 5: LEF-Backed Generation + Statistical Cell Mix

**Target directory:** `src/engines/netlistgen/`
**Prerequisites:** Stage A complete and its acceptance gate green — engine
lives at `src/engines/netlistgen/`, builds as a library target, pin access
is IoType-based (not name-based), existing test suite passes unmodified.
**Blocks:** Stage C (DEF/`.odb` writers, CLI executable) — note the
execution order was reordered at the user's request so CLI output is
available to inspect sooner; combinational-loop avoidance (Stage D) only
algorithmically depends on this stage, not on Stage C, so it's fine for
Stage C to run first.
**Do not proceed past this stage's acceptance gate (Section 5) in the same session.**

This stage adds LEF loading and the statistical cell-mix machinery
(including the max-entropy solve). **It deliberately does not yet address
combinational-loop safety** — net formation still uses today's
driver/receiver pairing mechanism, just now respecting LEF pin
identities and statistical bucket selection. Loop avoidance is a
self-contained algorithm change and gets its own stage (Stage D) so it can
be reasoned about and tested in isolation, on top of a version of the
generator that's already known to produce statistically-correct output.

---

## 1. Goal for this stage

1. Add optional LEF loading (real tech + cell masters) alongside the
   existing fast synthetic mode.
2. Add statistical, non-named cell mix control (sequential/combinational
   ratio + pin-count-bucket distribution, two modes, including the
   max-entropy solve for the inverse mode).

## 2. Extend `SyntheticNetlistSpec`

Add optional fields (extend, do not replace — Stage A's regression-safety
requirement extends forward: with no LEF fields set, output for a given
`(spec, seed)` must remain bit-identical to Stage A's behavior):

- `tech_lef_path` (optional `std::string`) — if set, `NetlistBuilder` loads
  real tech via `lefin::createTechAndLib` instead of building a synthetic
  tech/lib. (Check current 3-arg signature quirk already noted in project
  memory.)
- `cell_lef_paths` (optional `std::vector<std::string>`) — additional cell
  LEF(s) loaded against the same tech.
- `sequential_ratio` (`double`, 0.0–1.0): fraction of instances that are
  sequential; the remainder is combinational and governed by whichever of
  the two modes below is active. **No lower-bound constraint in this
  stage** — `0.0` is valid here (net formation isn't order-sensitive yet).
  Stage D will add a `sequential_ratio > 0` requirement as a direct
  consequence of introducing DAG-ordered net formation; don't add that
  constraint in this stage, since it isn't meaningful until Stage D's
  algorithm exists.
- **Two mutually exclusive combinational-mix modes** — validate at
  spec-build time that exactly one is set; fail fast if both or neither are
  present:

  **Mode A — forward (explicit distribution):**
  - `combinational_pin_distribution`: percentages across five pin-count
    buckets — **2, 3, 4, 5, 6-or-more** — that must sum to 100 (or 1.0;
    pick one convention and validate it, fail fast via `utl::Logger` if the
    sum is off). "Pin count" = total **signal** pins (inputs + outputs
    combined): 2-pin = buffer/inverter (1 in + 1 out), 3-pin = 2-input NAND
    (2 in + 1 out), etc.

  **Mode B — inverse (target average fanout):**
  - `target_avg_fanout` (`double`): desired average net fanout. The
    generator back-solves a `combinational_pin_distribution` to hit this
    target, since total instance-side signal pins (driven by the pin-count
    distribution) is what net formation draws from — average net fanout ≈
    total_pins / net_count.
  - **Underdetermination is real**: many different bucket-percentage shapes
    can produce the same mean (e.g. 100% bucket-4 and a 50/50 split of
    bucket-3/bucket-5 both average to 4). Rather than pick an arbitrary
    shape, solve for the **maximum-entropy distribution** over the bucket
    anchors subject to the mean constraint — this is the distribution that
    spreads mass across all buckets as evenly as the mean constraint
    allows, with no extra bias, and needs no user-supplied prior shape.
  - **Method**: let `x = [2, 3, 4, 5, 6]` be the bucket pin-count anchors
    in synthetic mode (in LEF mode, use each bucket's *measured average
    signal-pin count among loaded masters currently populating that
    bucket*, computed after the LEF library scan). Solve for a single
    scalar `theta` such that the tilted distribution
    `p_i = exp(theta * x_i) / sum_j(exp(theta * x_j))` has mean
    `sum_i(p_i * x_i) = target_avg_fanout`. This is a standard 1D
    root-find (e.g. bisection over a bounded `theta` range) — no new
    numerical dependency needed, plain C++ `<cmath>` suffices. The
    resulting `p_i` becomes the effective `combinational_pin_distribution`
    for generation (log it via `utl::Logger` so the user can see what
    shape was derived).
  - **Validate at spec-build time** that `target_avg_fanout` is strictly
    within `[min(x), max(x)]` (2 to 6 in synthetic mode) — no finite
    `theta` can push the mean outside the anchor range; fail fast with a
    clear message if it's out of bounds.

  **Shared, regardless of mode:**
  - **Critical: pin counting excludes power/ground.** In LEF mode, only
    count `dbMTerm`s with `dbSigType::SIGNAL` or `dbSigType::CLOCK`
    — `POWER`/`GROUND` terms (`VDD`/`VSS`) must not inflate the bucket.
    Synthetic-mode masters have no power pins to begin with, so this only
    matters for the LEF path, but implement it as one shared counting
    helper so both paths are provably consistent.
  - **Sequential cells get one fixed representative profile** for now
    (e.g. D, CK, Q = 3 signal pins in synthetic mode) — no pin-count
    distribution for the sequential side. Note as an open extension if
    finer control is wanted later.
  - **Classification in LEF mode is auto-detected**: a master is
    sequential if any of its `dbMTerm`s has `dbSigType::CLOCK`; everything
    else in the loaded library is combinational and gets bucketed by signal
    pin count. No explicit sequential/combinational list is ever passed in.
  - **"6-or-more" bucket in synthetic mode gets a fixed pin count of 6**
    (not a range) — keep this simple; note as tunable later if Stage 3
    benchmarks need wider tails.
  - **LEF-mode bucket selection**: build a lookup at spec-build time — for
    each of the 5 combinational buckets, the list of loaded masters whose
    signal-pin count matches (bucket 6+ = signal-pin count >= 6);
    separately, the list of masters auto-detected as sequential. At
    instance-generation time, roll `sequential_ratio` first, then (if
    combinational) roll the bucket distribution, then pick uniformly at
    random among matching masters in that bucket/class. **If a bucket has
    zero matching masters in the loaded library, fail fast at spec-build
    time** (not partway through generation) with a clear message naming
    the empty bucket — don't silently skip or redistribute weight.
  - **Multi-output combinational masters**: if the loaded library contains
    combinational masters with more than one output pin, exclude them from
    bucket population at spec-build time and log why. Every combinational
    master must resolve to exactly **one** output pin and
    `(pin_count - 1)` input pins — this isn't exercised by anything in this
    stage's net-formation logic yet, but it's a load-bearing assumption for
    Stage D's DAG algorithm, so enforce it now while the bucket-population
    code is being written.
- `distribution_tolerance_pct` (optional `double`, default e.g. 2.0): after
  generation completes, compute the empirical `sequential_ratio` and bucket
  proportions (and, in Mode B, empirical average net fanout) and compare
  against the requested targets. **This is a logged warning only, never a
  generation failure** — stochastic per-instance draws naturally won't hit
  exact percentages, especially at small instance counts. Contrast with the
  spec-build-time validations above (distribution summing to 100,
  mutual-exclusivity, target-average-in-range, non-empty buckets), which
  remain hard failures since those are structural/config errors, not
  sampling noise.
- Everything else (`instance_count`, `net_count`, fanout range, `seed`)
  unchanged in meaning and behavior.

## 3. `NetlistBuilder` changes

- Add a code path that, when `tech_lef_path` is set, uses `lefin` to build
  tech + lib instead of the current synthetic tech/lib construction.
- Master-freeze protocol: confirm whether `lefin`-loaded masters arrive
  pre-frozen; do not assume — verify against the pinned SHA and document the
  finding in a code comment (this project already has one hard-won ODB
  quirk-lesson per mistake avoided; add this as another if it differs from
  the synthetic path).
- Add a **DIEAREA sizing helper**: auto-compute a bounding box from
  instance count and a nominal site pitch (pull actual site dimensions from
  the loaded tech LEF if available; fall back to a hardcoded nominal pitch
  in synthetic-only mode). This stage doesn't write DEF yet (Stage C does)
  but the sizing logic belongs with the builder — Stage C's writer will
  just consume it. Instances remain `UNPLACED`.

## 4. Docs for this stage

- Update **`src/engines/netlistgen/README.md`**: document LEF-backed mode,
  the statistical cell-mix contract (both modes), the max-entropy solve.
  Note explicitly that net formation does **not** yet guarantee
  combinational-loop freedom — that lands in Stage D — so this stage's
  output shouldn't be treated as a valid acyclic netlist yet. Open
  questions: custom prior distribution for Mode B's max-entropy tilt
  instead of defaulting to uniform.
- Update **`src/engines/netlistgen/FLOW.md`**: add diagrams for the LEF
  loading branch, statistical bucket selection, and the max-entropy solve.

## 5. Tests — acceptance gate for Stage C

- LEF-backed generation tests using `data/nangate45` LEF fixtures, small
  instance counts for speed.
- Spec-build validation tests: valid config for both Mode A and Mode B,
  missing required field, `combinational_pin_distribution` not summing to
  100/1.0, both modes set simultaneously, neither mode set,
  `target_avg_fanout` outside `[2, 6]` — all must fail fast at spec-build
  time.
- Pin-counting correctness test: for a known Nangate45 master (e.g. a
  2-input NAND), confirm the shared counting helper returns signal-pin
  count only, excluding `VDD`/`VSS`.
- Bucket-population test: confirm spec-build fails fast if a requested
  bucket has zero matching masters in the loaded LEF library.
- Multi-output exclusion test: confirm a multi-output combinational master
  in a loaded library is excluded from bucket population, with a logged
  reason.
- **Max-entropy solve correctness test (Mode B)**: for a chosen
  `target_avg_fanout`, verify the solved `theta`/bucket distribution (a)
  has a weighted mean matching the target within numerical tolerance, and
  (b) actually spreads mass across more than 2 buckets — confirms the
  tilting method is doing real work, not collapsing to interpolation.
- Statistical validation test: generate a reasonably large synthetic
  instance count (e.g. 10k) with a known `sequential_ratio` and
  `combinational_pin_distribution` (or `target_avg_fanout`), then check
  observed proportions/average fanout are within `distribution_tolerance_pct`
  of the requested target, and confirm a deliberately mismatched check logs
  a warning without failing generation. (This should hold cleanly in this
  stage, without the deviation caveat that Stage D's loop-avoidance
  constraint will introduce — useful as a baseline to compare against
  after Stage D lands.)
- Confirm `Hypergraph::buildFromBlock()` still works unmodified against
  both synthetic-mode and LEF-backed-mode `dbBlock*` output.

**Do not start Stage C in this session.** Report back once all of the above
are green.

## 6. Explicitly out of scope for this stage

- DEF/`.odb` writers, CLI executable — Stage C (comes next).
- Combinational-loop avoidance — Stage D.
- Primary I/O ports, Verilog writer — Stage E.
- Pin-count distribution for sequential cells (single representative
  profile only).
- Configurable range for the "6-or-more" bucket (fixed at 6).
