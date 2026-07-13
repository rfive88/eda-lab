// Stage D tests for the netlistgen engine: combinational-loop freedom of the
// statistical-mix net formation (acyclic by construction — instance creation
// order doubles as a topological order), the sequential_ratio > 0
// bootstrap-source validation, the documented bootstrap/tail behavior when
// the eligible receiver pool runs thin (loosened receiver counts / skipped
// drivers, never a sinkless net), the guaranteed-instance-connectivity
// repair pass that follows (every instance ends up with >= 1 connected
// output, even one the main statistical pass skipped), and the end-to-end
// CLI check that written DEF output round-trips loop-free. Needs
// EDA_LAB_DATA_DIR (Nangate45 LEF fixtures) and NETLISTGEN_CLI_BIN (the
// built CLI binary path).

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/netlistgen.h"
#include "hypergraph/hypergraph.h"

#include "odb/db.h"
#include "odb/defin.h"
#include "odb/lefin.h"
#include "utl/Logger.h"

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

// The brief's cycle-detection model: combinational instances are nodes, edges
// follow driver -> receiver connectivity through each net, and sequential
// instances are cut at the D/Q boundary — implemented by simply not giving
// sequential instances a node, so a path through a register never closes a
// cycle. Iterative DFS with recursion-stack colouring (0 = white, 1 = on
// stack, 2 = done); a back edge to a grey node is a combinational cycle.
bool hasCombinationalCycle(odb::dbBlock* block)
{
  std::unordered_map<odb::dbInst*, int> node_id;
  int num_nodes = 0;
  for (odb::dbInst* inst : block->getInsts()) {
    if (!isSequentialMaster(inst->getMaster())) {
      node_id.emplace(inst, num_nodes++);
    }
  }

  std::vector<std::vector<int>> adj(num_nodes);
  for (odb::dbNet* net : block->getNets()) {
    int driver = -1;
    std::vector<int> receivers;
    for (odb::dbITerm* iterm : net->getITerms()) {
      const odb::dbSigType st = iterm->getSigType();
      if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
        continue;
      }
      const auto it = node_id.find(iterm->getInst());
      if (it == node_id.end()) {
        continue;  // sequential instance: cut at the D/Q boundary
      }
      if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
        driver = it->second;
      } else {
        receivers.push_back(it->second);
      }
    }
    if (driver >= 0) {
      for (const int r : receivers) {
        adj[driver].push_back(r);
      }
    }
  }

  std::vector<char> color(num_nodes, 0);
  std::vector<std::pair<int, size_t>> stack;  // (node, next edge index)
  for (int s = 0; s < num_nodes; ++s) {
    if (color[s] != 0) {
      continue;
    }
    color[s] = 1;
    stack.emplace_back(s, 0);
    while (!stack.empty()) {
      const int u = stack.back().first;
      const size_t next = stack.back().second;
      if (next < adj[u].size()) {
        ++stack.back().second;
        const int v = adj[u][next];
        if (color[v] == 1) {
          return true;  // back edge onto the recursion stack
        }
        if (color[v] == 0) {
          color[v] = 1;
          stack.emplace_back(v, 0);
        }
      } else {
        color[u] = 2;
        stack.pop_back();
      }
    }
  }
  return false;
}

// Every net has >= 1 sink and at most max_fanout sinks (driver excluded).
// Loosened tail nets may go below min_fanout, but never to zero.
void expectSaneFanouts(odb::dbBlock* block, int max_fanout)
{
  for (odb::dbNet* net : block->getNets()) {
    int sinks = 0;
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (iterm->getIoType() != odb::dbIoType::OUTPUT) {
        ++sinks;
      }
    }
    EXPECT_GE(sinks, 1) << net->getName();
    EXPECT_LE(sinks, max_fanout) << net->getName();
  }
}

// The guaranteed-instance-connectivity invariant (README.md's "Guaranteed
// instance connectivity" section): every instance has at least one
// connected signal OUTPUT iterm. formNetsAcyclic's thin-tail skip can leave
// a driver with no net at all after the main statistical pass; its repair
// pass exists specifically to close that gap before returning, so this
// should hold for every generated design, not just typical ones.
void expectNoDanglingInstances(odb::dbBlock* block)
{
  for (odb::dbInst* inst : block->getInsts()) {
    bool has_signal_output = false;
    bool connected = false;
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
        connected = true;
        break;
      }
    }
    EXPECT_TRUE(!has_signal_output || connected)
        << "instance " << inst->getName() << " is dangling";
  }
}

