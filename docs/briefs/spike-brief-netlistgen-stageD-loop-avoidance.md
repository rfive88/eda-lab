# Spike Brief — Stage D of 5: Combinational-Loop Avoidance

**Target directory:** `src/engines/netlistgen/`
**Prerequisites:** Stage C complete and its acceptance gate green — DEF/`.odb`
writers and the CLI executable exist and are tested, on top of Stage B's
LEF-backed generation and statistical cell mix. Net formation currently
uses unconstrained random driver/receiver pairing, so any DEF/`.odb` files
produced by Stage C's CLI so far may contain combinational loops (see
Stage C's caveat).
**Note on why this stage comes after Stage C**: this algorithm only
actually depends on Stage B's statistical-mix work, not on Stage C's
writers/CLI — the two were reordered at the user's request so real DEF
output was available to inspect sooner. No content in this stage assumes
anything from Stage C beyond "it exists"; feel free to treat Stage B as the
true algorithmic prerequisite if that's ever useful context.
**Blocks:** Stage E (primary I/O ports + Verilog writer)
**This stage completes "Phase 1"**: after this stage's acceptance gate is
green, `netlistgen` is ready to unblock Stage 3 benchmark generation — and
the CLI from Stage C now produces genuinely valid (loop-free) output.
**Do not proceed past this stage's acceptance gate (Section 5) in the same session.**

This stage retrofits net formation to guarantee combinational-loop freedom
by construction. It's a self-contained algorithm change on top of Stage B's
already-tested statistical generation — Stage B proved the *distribution*
of cell types/pin counts is correct; this stage makes the *connectivity*
provably acyclic (among purely combinational cells) without disturbing
that distribution more than structurally necessary.

**Suggested sanity check once this lands**: regenerate a DEF file with the
same config you used to manually inspect Stage C's CLI output, and confirm
it's now structurally different (should reflect the ordered/constrained net
formation) — a good concrete way to see this stage's effect, not just a
passing test.

---

## 1. Goal for this stage

**Guarantee absence of combinational loops** in generated netlists by
construction, not by post-hoc detection.

