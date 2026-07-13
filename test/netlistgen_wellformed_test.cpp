// Well-formedness audit tests (spike-netlistgen-wellformed-audit.md): the
// D/Q-only sequential pin constraint and the hardened validateNetlist gate.
//
//   - isDataPin unit coverage: synthetic representative pins (i0 CLOCK
//     ineligible, i1/o0 eligible) and a hand-built Nangate45-style FF whose
//     pins are all USE SIGNAL (D/SI/Q eligible; CK/RN/SE/QN excluded by
//     name, sequential masters only).
//   - D/Q-only end to end, LEF and synthetic mode, including Stage E1's
//     PI/PO wiring: every connected iterm in a generated design is a data
//     pin; a sequential instance's control pins are never on any net.
//   - The Stage D repair pass obeys the same constraint under a
//     pathological config that forces it to run.
//   - validateNetlist's dangling-instance check counts DATA outputs only (a
//     connected QN does not save an instance whose Q dangles) and its
//     control-pin check rejects a CLOCK-typed iterm connected to a net.
//   - A num_nets cap too low to connect every instance is a hard generation
//     error (generateSynthetic returns -1; the CLI exits nonzero and writes
//     no output files), never silent truncation.
//
// Needs EDA_LAB_DATA_DIR (Nangate45 LEF fixtures).

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/cli_config.h"
#include "engines/netlistgen/netlist_validation.h"
#include "engines/netlistgen/netlistgen.h"

#include "odb/db.h"