// ---------------------------------------------------------------------------
// Detector sanity: a hand-built two-inverter loop must be reported as cyclic
// (guards the loop-freedom tests below against a vacuous pass).
// ---------------------------------------------------------------------------

TEST(CycleDetectorTest, FlagsHandBuiltCombinationalLoop)
{
  NetlistBuilder nb("cyc");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbInst* u1 = nb.makeInst(inv, "u1");
  odb::dbNet* a = nb.makeNet("a");
  odb::dbNet* b = nb.makeNet("b");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", a));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "i0", a));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", b));
  ASSERT_TRUE(NetlistBuilder::connect(u0, "i0", b));
  EXPECT_TRUE(hasCombinationalCycle(nb.block()));
}

TEST(CycleDetectorTest, SequentialFeedbackIsNotACycle)
{
  // u0 (comb) -> u1 (DFF-like, clocked) -> back to u0: a legitimate
  // sequential feedback loop, cut at the register.
  NetlistBuilder nb("seqfb");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbMaster* dff = nb.makeMaster("DFF", 2, 1, /*clocked=*/true);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbInst* u1 = nb.makeInst(dff, "u1");
  odb::dbNet* d = nb.makeNet("d");
  odb::dbNet* q = nb.makeNet("q");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", d));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "i1", d));  // D pin (i0 is CK)
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", q));
  ASSERT_TRUE(NetlistBuilder::connect(u0, "i0", q));
  EXPECT_FALSE(hasCombinationalCycle(nb.block()));
}

// ---------------------------------------------------------------------------
// The key Stage D correctness property: generated netlists are loop-free,
// still well-formed per net, and still consumable by Hypergraph.
// ---------------------------------------------------------------------------

void expectLoopFreeSynthetic(int num_insts, uint32_t seed)
{
  NetlistBuilder nb("loopfree");
  SyntheticNetlistSpec spec;
  spec.num_insts = num_insts;
  spec.sequential_ratio = 0.15;
  spec.combinational_pin_distribution =
      std::array<double, 5>{30, 25, 20, 15, 10};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = seed;
  const int nets = generateSynthetic(nb, spec);
  ASSERT_GT(nets, 0) << "insts " << num_insts << " seed " << seed;

  EXPECT_FALSE(hasCombinationalCycle(nb.block()))
      << "insts " << num_insts << " seed " << seed;
  expectSaneFanouts(nb.block(), spec.max_fanout);
  expectNoDanglingInstances(nb.block());

  // Hypergraph::buildFromBlock consumes the acyclic block unmodified.
  Hypergraph hg;
  hg.buildFromBlock(nb.block());
  EXPECT_EQ(hg.numVertices(), num_insts);
  EXPECT_EQ(hg.numHyperedges(), nets);
}

TEST(LoopFreedomTest, SyntheticSmallMediumLargeMultiSeed)
{
  for (const int num_insts : {300, 5000, 50000}) {
    for (const uint32_t seed : {1u, 7u, 42u}) {
      expectLoopFreeSynthetic(num_insts, seed);
    }
  }
}

TEST(LoopFreedomTest, LefBackedMultiSeed)
{
  for (const uint32_t seed : {3u, 9u}) {
    NetlistBuilder nb("looplef");
    SyntheticNetlistSpec spec;
    spec.tech_lef_path = techLef();
    spec.cell_lef_paths = {stdcellLef()};
    spec.num_insts = 500;
    spec.sequential_ratio = 0.15;
    spec.combinational_pin_distribution =
        std::array<double, 5>{20, 20, 20, 20, 20};
    spec.min_fanout = 2;
    spec.max_fanout = 5;
    spec.seed = seed;
    const int nets = generateSynthetic(nb, spec);
    ASSERT_GT(nets, 0) << "seed " << seed;
    EXPECT_FALSE(hasCombinationalCycle(nb.block())) << "seed " << seed;
    expectSaneFanouts(nb.block(), spec.max_fanout);
    expectNoDanglingInstances(nb.block());
  }
}