Random driver/receiver pairing (Stage B's model) can create a cycle purely
among combinational cells — a real correctness bug for any downstream tool
that assumes a valid digital netlist.

## 2. Design: instantiation order as topological (DAG) order

- Instances are already created in a sequential loop (index
  `0..instance_count-1`, rolling sequential/combinational and bucket per
  Stage B). Reuse this existing index directly as a topological order — no
  separate "level" concept needed.
- **Sequential cell outputs (Q) are valid net drivers immediately,
  regardless of creation order** — a register's output is always a safe
  source, since it's the loop-breaker.
- **Sequential cell inputs (D) accept a driver from anywhere** — any
  combinational output (earlier or later in creation order), any Q — with
  no ordering constraint. A D pin can safely depend on nets that
  transitively depend on its own instance's Q; that's a legitimate
  sequential feedback loop, not a combinational one.
- **Combinational cell inputs may only be driven by**: sequential Q
  outputs (any), or combinational outputs from instances **created
  earlier** in the index order. Never from a combinational instance created
  later, and never (transitively) from itself.
- **Practical net-formation change**: process net formation in the same
  order instances are created. For each combinational instance's single
  output pin (Stage B's multi-output-exclusion work makes this assumption
  safe), form its net's receiver set by drawing only from the currently
  valid candidate pool — unused input iterms belonging to (a) any
  sequential D pin, or (b) any combinational instance not yet processed
  (i.e. later in creation order). This is an additive filter on top of
  Stage B's existing fanout-range/target-average driven receiver-count
  logic — the *count* of receivers is still governed by that logic; only
  the *eligibility* of candidates changes.

## 3. Add the bootstrap-source validation

- **`sequential_ratio` must be > 0 in this stage.** Sequential Q outputs
  are the only bootstrap source for the first combinational instances in
  generation order — no primary-input ports exist yet (those arrive in
  Stage E, which relaxes this constraint back to "at least one of
  `sequential_ratio > 0` or `primary_input_count > 0`"). **Fail fast at
  spec-build time if `sequential_ratio <= 0`**, with a message explaining
  why (no valid signal source exists to start the combinational DAG from).
  This is a new validation added in this stage — Stage B deliberately left
  it out since it wasn't yet meaningful.
- **Bootstrap concern**: the earliest instances in creation order have a
  thin candidate pool (only sequential Q outputs exist yet, until enough
  instances have been processed). If, even with `sequential_ratio > 0`,
  the very first few combinational instances still can't find enough
  receivers to satisfy the minimum of `fanout_range`, either loosen that
  particular net's receiver count for early instances (log why) or fail
  fast with a clear diagnostic rather than looping/stalling — pick one
  deterministic, documented behavior and justify it in the README rather
  than leaving it unspecified.

## 4. Implementation note

This section changes the receiver-eligibility ruleset for net formation,
not Stage B's pin-count/fanout statistical machinery — keep those concerns
separated in the implementation so Stage E (adding primary ports as another
valid driver/receiver class) is a small extension, not a rewrite.

Expect the empirical fanout distribution measured by Stage B's statistical
validation test to shift slightly once this constraint is in place
(early-order instances have a thinner candidate pool than late-order ones)
— this is a known, expected structural effect, not a regression. Compare
against Stage B's baseline measurement when documenting the expected
magnitude.

## 5. Docs for this stage

- Update **`src/engines/netlistgen/README.md`**: document the
  combinational-loop-avoidance guarantee, the instantiation-order-as-DAG
  design, and the bootstrap-source requirement (`sequential_ratio > 0`,
  relaxed in Stage E). Note the expected fanout-distribution shift relative
  to Stage B's baseline.
- Update **`src/engines/netlistgen/FLOW.md`**: add a diagram for the
  ordered net-formation loop with the loop-avoidance filter, replacing (or
  supplementing) Stage B's simpler net-formation diagram.

## 6. Tests — acceptance gate for Stage E

- **Combinational-loop-freedom test (the key new correctness test)**:
  generate a netlist, build a graph where combinational instances are
  nodes and edges follow driver→receiver connectivity, sequential
  instances are cut at the D/Q boundary (Q = source with no incoming edge
  from this instance's own D; D = sink with no outgoing edge to this
  instance's own Q), and run cycle detection (e.g. DFS with recursion-stack
  tracking). Assert no cycle exists. Run at multiple instance counts
  (small, medium, large) and multiple seeds.
- Spec-build validation test: `sequential_ratio <= 0` fails fast with a
  clear message.
- Bootstrap edge-case test: a config with `sequential_ratio` just above 0
  and a small `instance_count` — confirm the chosen deterministic behavior
  from Section 3 (loosened early receiver count, or fail-fast) actually
  happens as documented, not silently something else.
- Re-run Stage B's statistical validation test with loop avoidance active;
  confirm results are still within `distribution_tolerance_pct` (or, if
  not, that the deviation is understood, documented, and consistent with
  Section 4's expected shift rather than an unexplained gap).
- Re-run Stage C's CLI smoke test; confirm DEF/`.odb` output is now
  loop-free (this is the concrete before/after check suggested at the top
  of this brief).
- Re-run Stage C's net well-formedness validation (single driver, ≥1 sink,
  no dangling nets) against this stage's output — the DAG-ordered
  net-formation retrofit changes *how* nets are built, so it's worth
  explicit confirmation that well-formedness still holds, not just
  loop-freedom.
- Confirm `Hypergraph::buildFromBlock()` still works unmodified against the
  now-acyclic `dbBlock*` output.

**Do not start Stage E in this session.** Report back once all of the above
are green — this completes "Phase 1" and unblocks Stage 3.

## 7. Explicitly out of scope for this stage

- Primary I/O ports, Verilog writer — Stage E.
- Any relaxation of the `sequential_ratio > 0` requirement — that's Stage
  E's job once `primary_input_count` exists as an alternate bootstrap
  source.
