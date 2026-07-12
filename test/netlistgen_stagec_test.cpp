// Stage C tests for the netlistgen engine: net well-formedness validation, the
// DEF / .odb writers, JSON config parsing, and the standalone CLI executable
// (including its validate-before-write fail-fast). Needs EDA_LAB_DATA_DIR
// (Nangate45 LEF fixtures) and NETLISTGEN_CLI_BIN (the built CLI binary path).
//
// Stage D landed loop-free net formation, which made sequential_ratio > 0
// mandatory in statistical mode — every generating spec here sets it.
// Loop-freedom itself is asserted in test/netlistgen_staged_test.cpp.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/cli_config.h"
#include "engines/netlistgen/netlist_validation.h"
#include "engines/netlistgen/netlist_writers.h"
#include "engines/netlistgen/netlistgen.h"

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

// A LEF-backed spec matching the CLI smoke-test config (all five combinational
// buckets populated by Nangate45; sequential_ratio must be > 0 since Stage D —
// deeper sequential-classification checks live in the stage B tests).
SyntheticNetlistSpec smokeSpec()
{
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {stdcellLef()};
  spec.num_insts = 300;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 7;
  return spec;
}

// ---------------------------------------------------------------------------
// Net well-formedness validation
// ---------------------------------------------------------------------------

TEST(ValidationTest, PassesOnSyntheticGeneration)
{
  NetlistBuilder nb("synthval");
  SyntheticNetlistSpec spec;
  spec.num_insts = 2000;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution = std::array<double, 5>{30, 25, 20, 15, 10};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 11;
  ASSERT_GT(generateSynthetic(nb, spec), 0);

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_TRUE(v.ok) << v.message;
}

TEST(ValidationTest, PassesOnLefGeneration)
{
  NetlistBuilder nb("lefval");
  ASSERT_GT(generateSynthetic(nb, smokeSpec()), 0);

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_TRUE(v.ok) << v.message;
}

TEST(ValidationTest, FlagsDanglingNet)
{
  NetlistBuilder nb("dangle");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  nb.makeInst(inv, "u0");
  nb.makeNet("dead");  // created, never connected

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.message.find("dangling"), std::string::npos) << v.message;
}

TEST(ValidationTest, FlagsDriverlessNet)
{
  NetlistBuilder nb("nodriver");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbNet* net = nb.makeNet("sinkonly");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "i0", net));  // sink, no driver

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.message.find("0 drivers"), std::string::npos) << v.message;
}

TEST(ValidationTest, FlagsMultiDriverNet)
{
  NetlistBuilder nb("twodriver");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbInst* u1 = nb.makeInst(inv, "u1");
  odb::dbInst* u2 = nb.makeInst(inv, "u2");
  odb::dbNet* net = nb.makeNet("contended");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", net));  // driver 1
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", net));  // driver 2
  ASSERT_TRUE(NetlistBuilder::connect(u2, "i0", net));  // a sink exists

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.message.find("2 drivers"), std::string::npos) << v.message;
}

TEST(ValidationTest, FlagsSinklessNet)
{
  NetlistBuilder nb("nosink");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbNet* net = nb.makeNet("driveronly");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", net));  // driver, no sinks

  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.message.find("no sinks"), std::string::npos) << v.message;
}

// ---------------------------------------------------------------------------
// DEF / .odb writers
// ---------------------------------------------------------------------------

TEST(WriterTest, WritesDefAndOdbFiles)
{
  NetlistBuilder nb("writers");
  SyntheticNetlistSpec spec;
  spec.num_insts = 100;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  ASSERT_GT(generateSynthetic(nb, spec), 0);
  nb.estimateDieArea(spec.num_insts);

  const std::string def = tmpPath("writers.def");
  const std::string odb = tmpPath("writers.odb");
  std::remove(def.c_str());
  std::remove(odb.c_str());

  EXPECT_TRUE(writeDef(nb.block(), def, nb.logger()));
  EXPECT_TRUE(writeOdb(nb.db(), odb));

  EXPECT_TRUE(std::ifstream(def).good());
  EXPECT_TRUE(std::ifstream(odb, std::ios::binary).good());
}

