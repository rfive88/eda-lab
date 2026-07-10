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
    // Stage E: also tally primary-input dbBTerms as drivers and
    // primary-output dbBTerms as sinks here before judging.

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
  return result;
}

}  // namespace eda