// The construction invariant behind loop freedom, checked directly: a net
// driven by combinational u<i> only reaches combinational u<j> with j > i
// (instance names encode creation order). Stronger than cycle absence.
TEST(LoopFreedomTest, CombEdgesFollowCreationOrder)
{
  NetlistBuilder nb("ordered");
  SyntheticNetlistSpec spec;
  spec.num_insts = 3000;
  spec.sequential_ratio = 0.2;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.seed = 5;
  ASSERT_GT(generateSynthetic(nb, spec), 0);

  auto creationIndex = [](odb::dbInst* inst) {
    return std::atoi(inst->getName().c_str() + 1);  // names are u<N>
  };
  for (odb::dbNet* net : nb.block()->getNets()) {
    odb::dbITerm* driver = nullptr;
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
        driver = iterm;
      }
    }
    ASSERT_NE(driver, nullptr) << net->getName();
    if (isSequentialMaster(driver->getInst()->getMaster())) {
      continue;  // Q outputs are unconstrained drivers
    }
    const int di = creationIndex(driver->getInst());
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (iterm == driver
          || isSequentialMaster(iterm->getInst()->getMaster())) {
        continue;
      }
      EXPECT_GT(creationIndex(iterm->getInst()), di)
          << net->getName() << ": comb driver u" << di
          << " feeds non-later comb input of "
          << iterm->getInst()->getName();
    }
  }
}

// ---------------------------------------------------------------------------
// Bootstrap-source validation: sequential_ratio must be > 0 in statistical
// mode (Q outputs are the only signal source until Stage E's primary inputs).
// ---------------------------------------------------------------------------

TEST(BootstrapValidationTest, ZeroSequentialRatioFailsFast)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 100;
  spec.sequential_ratio = 0.0;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(BootstrapValidationTest, UnsetSequentialRatioFailsFast)
{
  // Statistical mix engaged via the mode field only: an unset ratio counts
  // as 0 and must fail the same way.
  SyntheticNetlistSpec spec;
  spec.num_insts = 100;
  spec.target_avg_fanout = 3.0;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(BootstrapValidationTest, JustAboveZeroPasses)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 100;
  spec.sequential_ratio = 0.001;
  spec.target_avg_fanout = 3.0;
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));
}

TEST(BootstrapValidationTest, LegacyModeUnaffected)
{
  // The legacy weighted mix (Stage A path) has no sequential_ratio and is
  // exempt from the bootstrap requirement (and the acyclicity guarantee).
  NetlistBuilder nb("legacy");
  SyntheticNetlistSpec spec;
  spec.masters = {{"INV", 1, 1, 1.0}, {"NAND2", 2, 1, 1.0}};
  spec.num_insts = 64;
  EXPECT_GT(generateSynthetic(nb, spec), 0);
}

// ---------------------------------------------------------------------------
// Bootstrap edge case: sequential_ratio just above 0 with a small instance
// count and a demanding min_fanout. The documented deterministic behavior is
// LOOSENING: when the eligible receiver pool cannot cover min_fanout the net
// is formed with fewer receivers (>= 1), and a driver with zero eligible
// receivers forms no net at all — generation still succeeds, output stays
// well-formed (per-net) and loop-free, with no stall and no failure.
// ---------------------------------------------------------------------------

