# Spike Brief — Netlist Well-Formedness Audit and Sequential Pin Constraint

## Goal

Ensure the netlist produced after Stage D is structurally well-formed per the following
definition, and that this is enforced as a hard gate before any statistics are printed:

1. **Single driver per net**: every net has exactly one driving pin (PI port or a cell
   output pin). *(Already exists in `validateNetlist` — verify it is correct.)*
2. **At least one load per net**: every net drives at least one sink pin (PO port or a
   cell input pin). *(Already exists — verify.)*
3. **No dangling instances**: every cell instance has at least one signal output pin
   connected to a net. *(Added in commit `19cac36` — verify correctness.)*
4. **Sequential cell data-pin constraint**: only the **D input** and **Q output** of a
   sequential cell participate in data-net connectivity. Clock, reset, set, scan-in,
   scan-enable, scan-out, test-mode, and all other control/clock pins must **not** be
   connected to any net in the generated netlist. *(This constraint was explicitly
   specified but does not appear in the summary — status unknown. This is the primary
   gap this brief closes.)*

## Background

Read `docs/tmp/netlist-dangling-instance-fix-summary.md` before starting. Points 1–3
above were addressed in commits `11b909e` and `19cac36`. The summary is silent on
point 4, which was explicitly instructed: only the D pin (data input) and Q pin (data
output) of a sequential cell should be wired into signal nets. Clock/reset/scan are
global signals in a real flow and must not appear as signal-net endpoints in synthetic
netlists. This brief audits whether point 4 was actually implemented and adds whatever
is missing.

## Source-read requirement

**Before writing a single line of implementation**, read:

1. All source files under `src/engines/netlistgen/` — understand:
   - How sequential cell instances are created in Stage A (synthetic vs LEF-backed)
   - How Stage B samples fanout and assigns pins (which pins are eligible for drivers/sinks)
   - How Stage D's `formNetsAcyclic` selects driver and sink pins — specifically whether
     it filters pins by type before adding them to the eligible pool
   - How Stage D's dangling-instance repair pass selects sink pins for repair — verify it
     cannot accidentally assign a clock or reset pin as a data sink
   - `instanceHasConnectedOutput` in `netlist_validation.cpp` — verify it counts only
     signal output pins (Q), not clock outputs or scan chains
   - `validateNetlist` — understand all current checks

2. For LEF-backed mode: look up `dbMTerm::getSigType()` and `dbSigType` — understand
   which signal types exist (SIGNAL, CLOCK, RESET, SCAN, ANALOG, POWER, GROUND) and
   how to filter to SIGNAL only.

3. For synthetic mode (no LEF): understand how synthetic sequential cell pin roles are
   currently represented. If pin roles are not currently tracked, you will need to add
   them (see Section 4).

Only after completing this read should any implementation begin.

---

## Section 1 — Sequential cell pin semantics

### The constraint

For any sequential (flip-flop or latch) cell instance in the netlist:

- **The D pin** (data input): eligible to appear as a **sink** on a signal net.
- **The Q pin** (data output): eligible to appear as a **driver** on a signal net.
- **All other pins** (clk, rst, set, scan_in, scan_en, scan_out, test, QN, etc.):
  **not connected to any net**. These are left unconnected in the generated netlist.

This applies to both LEF-backed and synthetic mode.

### LEF-backed mode

Use `dbMTerm::getSigType()`. Only connect pins where `getSigType() == dbSigType::SIGNAL`.
Do **not** connect pins of type CLOCK, RESET, SCAN, ANALOG, POWER, or GROUND.

Additionally filter by direction within the SIGNAL set:
- `dbIoType::INPUT` → eligible as sink (D pin)
- `dbIoType::OUTPUT` → eligible as driver (Q pin)
- `dbIoType::INOUT` → eligible as either (rare for FFs, handle conservatively as sink)

**Do not assume** that a real FF cell has only one SIGNAL INPUT and one SIGNAL OUTPUT.
Some library cells (e.g. scan-enabled FFs in Nangate45) have SI (scan-in) as a SIGNAL
INPUT alongside D. If SI is SIGNAL/INPUT, it should be treated the same as D — eligible
as a data sink. This is correct: SI is a data path, just muxed behind scan enable.
Scan-enable (SE) however is typically typed SIGNAL/INPUT as well in some LEFs — check
the actual Nangate45 cell types used and document which pins are excluded. If uncertain,
default to connecting all SIGNAL/INPUT pins for combinational cells and all SIGNAL/INPUT
pins for sequential cells, but **never** connect CLOCK-typed pins regardless of direction.

### Synthetic mode

In synthetic mode there is no LEF to query. The current code likely marks instances as
`is_sequential` via a bool flag or similar. For synthetic sequential instances, the
convention is:

- Exactly **one** input pin role: D (index 0 of the instance's input pin list, or
  whatever the current synthetic model uses)
- Exactly **one** output pin role: Q (index 0 of the output pin list)
- No other pins exist on a synthetic FF (synthetic cells are structural skeletons, not
  real cells)

Verify this matches the current synthetic cell model. If synthetic FFs currently model
multiple input/output slots, restrict net formation to slot 0 (D) and slot 0 (Q) only,
explicitly. Document the convention in `README.md`.

---

## Section 2 — Audit of `instanceHasConnectedOutput`

The check added in `19cac36` asks: "does this instance have at least one connected
signal output pin?" Verify:

1. In **LEF-backed** mode: does it check `getSigType() == SIGNAL && getIoType() == OUTPUT`?
   If it checks all iterms regardless of type, a sequential instance with only its clock
   net connected would pass — which is wrong.

2. In **synthetic** mode: does it check only the Q-slot output? Or all output pins
   including any accidentally-connected non-data pins?

