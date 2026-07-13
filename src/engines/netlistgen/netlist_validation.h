// eda-lab: structural net well-formedness validation for netlistgen
// (Stage C; primary-port awareness added in Stage E1).
//
// A defensive correctness check, independent of and in addition to Stage D's
// combinational-loop-freedom guarantee. It confirms a more basic invariant on
// every net so a generation bug is caught before file output is manually
// inspected (or before Stage D's more complex logic could mask it):
//
//   - Exactly one driver: exactly one connected terminal that supplies a
//     signal to the net — a dbITerm with IoType::OUTPUT, or (since Stage E1)
//     a dbBTerm with IoType::INPUT (a primary input feeds the design from
//     outside, so it drives the net from the internal design's perspective).
//     Zero drivers or more than one is a failure.
//   - At least one sink: at least one connected terminal that consumes the
//     net's signal — a dbITerm with IoType::INPUT/INOUT, or a dbBTerm with
//     IoType::OUTPUT/INOUT (a primary output observes the net from outside).
//     Zero sinks is a failure (a driver with nothing to drive — dangling).
//   - No dangling nets: a net with zero connected terminals (iterms or
//     bterms) is a failure.
//   - No dangling instances: every instance's DATA output(s) must actually
//     drive something. "Data output" follows the D/Q-only sequential pin
//     convention (isDataPin in netlistgen.h): a sequential instance is
//     alive only through its Q — a connected clock output or QN does not
//     count. An instance whose data OUTPUT iterm(s) are all unconnected
//     (dbITerm::getNet() == nullptr) is dead logic and fails validation,
//     exactly as strictly as a multiply-driven or sinkless net. Checked
//     instance-by-instance (independent of the net-level tallies above,
//     which are net-centric and cannot see a driver pin that was never
//     connected to any net at all). An instance with no data output pin at
//     all (nothing for this check to apply to) trivially passes.
//   - No control pins on nets (the D/Q-only sequential pin constraint,
//     added by the well-formedness audit): every dbITerm connected to any
//     net must be a data pin per isDataPin — a connected clock, async
//     set/reset, scan-enable, or other non-SIGNAL/control pin fails
//     validation, naming the pin and its instance. Runs last, after the
//     dangling-instance check, so an instance alive only through a control
//     pin is reported as dangling (the more fundamental defect) first.
//
// Power/ground terminals (dbSigType POWER/GROUND) are ignored on both
// iterms and bterms — neither drivers nor sinks for this signal-
// connectivity check. Classification is IoType-based (the Stage A
// refactor); only the control-pin check is additionally name-aware, via
// isDataPin, because libraries like Nangate45 tag control pins USE SIGNAL.
//
// Stage E1 folds dbBTerms into the SAME tally as dbITerms (tallyITerms /
// tallyBTerms are two small, symmetric helpers feeding one NetTally) rather
// than adding a parallel rule — a net with a bTerm is judged by exactly the
// same "exactly one driver, >=1 sink" invariant as any other net. (A PI's
// net is always freshly built from never-connected or stolen sink pins —
// never an existing net's driver — so the single-driver rule holds; see
// netlistgen.h's Stage E1 notes.)

#pragma once

#include <string>

namespace odb {
class dbBlock;
}

namespace eda {

struct NetlistValidation
{
  bool ok = true;
  // Empty when ok; otherwise a human-readable message naming the first
  // offending net and what is wrong with it.
  std::string message;
};

// Walk every dbNet, then every dbInst, in `block` and confirm the
// well-formedness invariants above. Returns on the first violation with a
// message identifying the offending net or instance. A null block or a
// block with no nets/instances is considered valid (nothing to check).
NetlistValidation validateNetlist(odb::dbBlock* block);

}  // namespace eda