// ---------------------------------------------------------------------------
// JSON config parsing
// ---------------------------------------------------------------------------

TEST(ConfigParseTest, ModeAValid)
{
  const std::string cfg = R"({
    "seed": 42,
    "instance_count": 5000,
    "net_count": null,
    "fanout_range": { "min": 2, "max": 6 },
    "tech_lef_path": "data/nangate45/Nangate45_tech.lef",
    "cell_lef_paths": ["data/nangate45/Nangate45_stdcell.lef"],
    "sequential_ratio": 0.15,
    "combinational_pin_distribution": {"2":20,"3":30,"4":20,"5":20,"6+":10},
    "distribution_tolerance_pct": 2.0,
    "output_def_path": "run/generated.def",
    "output_odb_path": "run/generated.odb"
  })";
  CliConfig c;
  std::string err;
  ASSERT_TRUE(parseCliConfig(cfg, c, err)) << err;

  EXPECT_EQ(c.spec.seed, 42u);
  EXPECT_EQ(c.spec.num_insts, 5000);
  EXPECT_EQ(c.spec.num_nets, -1);  // null -> "as many as pools allow"
  EXPECT_EQ(c.spec.min_fanout, 2);
  EXPECT_EQ(c.spec.max_fanout, 6);
  ASSERT_TRUE(c.spec.tech_lef_path.has_value());
  ASSERT_EQ(c.spec.cell_lef_paths.size(), 1u);
  ASSERT_TRUE(c.spec.sequential_ratio.has_value());
  EXPECT_DOUBLE_EQ(*c.spec.sequential_ratio, 0.15);
  ASSERT_TRUE(c.spec.combinational_pin_distribution.has_value());
  const auto& d = *c.spec.combinational_pin_distribution;
  EXPECT_DOUBLE_EQ(d[0], 20);
  EXPECT_DOUBLE_EQ(d[1], 30);
  EXPECT_DOUBLE_EQ(d[4], 10);
  EXPECT_FALSE(c.spec.target_avg_fanout.has_value());
  ASSERT_TRUE(c.output_def_path.has_value());
  ASSERT_TRUE(c.output_odb_path.has_value());
}

TEST(ConfigParseTest, ModeBValid)
{
  const std::string cfg = R"({
    "seed": 42,
    "instance_count": 5000,
    "fanout_range": { "min": 2, "max": 6 },
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.4,
    "output_def_path": "run/generated.def"
  })";
  CliConfig c;
  std::string err;
  ASSERT_TRUE(parseCliConfig(cfg, c, err)) << err;

  ASSERT_TRUE(c.spec.target_avg_fanout.has_value());
  EXPECT_DOUBLE_EQ(*c.spec.target_avg_fanout, 3.4);
  EXPECT_FALSE(c.spec.combinational_pin_distribution.has_value());
  ASSERT_TRUE(c.output_def_path.has_value());
  EXPECT_FALSE(c.output_odb_path.has_value());
}

TEST(ConfigParseTest, MissingRequiredInstanceCount)
{
  const std::string cfg = R"({
    "output_def_path": "run/x.def"
  })";
  CliConfig c;
  std::string err;
  EXPECT_FALSE(parseCliConfig(cfg, c, err));
  EXPECT_NE(err.find("instance_count"), std::string::npos) << err;
}

TEST(ConfigParseTest, NoOutputPathFailsFast)
{
  const std::string cfg = R"({
    "instance_count": 100,
    "target_avg_fanout": 3.0
  })";
  CliConfig c;
  std::string err;
  EXPECT_FALSE(parseCliConfig(cfg, c, err));
  EXPECT_NE(err.find("output"), std::string::npos) << err;
}

TEST(ConfigParseTest, MalformedJsonFails)
{
  CliConfig c;
  std::string err;
  EXPECT_FALSE(parseCliConfig("{ not json ", c, err));
}