TEST(BootstrapEdgeCaseTest, ThinPoolLoosensReceiverCountDeterministically)
{
  NetlistBuilder nb("thin");
  SyntheticNetlistSpec spec;
  spec.num_insts = 40;
  spec.sequential_ratio = 0.05;  // just above 0
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 4;  // demand outstrips the input-pin supply
  spec.max_fanout = 4;
  spec.seed = 13;
  const int nets = generateSynthetic(nb, spec);
  ASSERT_GT(nets, 0);

  // Total signal output pins = upper bound on formable nets; sink demand at
  // min_fanout 4 outstrips the available input pins, so the tail of the
  // creation order MUST have been loosened (some net below min_fanout) or
  // skipped (fewer nets than drivers) — and never a sinkless net.
  int output_pins = 0;
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    for (odb::dbITerm* iterm : inst->getITerms()) {
      const odb::dbSigType st = iterm->getSigType();
      if (st != odb::dbSigType::POWER && st != odb::dbSigType::GROUND
          && iterm->getIoType() == odb::dbIoType::OUTPUT) {
        ++output_pins;
      }
    }
  }
  int below_min = 0;
  for (odb::dbNet* net : nb.block()->getNets()) {
    int sinks = 0;
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (iterm->getIoType() != odb::dbIoType::OUTPUT) {
        ++sinks;
      }
    }
    ASSERT_GE(sinks, 1) << net->getName() << ": sinkless net";
    EXPECT_LE(sinks, spec.max_fanout) << net->getName();
    if (sinks < spec.min_fanout) {
      ++below_min;
    }
  }
  EXPECT_TRUE(below_min > 0 || nets < output_pins)
      << "expected loosened nets or skipped drivers under a thin pool "
      << "(nets " << nets << ", output pins " << output_pins << ")";
  EXPECT_FALSE(hasCombinationalCycle(nb.block()));

  // Deterministic: the identical spec reproduces the identical outcome.
  NetlistBuilder nb2("thin2");
  EXPECT_EQ(generateSynthetic(nb2, spec), nets);
  int below_min2 = 0;
  for (odb::dbNet* net : nb2.block()->getNets()) {
    int sinks = 0;
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (iterm->getIoType() != odb::dbIoType::OUTPUT) {
        ++sinks;
      }
    }
    if (sinks < spec.min_fanout) {
      ++below_min2;
    }
  }
  EXPECT_EQ(below_min2, below_min);
}

// ---------------------------------------------------------------------------
// CLI smoke re-run (the brief's before/after check): spawn netlistgen_cli,
// read the DEF back through defin, and confirm the written output is
// loop-free — Stage C's writers now emit genuinely valid netlists.
// ---------------------------------------------------------------------------

TEST(CliLoopFreedomTest, WrittenDefRoundTripsLoopFree)
{
  const std::string def = tmpPath("cli_staged.def");
  const std::string cfg_path = tmpPath("cli_staged.json");
  std::remove(def.c_str());

  {
    std::ofstream cfg(cfg_path);
    ASSERT_TRUE(cfg.good());
    cfg << "{\n"
        << "  \"seed\": 7,\n"
        << "  \"instance_count\": 300,\n"
        << "  \"fanout_range\": { \"min\": 2, \"max\": 5 },\n"
        << "  \"tech_lef_path\": \"" << techLef() << "\",\n"
        << "  \"cell_lef_paths\": [\"" << stdcellLef() << "\"],\n"
        << "  \"sequential_ratio\": 0.1,\n"
        << "  \"combinational_pin_distribution\": "
           "{\"2\":20,\"3\":20,\"4\":20,\"5\":20,\"6+\":20},\n"
        << "  \"output_def_path\": \"" << def << "\"\n"
        << "}\n";
  }

  const std::string cmd = std::string(NETLISTGEN_CLI_BIN) + " " + cfg_path;
  ASSERT_EQ(std::system(cmd.c_str()), 0) << "CLI exited nonzero";
  ASSERT_TRUE(std::ifstream(def).good());

  // Read the DEF back (Nangate45 tech + cells, mirroring the stage C smoke
  // test) and run cycle detection on the round-tripped block.
  utl::Logger logger;
  odb::dbDatabase* db = odb::dbDatabase::create();
  odb::lefin lef(db, &logger, /*ignore_non_routing_layers=*/false);
  odb::dbLib* tech_lib =
      lef.createTechAndLib("tech", "tech_lib", techLef().c_str());
  ASSERT_NE(tech_lib, nullptr);
  odb::dbLib* cell_lib =
      lef.createLib(tech_lib->getTech(), "nangate", stdcellLef().c_str());
  ASSERT_NE(cell_lib, nullptr);

  odb::dbChip* chip = odb::dbChip::create(db, tech_lib->getTech(), "chip");
  odb::defin def_reader(db, &logger);
  std::vector<odb::dbLib*> libs = {tech_lib, cell_lib};
  def_reader.readChip(libs, def.c_str(), chip, false);

  odb::dbBlock* block = chip->getBlock();
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(static_cast<int>(block->getInsts().size()), 300);
  EXPECT_GT(static_cast<int>(block->getNets().size()), 0);
  EXPECT_FALSE(hasCombinationalCycle(block));
  odb::dbDatabase::destroy(db);
}

}  // namespace
}  // namespace eda
