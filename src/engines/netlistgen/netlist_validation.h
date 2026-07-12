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
//
// Power/ground terminals (dbSigType POWER/GROUND) are ignored on both
// iterms and bterms — neither drivers nor sinks for this signal-
// connectivity check. Classification is IoType-based (the Stage A
// refactor), never name-based.
//
// Stage E1 folds dbBTerms into the SAME tally as dbITerms (tallyITerms /
// tallyBTerms are two small, symmetric helpers feeding one NetTally) rather
// than adding a parallel rule — a net with a bTerm is judged by exactly the
// same "exactly one driver, >=1 sink" invariant as any other net. This is
// why Stage E1's primary-input realization REPLACES a selected net's
// existing internal driver rather than adding the PI bTerm alongside it:
// once bTerms count toward the driver tally, "additional driver" would
// always fail this check (two drivers, real-world designs never have that).

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

// Walk every dbNet in `block` and confirm the well-formedness invariants
// above. Returns on the first violation with a message identifying the net.
// A null block or a block with no nets is considered valid (nothing to check).
NetlistValidation validateNetlist(odb::dbBlock* block);

}  // namespace eda
