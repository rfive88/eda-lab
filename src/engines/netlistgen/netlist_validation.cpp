// See netlist_validation.h.

#include "engines/netlistgen/netlist_validation.h"

#include "engines/netlistgen/netlistgen.h"  // isDataPin (D/Q-only convention)
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

// Instance-level well-formedness: every instance's DATA output(s) must
// actually drive something. "Data output" is exactly isDataPin's D/Q-only
// rule — a connected clock output or a connected QN does NOT save a
// sequential instance whose Q dangles, because only data pins participate
// in data-net connectivity. Returns true and leaves `message` untouched if
// `inst` has at least one connected data OUTPUT iterm, or has no data
// output at all (nothing this check applies to). Returns false with a
// message naming the instance if every data output is unconnected (a
// dangling/dead-logic instance).
bool instanceHasConnectedOutput(odb::dbInst* inst, std::string& message)
{
  bool has_data_output = false;
  for (odb::dbITerm* iterm : inst->getITerms()) {
    if (iterm->getIoType() != odb::dbIoType::OUTPUT
        || !isDataPin(iterm->getMTerm())) {
      continue;
    }
    has_data_output = true;
    if (iterm->getNet() != nullptr) {
      return true;
    }
  }
  if (!has_data_output) {
    return true;  // no data output pin at all; nothing to check
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

  // Final check (the D/Q-only sequential pin constraint): no control pin —
  // clock, async set/reset, scan-enable, or any other non-data pin per
  // isDataPin — may be connected to any net. Generation leaves these pins
  // unconnected by construction; a connected one is a generation bug (or a
  // hand-built block violating the convention). Runs after the dangling-
  // instance check so a sequential instance kept "alive" only through a
  // non-data pin is reported as dangling (the more fundamental defect)
  // rather than as a control-pin violation.
  for (odb::dbNet* net : block->getNets()) {
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (!isDataPin(iterm->getMTerm())) {
        result.ok = false;
        result.message = "net '" + std::string(net->getName())
                         + "' is connected to non-data pin '"
                         + iterm->getMTerm()->getName() + "' on instance '"
                         + std::string(iterm->getInst()->getName())
                         + "' (control/clock pins must stay unconnected)";
        return result;
      }
    }
  }
  return result;
}

}  // namespace eda