namespace eda {
namespace {

std::string dataDir()
{
  return std::string(EDA_LAB_DATA_DIR);
}
std::string techLef()
{
  return dataDir() + "/nangate45/Nangate45_tech.lef";
}
std::string stdcellLef()
{
  return dataDir() + "/nangate45/Nangate45_stdcell.lef";
}
std::string tmpPath(const std::string& name)
{
  return std::string(::testing::TempDir()) + name;
}

// Hand-build a Nangate45-style scan FF master on `nb`'s (synthetic) library:
// every pin USE SIGNAL, so only isDataPin's name rule separates data from
// control — exactly the case the audit exists for. Pins: D/SI (data inputs),
// CK/RN/SE (control inputs), Q (data output), QN (excluded output).
odb::dbMaster* makeSignalTaggedFf(NetlistBuilder& nb, const std::string& name)
{
  // Force lazy synthetic-tech creation, leaving nothing behind (a stray
  // net would itself fail validateNetlist as dangling).
  odb::dbNet::destroy(nb.makeNet("tech_trigger_" + name));
  odb::dbLib* lib = *nb.db()->getLibs().begin();
  odb::dbMaster* ff = odb::dbMaster::create(lib, name.c_str());
  if (ff == nullptr) {
    return nullptr;
  }
  ff->setType(odb::dbMasterType::CORE);
  for (const char* pin : {"D", "SI", "CK", "RN", "SE"}) {
    odb::dbMTerm::create(ff, pin, odb::dbIoType::INPUT,
                         odb::dbSigType::SIGNAL);
  }
  for (const char* pin : {"Q", "QN"}) {
    odb::dbMTerm::create(ff, pin, odb::dbIoType::OUTPUT,
                         odb::dbSigType::SIGNAL);
  }
  ff->setFrozen();
  return ff;
}

// The D/Q-only convention, checked independently of isDataPin (so a bug in
// the predicate cannot make this vacuous): every connected iterm on a
// sequential instance must be one of the allowed data-pin names.
void expectSequentialConnectionsAreDQOnly(
    odb::dbBlock* block, const std::vector<std::string>& allowed)
{
  for (odb::dbInst* inst : block->getInsts()) {
    if (!isSequentialMaster(inst->getMaster())) {
      continue;
    }
    for (odb::dbITerm* iterm : inst->getITerms()) {
      if (iterm->getNet() == nullptr) {
        continue;
      }
      const std::string pin = iterm->getMTerm()->getName();
      EXPECT_NE(std::find(allowed.begin(), allowed.end(), pin), allowed.end())
          << "sequential instance " << inst->getName()
          << " has non-data pin '" << pin << "' connected to net "
          << iterm->getNet()->getName();
    }
  }
}

// Every connected iterm anywhere in the design is a data pin, and is typed
// SIGNAL. Complements the name-list check above with the predicate itself.
void expectAllConnectedItermsAreDataPins(odb::dbBlock* block)
{
  for (odb::dbNet* net : block->getNets()) {
    for (odb::dbITerm* iterm : net->getITerms()) {
      EXPECT_EQ(iterm->getSigType(), odb::dbSigType::SIGNAL)
          << net->getName() << " / " << iterm->getMTerm()->getName();
      EXPECT_TRUE(isDataPin(iterm->getMTerm()))
          << net->getName() << " / " << iterm->getMTerm()->getName();
    }
  }
}

// ---------------------------------------------------------------------------
// isDataPin unit coverage.
// ---------------------------------------------------------------------------

TEST(IsDataPinTest, SyntheticRepresentatives)
{
  NetlistBuilder nb("dp_syn");
  odb::dbMaster* seq = nb.makeMaster("SEQ", 2, 1, /*clocked=*/true);
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  ASSERT_NE(seq, nullptr);
  ASSERT_NE(inv, nullptr);

  // Sequential: i0 is the CLOCK pin (sig-type gate), i1 = D, o0 = Q.
  EXPECT_FALSE(isDataPin(seq->findMTerm("i0")));
  EXPECT_TRUE(isDataPin(seq->findMTerm("i1")));
  EXPECT_TRUE(isDataPin(seq->findMTerm("o0")));

  // Combinational: everything is data.
  EXPECT_TRUE(isDataPin(inv->findMTerm("i0")));
  EXPECT_TRUE(isDataPin(inv->findMTerm("o0")));
}

TEST(IsDataPinTest, SignalTaggedFfExcludesControlByName)
{
  // Every pin on this FF is USE SIGNAL (Nangate45 style), so only the name
  // rule can tell control from data — and it applies because the CK pin
  // name makes the master sequential.
  NetlistBuilder nb("dp_lef");
  odb::dbMaster* ff = makeSignalTaggedFf(nb, "SDFFR_LIKE");
  ASSERT_NE(ff, nullptr);
  ASSERT_TRUE(isSequentialMaster(ff));

  EXPECT_TRUE(isDataPin(ff->findMTerm("D")));
  EXPECT_TRUE(isDataPin(ff->findMTerm("SI")));  // scan-in IS a data path
  EXPECT_TRUE(isDataPin(ff->findMTerm("Q")));
  EXPECT_FALSE(isDataPin(ff->findMTerm("CK")));
  EXPECT_FALSE(isDataPin(ff->findMTerm("RN")));
  EXPECT_FALSE(isDataPin(ff->findMTerm("SE")));
  EXPECT_FALSE(isDataPin(ff->findMTerm("QN")));
}

TEST(IsDataPinTest, NameRuleAppliesToSequentialMastersOnly)
{
  // A combinational master reusing an excluded letter (a full adder's sum
  // output S) must be unaffected by the name rule.
  NetlistBuilder nb("dp_comb");
  odb::dbNet::destroy(nb.makeNet("tech_trigger"));
  odb::dbLib* lib = *nb.db()->getLibs().begin();
  odb::dbMaster* fa = odb::dbMaster::create(lib, "FA_LIKE");
  ASSERT_NE(fa, nullptr);
  fa->setType(odb::dbMasterType::CORE);
  for (const char* pin : {"A", "B", "CI"}) {
    odb::dbMTerm::create(fa, pin, odb::dbIoType::INPUT,
                         odb::dbSigType::SIGNAL);
  }
  for (const char* pin : {"S", "CO"}) {
    odb::dbMTerm::create(fa, pin, odb::dbIoType::OUTPUT,
                         odb::dbSigType::SIGNAL);
  }
  fa->setFrozen();
  ASSERT_FALSE(isSequentialMaster(fa));
  EXPECT_TRUE(isDataPin(fa->findMTerm("S")));
  EXPECT_TRUE(isDataPin(fa->findMTerm("CO")));
}

// ---------------------------------------------------------------------------
// D/Q-only end to end: LEF-backed and synthetic generation (Stage E1
// engaged in both, so its PI/PO pool filtering is covered too).
// ---------------------------------------------------------------------------

TEST(DqOnlyTest, LefBackedGenerationConnectsDataPinsOnly)
{
  NetlistBuilder nb("dq_lef");
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {stdcellLef()};
  spec.num_insts = 300;
  spec.sequential_ratio = 0.25;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 7;
  // Engage Stage E1 with a boundary-cell-heavy pin-type mix so its buffered/
  // registered PI/PO wiring runs under the same constraint.
  spec.rent_k = 2.5;
  spec.rent_p = 0.6;
  spec.io_pin_type_distribution = IoPinTypeDistribution{0.4, 0.3, 0.3};
  ASSERT_GT(generateSynthetic(nb, spec), 0);

  expectAllConnectedItermsAreDataPins(nb.block());
  // Nangate45 flip-flops: the only legitimate data-net endpoints are D, SI
  // (a muxed data path) and Q.
  expectSequentialConnectionsAreDQOnly(nb.block(), {"D", "SI", "Q"});
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

TEST(DqOnlyTest, SyntheticGenerationConnectsDataPinsOnly)
{
  NetlistBuilder nb("dq_syn");
  SyntheticNetlistSpec spec;
  spec.num_insts = 500;
  spec.sequential_ratio = 0.3;
  spec.target_avg_fanout = 3.0;
  spec.seed = 11;
  spec.rent_k = 2.5;
  spec.rent_p = 0.6;
  spec.io_pin_type_distribution = IoPinTypeDistribution{0.4, 0.3, 0.3};
  ASSERT_GT(generateSynthetic(nb, spec), 0);

  expectAllConnectedItermsAreDataPins(nb.block());
  // Synthetic sequential representative: D = i1, Q = o0; the CLOCK pin i0
  // must never be connected.
  expectSequentialConnectionsAreDQOnly(nb.block(), {"i1", "o0"});
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    if (isSequentialMaster(inst->getMaster())) {
      odb::dbITerm* ck = inst->findITerm("i0");
      ASSERT_NE(ck, nullptr);
      EXPECT_EQ(ck->getNet(), nullptr)
          << inst->getName() << ": clock pin connected";
    }
  }
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

// The Stage D repair pass, forced to run hard: an all-sequential design with
// a tight fanout range exhausts the D-input supply early, so most drivers
// are skipped by the main draw and must be repaired (including via the
// steal-a-spare-sink fallback). The repair pass must still never touch a
// control pin.
TEST(DqOnlyTest, RepairPassObeysPinConstraint)
{
  NetlistBuilder nb("dq_repair");
  SyntheticNetlistSpec spec;
  spec.num_insts = 200;
  spec.sequential_ratio = 1.0;  // every instance sequential: 1 D per 1 Q
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 4;  // each net eats 4 D pins; most drivers need repair
  spec.max_fanout = 4;
  spec.seed = 13;
  ASSERT_GT(generateSynthetic(nb, spec), 0);

  expectAllConnectedItermsAreDataPins(nb.block());
  expectSequentialConnectionsAreDQOnly(nb.block(), {"i1", "o0"});
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

// ---------------------------------------------------------------------------
// validateNetlist hardening.
// ---------------------------------------------------------------------------

// Dangling-instance signal-type gating: a sequential instance whose only
// connected output is its excluded QN (its data output Q dangles) is dead
// logic and must fail validation — before the audit, any connected signal
// OUTPUT saved it.
TEST(ValidateNetlistAuditTest, ConnectedQnDoesNotSaveDanglingQ)
{
  NetlistBuilder nb("val_qn");
  odb::dbMaster* ff = makeSignalTaggedFf(nb, "FFQN");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(ff, "u0");
  odb::dbInst* u1 = nb.makeInst(inv, "u1");
  // u0.QN -> u1.i0 (net a), u1.o0 -> u0.D (net b): both nets pass the
  // per-net driver/sink checks; only the instance-level data-output rule
  // can catch that u0's Q drives nothing.
  odb::dbNet* a = nb.makeNet("a");
  odb::dbNet* b = nb.makeNet("b");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "QN", a));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "i0", a));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", b));
  ASSERT_TRUE(NetlistBuilder::connect(u0, "D", b));

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.message.find("u0"), std::string::npos) << v.message;
  EXPECT_NE(v.message.find("no connected output"), std::string::npos)
      << v.message;

  // Rewiring the same topology through Q instead of QN makes it valid.
  u0->findITerm("QN")->disconnect();
  ASSERT_TRUE(NetlistBuilder::connect(u0, "Q", a));
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

