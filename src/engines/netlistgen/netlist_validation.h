// eda-lab: structural net well-formedness validation for netlistgen (Stage C).
//
// A defensive correctness check, independent of and in addition to Stage D's
// combinational-loop-freedom guarantee. It confirms a more basic invariant on
// every net so a generation bug is caught before file output is manually
// inspected (or before Stage D's more complex logic could mask it):
//
//   - Exactly one driver: exactly one connected dbITerm with IoType::OUTPUT.
//     Zero drivers or more than one is a failure.
//   - At least one sink: at least one connected dbITerm with IoType::INPUT
//     (INOUT is accepted as a sink too — it can be driven). Zero sinks is a
//     failure (a driver with nothing to drive — dangling).
//   - No dangling nets: a net with zero connected iterms is a failure.
//
// Power/ground iterms (dbSigType POWER/GROUND) are ignored — they are neither
// drivers nor sinks for this signal-connectivity check. Classification is
// IoType-based (the Stage A refactor), never name-based.
//
// This stage has no primary ports yet (Stage E), so only dbITerms are
// considered. The per-net tallying is factored so Stage E can extend it to
// also treat primary-input dbBTerms as valid drivers and primary-output
// dbBTerms as valid sinks, without a rewrite.

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
