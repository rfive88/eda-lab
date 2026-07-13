// See netlist_validation.h.

#include "engines/netlistgen/netlist_validation.h"

#include "odb/db.h"

namespace eda {

namespace {

// Per-net tally of signal drivers and sinks. Kept as a small struct so Stage E
// can fold primary-input/-output dbBTerm counts into the same totals before
// the verdict is computed, rather than duplicating the driver/sink rule.
struct NetTally
{
  int connected = 0;  // total connected terminals (iterms this stage)
  int drivers = 0;    // OUTPUT-direction signal terminals
  int sinks = 0;      // INPUT/INOUT-direction signal terminals
};

void tallyITerms(odb::dbNet* net, NetTally& tally)
{
  for (odb::dbITerm* iterm : net->getITerms()) {
    ++tally.connected;
    const odb::dbSigType st = iterm->getSigType();
    if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
      continue;
    }
    switch (iterm->getIoType().getValue()) {
      case odb::dbIoType::OUTPUT:
        ++tally.drivers;
        break;
      case odb::dbIoType::INPUT:
      case odb::dbIoType::INOUT:
        ++tally.sinks;
        break;
      case odb::dbIoType::FEEDTHRU:
        break;
    }
  }
}

// Stage E1: a primary-input bTerm supplies a signal into the design (a
// driver, from the internal net's perspective); a primary-output bTerm
// observes/consumes one (a sink). Symmetric with tallyITerms' IoType
// switch, just on the other terminal kind — folded in here (not a
// duplicated rule) so a net with a bTerm is judged by the exact same
// "exactly one driver, >=1 sink" rule as any other net.
void tallyBTerms(odb::dbNet* net, NetTally& tally)
{
  for (odb::dbBTerm* bterm : net->getBTerms()) {
    ++tally.connected;
    const odb::dbSigType st = bterm->getSigType();
    if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
      continue;
    }
    switch (bterm->getIoType().getValue()) {
      case odb::dbIoType::INPUT:
        ++tally.drivers;
        break;
      case odb::dbIoType::OUTPUT:
      case odb::dbIoType::INOUT:
        ++tally.sinks;
        break;
      case odb::dbIoType::FEEDTHRU:
        break;
    }
  }
}

// Instance-level well-formedness: every instance's output(s) must actually
// drive something (dbSigType::POWER/GROUND pins are ignored, same as the
// net-level tallies above). Returns true and leaves `message` untouched if
// `inst` has at least one connected signal OUTPUT iterm, or has no signal
// output at all (nothing this check applies to). Returns false with a
// message naming the instance if every signal output is unconnected (a
// dangling/dead-logic instance).
bool instanceHasConnectedOutput(odb::dbInst* inst, std::string& message)
{
  bool has_signal_output = false;
  for (odb::dbITerm* iterm : inst->getITerms()) {
    if (iterm->getIoType() != odb::dbIoType::OUTPUT) {
      continue;
    }
    const odb::dbSigType st = iterm->getSigType();
    if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
      continue;
    }
    has_signal_output = true;
    if (iterm->getNet() != nullptr) {
      return true;
    }
  }
  if (!has_signal_output) {
    return true;  // no signal output pin at all; nothing to check
  }
  message = "instance '" + std::string(inst->getName())
            + "' has no connected output (dangling instance)";
  return false;
}

}  // namespace

NetlistValidation validateNetlist(odb::dbBlock* block)
{
  NetlistValidation result;
  if (block == nullptr) {
    return result;
  }

  for (odb::dbNet* net : block->getNets()) {
    NetTally tally;
    tallyITerms(net, tally);
    tallyBTerms(net, tally);

    const std::string name = net->getName();
    if (tally.connected == 0) {
      result.ok = false;
      result.message = "net '" + name + "' is dangling (no connected terminals)";
      return result;
    }
    if (tally.drivers != 1) {
      result.ok = false;
      result.message = "net '" + name + "' has " + std::to_string(tally.drivers)
                       + " drivers (expected exactly 1)";
      return result;
    }
    if (tally.sinks < 1) {
      result.ok = false;
      result.message =
          "net '" + name + "' has no sinks (driver with nothing to drive)";
      return result;
    }
  }

  for (odb::dbInst* inst : block->getInsts()) {
    std::string message;
    if (!instanceHasConnectedOutput(inst, message)) {
      result.ok = false;
      result.message = message;
      return result;
    }
  }
  return result;
}

}  // namespace eda