// Control-pin-on-net hard gate: a CLOCK-typed iterm connected to an
// otherwise valid net fails validation with a message naming the pin and
// its instance.
TEST(ValidateNetlistAuditTest, ClockPinOnNetFails)
{
  NetlistBuilder nb("val_ck");
  odb::dbMaster* seq = nb.makeMaster("SEQ", 2, 1, /*clocked=*/true);
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(seq, "u0");
  odb::dbInst* u1 = nb.makeInst(inv, "u1");
  // Fully valid data wiring first: u0.o0 -> u1.i0, u1.o0 -> u0.i1 (a
  // register feedback loop, legal), so every instance has a connected data
  // output and every net has one driver and >= 1 sink.
  odb::dbNet* a = nb.makeNet("a");
  odb::dbNet* b = nb.makeNet("b");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", a));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "i0", a));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", b));
  ASSERT_TRUE(NetlistBuilder::connect(u0, "i1", b));
  ASSERT_TRUE(validateNetlist(nb.block()).ok);

  // Now violate the convention: u0's CLOCK pin i0 joins net b as one more
  // sink. Net-level tallies still pass (one driver, two sinks) — only the
  // control-pin check can reject it.
  ASSERT_TRUE(NetlistBuilder::connect(u0, "i0", b));
  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.message.find("non-data pin"), std::string::npos) << v.message;
  EXPECT_NE(v.message.find("i0"), std::string::npos) << v.message;
  EXPECT_NE(v.message.find("u0"), std::string::npos) << v.message;
}