// ---------------------------------------------------------------------------
// CLI validate-before-write fail-fast (no process spawn needed)
// ---------------------------------------------------------------------------

TEST(CliGatingTest, RefusesToWriteOnMalformedNetlist)
{
  NetlistBuilder nb("gating");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbInst* u1 = nb.makeInst(inv, "u1");
  odb::dbNet* net = nb.makeNet("bad");
  // Two drivers, no sink -> malformed.
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", net));
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", net));

  CliConfig config;
  config.spec.num_insts = 2;
  config.output_def_path = tmpPath("gating.def");
  config.output_odb_path = tmpPath("gating.odb");
  std::remove(config.output_def_path->c_str());
  std::remove(config.output_odb_path->c_str());

  std::ostringstream err;
  EXPECT_FALSE(validateAndWrite(nb, config, err));
  EXPECT_NE(err.str().find("validation failed"), std::string::npos) << err.str();

  // Nothing was written.
  EXPECT_FALSE(std::ifstream(*config.output_def_path).good());
  EXPECT_FALSE(std::ifstream(*config.output_odb_path).good());
}

// ---------------------------------------------------------------------------
// CLI smoke: spawn the executable, read the DEF back, confirm counts.
// ---------------------------------------------------------------------------

// Reads a DEF back through defin (Nangate45 tech + stdcell libs) and returns
// {instances, nets}. Mirrors hello_odb's read path.
std::pair<int, int> readDefCounts(const std::string& def_path)
{
  utl::Logger logger;
  odb::dbDatabase* db = odb::dbDatabase::create();
  odb::lefin lef(db, &logger, /*ignore_non_routing_layers=*/false);
  odb::dbLib* tech_lib = lef.createTechAndLib("tech", "tech_lib", techLef().c_str());
  odb::dbTech* tech = tech_lib->getTech();
  odb::dbLib* cell_lib = lef.createLib(tech, "nangate", stdcellLef().c_str());

  odb::dbChip* chip = odb::dbChip::create(db, tech, "chip");
  odb::defin def_reader(db, &logger);
  std::vector<odb::dbLib*> libs = {tech_lib, cell_lib};
  def_reader.readChip(libs, def_path.c_str(), chip, false);

  odb::dbBlock* block = chip->getBlock();
  const int insts = static_cast<int>(block->getInsts().size());
  const int nets = static_cast<int>(block->getNets().size());
  odb::dbDatabase::destroy(db);
  return {insts, nets};
}

TEST(CliSmokeTest, GeneratesWritesAndRoundTrips)
{
  // Expected counts from the identical in-memory spec (determinism ties the
  // spawned CLI run to this one for a fixed seed).
  int expected_insts = 0, expected_nets = 0;
  {
    NetlistBuilder nb("expected");
    const int nets = generateSynthetic(nb, smokeSpec());
    ASSERT_GT(nets, 0);
    expected_insts = static_cast<int>(nb.block()->getInsts().size());
    expected_nets = nets;
  }

  const std::string def = tmpPath("cli_smoke.def");
  const std::string odb = tmpPath("cli_smoke.odb");
  const std::string cfg_path = tmpPath("cli_smoke.json");
  std::remove(def.c_str());
  std::remove(odb.c_str());

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
        << "  \"output_def_path\": \"" << def << "\",\n"
        << "  \"output_odb_path\": \"" << odb << "\"\n"
        << "}\n";
  }

  const std::string cmd = std::string(NETLISTGEN_CLI_BIN) + " " + cfg_path;
  const int rc = std::system(cmd.c_str());
  ASSERT_EQ(rc, 0) << "CLI exited nonzero";

  EXPECT_TRUE(std::ifstream(def).good());
  EXPECT_TRUE(std::ifstream(odb, std::ios::binary).good());

  const auto [insts, nets] = readDefCounts(def);
  EXPECT_EQ(insts, expected_insts);
  EXPECT_EQ(insts, 300);  // matches instance_count in the config
  EXPECT_EQ(nets, expected_nets);
}

}  // namespace
}  // namespace eda