If either check is wrong, fix it so "signal output" means exactly "a pin that participates
in data-path connectivity per the D/Q convention above."

---

## Section 3 — Audit of Stage D repair pass

The repair pass in `formNetsAcyclic` finds dangling instances and gives each one a
receiver. Verify:

1. When the repair pass selects a **sink** to add for a dangling instance's output (Q),
   it only picks from the pool of **D-eligible input pins** — not from clock inputs or
   reset inputs.

2. When the repair pass "steals a spare sink" from an already multi-sink net, the stolen
   sink is a D-eligible input pin on the receiving instance, not a clock or control pin.

3. The repair pass itself never creates a new connection to a clk/rst/scan pin on any
   instance — neither as driver nor as sink.

If any of these conditions are violated, fix the pin-selection filter in the repair pass
to restrict to the SIGNAL/D-eligible pool.

---

## Section 4 — Audit of Stage B (normal net formation)

Stage B samples fanout and selects driver and sink pins for each net. Verify:

1. The **driver** candidate pool for sequential instances contains only Q-type outputs
   (SIGNAL/OUTPUT in LEF mode; Q-slot in synthetic mode).

2. The **sink** candidate pool contains only D-type inputs (SIGNAL/INPUT in LEF mode;
   D-slot in synthetic mode).

3. The pool is constructed (or filtered) before random sampling begins — not filtered
   after the fact.

If Stage B is currently sampling from all pins and then filtering, refactor so the
eligible pool is clean before sampling.

---

## Section 5 — New `validateNetlist` check: no control pins on signal nets

Add a fourth check to `validateNetlist` (runs after the existing three):

```
for each net N in the block:
    for each dbITerm T connected to N:
        if T->getMTerm()->getSigType() != SIGNAL:      // LEF mode
            FAIL: "net {N} is connected to non-signal pin {T} on instance {inst}"
```

For synthetic mode, an equivalent check: if a sequential instance's non-D/Q slot is
connected to any net, fail.

This check is independent of the dangling-instance check and must pass before the
statistics summary is printed.

---

## Section 6 — `num_nets` cap policy decision

The summary notes that if `spec.num_nets` is set low enough to truncate generation,
some instances are intentionally left dangling and the hard gate fires — the summary
author treated this as a "deliberate truncation you asked for."

**This is incorrect per the stated intent**: the invariant "no dangling instances" must
hold on the final netlist regardless of how generation was parameterised. A config that
produces a dangling instance should be rejected as invalid, not silently accepted as
"intentional truncation."

Fix: if the repair pass exhausts all repair options for an instance (which should only
happen when `num_nets` is artificially too low), **fail fast with a clear error**:

```
ERROR: instance {inst} cannot be given a connected output — net count cap too low.
       Raise num_nets or remove the cap. Generation aborted.
```

Do not write any output files when this error occurs. Do not print statistics.

---

## Files to modify

Based on your source read, identify the minimal set. Expected:
- `netlistgen.cpp` (Stage B pin-pool filtering, Stage D repair pass pin-pool filtering)
- `netlist_validation.cpp` (`instanceHasConnectedOutput` signal-type filter, new
  control-pin check)
- Possibly `netlistgen_spec.cpp` or equivalent (num_nets + dangling → hard error)
- `README.md` — document the D/Q-only convention explicitly, synthetic and LEF modes
- `FLOW.md` — update validateNetlist diagram to include the new control-pin check

---

## Tests

Add to the existing test suite:

1. **D/Q-only in LEF mode**: generate a small LEF-backed netlist; iterate all connected
   iterms and assert none have `getSigType() != SIGNAL`.

2. **D/Q-only in synthetic mode**: generate a small synthetic netlist; for every
   sequential instance, assert only D-slot and Q-slot are connected.

3. **Repair pass obeys pin constraint**: construct a pathological config that forces
   the repair pass to run (e.g. a very high `sequential_ratio` with a tight
   `fanout_range`); after generation, run check 1 or 2 above — verify the repair pass
   did not connect any control pin.

4. **`instanceHasConnectedOutput` signal-type gating**: directly call the check with a
   synthetic block where a sequential instance has only its clock output connected (not
   its Q); assert the check returns failure.

5. **Control-pin-on-net hard gate**: construct a block manually with a CLOCK-typed iterm
   connected to a signal net; call `validateNetlist` and assert non-OK Status with a
   message naming the offending pin.

6. **`num_nets` cap → hard error**: configure `num_nets` to a value provably too low to
   connect all instances; assert generation returns non-OK Status and writes no output
   files.

7. **Regression**: all existing Stage B, C, D, E1 tests must remain green.

All tests must pass under `ctest` before committing.

---

## Deliverables checklist

- [ ] Source read complete, including `netlist-dangling-instance-fix-summary.md`
- [ ] Confirm or fix Stage B pin-pool filtering (SIGNAL/D for sinks, SIGNAL/Q for drivers)
- [ ] Confirm or fix Stage D repair pass — only D-eligible sinks used
- [ ] Fix `instanceHasConnectedOutput` to check signal-typed output pins only
- [ ] Add control-pin-on-net check to `validateNetlist`
- [ ] Fix `num_nets` cap truncation to hard-error instead of silent dangling
- [ ] `README.md` updated with explicit D/Q-only convention for synthetic and LEF modes
- [ ] `FLOW.md` updated with new validateNetlist check
- [ ] All new tests green
- [ ] All existing tests green (no regression)
- [ ] Committed with message `netlistgen: enforce D/Q-only sequential pin constraint and audit well-formedness`

## Hard gate

Do not proceed to Stage E2 (Verilog writer) until all tests are green and this commit
is clean. The Verilog writer assumes a well-formed netlist — any structural violation
here will produce invalid Verilog.