// ---------------------------------------------------------------------------
// num_nets cap policy (Section 6 of the audit): a cap that leaves any
// instance unrepairable is a hard generation error, not silent truncation.
// ---------------------------------------------------------------------------

TEST(NetCapTest, CapTooLowIsAHardGenerationError)
{
  NetlistBuilder nb("cap_low");
  SyntheticNetlistSpec spec;
  spec.num_insts = 200;
  spec.sequential_ratio = 0.3;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.num_nets = 5;  // provably too low: 200 instances need >= ~200 nets
  spec.seed = 3;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(NetCapTest, GenerousCapStillSucceeds)
{
  NetlistBuilder nb("cap_ok");
  SyntheticNetlistSpec spec;
  spec.num_insts = 200;
  spec.sequential_ratio = 0.3;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.num_nets = 100000;  // cap present but never binding
  spec.seed = 3;
  ASSERT_GT(generateSynthetic(nb, spec), 0);
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

TEST(NetCapTest, CliWritesNoOutputOnCapError)
{
  const std::string def = tmpPath("cap_error.def");
  const std::string cfg_path = tmpPath("cap_error.json");
  std::remove(def.c_str());
  {
    std::ofstream cfg(cfg_path);
    ASSERT_TRUE(cfg.good());
    cfg << "{\n"
        << "  \"seed\": 3,\n"
        << "  \"instance_count\": 200,\n"
        << "  \"net_count\": 5,\n"
        << "  \"sequential_ratio\": 0.3,\n"
        << "  \"combinational_pin_distribution\": "
           "{\"2\":20,\"3\":20,\"4\":20,\"5\":20,\"6+\":20},\n"
        << "  \"output_def_path\": \"" << def << "\"\n"
        << "}\n";
  }
  std::ostringstream err;
  EXPECT_NE(runCliFromFile(cfg_path, err), 0);
  EXPECT_NE(err.str().find("generation failed"), std::string::npos)
      << err.str();
  EXPECT_FALSE(std::ifstream(def).good())
      << "CLI wrote an output file despite the generation error";
}

}  // namespace
}  // namespace eda
