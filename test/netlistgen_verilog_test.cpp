// Stage E2 tests for the netlistgen engine: the structural Verilog writer
// (writeVerilog), its JSON/CLI gating (LEF-mode requirement + top_module_name
// identifier validation), the name-consistency guarantee across the
// .v / .def / .odb triplet, and the sequential_ratio==0 bootstrap relaxation
// that Stage E1's primary-input ports enable. See
// docs/briefs/spike-netlistgen-E2-verilog.md.
//
// Needs EDA_LAB_DATA_DIR (Nangate45 LEF fixtures — Verilog output is LEF-backed
// mode only) and NETLISTGEN_CLI_BIN (the built CLI, for the all-three-outputs
// smoke test).

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/cli_config.h"
#include "engines/netlistgen/netlist_validation.h"
#include "engines/netlistgen/netlist_writers.h"
#include "engines/netlistgen/netlistgen.h"
#include "engines/netlistgen/verilog_out.h"

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

// A LEF-backed spec that also generates primary I/O ports (rent_k/rent_p set),
// matching the Stage C smoke spec. Default io_pin_type_distribution is
// all-combinational, so no boundary buffer/FF instances are created and the
// emitted instance count equals num_insts exactly (Stage E2 brief, test 3).
SyntheticNetlistSpec lefPortSpec()
{
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {stdcellLef()};
  spec.num_insts = 300;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 7;
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;
  return spec;
}

std::string slurp(const std::string& path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// --- Lightweight structural-Verilog parse (see writeVerilog's output shape) ---
struct ParsedVerilog
{
  std::string module_name;
  int module_count = 0;
  int endmodule_count = 0;
  std::set<std::string> inputs;
  std::set<std::string> outputs;
  std::set<std::string> wires;
  std::set<std::string> insts;    // instance names
  std::set<std::string> masters;  // cell master names used
  int inst_count = 0;             // counts duplicates (== emitted instances)
  int open_parens = 0;
  int close_parens = 0;
};

ParsedVerilog parseVerilog(const std::string& text)
{
  ParsedVerilog p;
  for (char c : text) {
    if (c == '(') {
      ++p.open_parens;
    } else if (c == ')') {
      ++p.close_parens;
    }
  }
  const std::regex module_re(R"(^\s*module\s+(\S+)\s*\()");
  const std::regex endmodule_re(R"(^\s*endmodule\s*$)");
  const std::regex input_re(R"(^\s*input\s+(\S+)\s*;)");
  const std::regex output_re(R"(^\s*output\s+(\S+)\s*;)");
  const std::regex wire_re(R"(^\s*wire\s+(\S+)\s*;)");
  // An instance instantiation: "    <MASTER> <inst> (" — two bare tokens
  // followed by an open paren. The module header also matches this shape, so
  // it is excluded explicitly.
  const std::regex inst_re(R"(^\s*(\S+)\s+(\S+)\s+\(\s*$)");

  std::istringstream ss(text);
  std::string line;
  std::smatch m;
  while (std::getline(ss, line)) {
    if (std::regex_search(line, m, module_re)) {
      ++p.module_count;
      p.module_name = m[1];
      continue;
    }
    if (std::regex_search(line, m, endmodule_re)) {
      ++p.endmodule_count;
      continue;
    }
    if (std::regex_search(line, m, input_re)) {
      p.inputs.insert(m[1]);
      continue;
    }
    if (std::regex_search(line, m, output_re)) {
      p.outputs.insert(m[1]);
      continue;
    }
    if (std::regex_search(line, m, wire_re)) {
      p.wires.insert(m[1]);
      continue;
    }
    if (std::regex_search(line, m, inst_re) && m[1] != "module") {
      p.masters.insert(m[1]);
      p.insts.insert(m[2]);
      ++p.inst_count;
      continue;
    }
  }
  return p;
}

// Instance / net names from a DEF, parsed by section (COMPONENTS lines are
// "- <inst> <master> ...", NETS lines are "- <net> ..."). Only lines whose
// first token is "-" open a new record; "+"/"(" continuation lines are
// ignored. SPECIALNETS is skipped (there are none here anyway).
struct ParsedDef
{
  std::set<std::string> insts;
  std::set<std::string> nets;
};

ParsedDef parseDef(const std::string& path)
{
  ParsedDef d;
  std::ifstream in(path);
  std::string line;
  enum class Mode { kNone, kComponents, kNets } mode = Mode::kNone;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string tok;
    if (!(ls >> tok)) {
      continue;
    }
    if (tok == "COMPONENTS") {
      mode = Mode::kComponents;
      continue;
    }
    if (tok == "NETS") {
      mode = Mode::kNets;
      continue;
    }
    if (tok == "END") {
      mode = Mode::kNone;
      continue;
    }
    if (tok != "-") {
      continue;
    }
    if (mode == Mode::kComponents) {
      std::string inst;
      if (ls >> inst) {
        d.insts.insert(inst);
      }
    } else if (mode == Mode::kNets) {
      std::string net;
      if (ls >> net) {
        d.nets.insert(net);
      }
    }
  }
  return d;
}

// ---------------------------------------------------------------------------
// 1. No Verilog params: no .v produced, prior behaviour unchanged.
// ---------------------------------------------------------------------------

TEST(VerilogGatingTest, NoVerilogPathWritesNoDotV)
{
  const std::string cfg = R"({
    "instance_count": 100,
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.0,
    "output_def_path": "run/x.def"
  })";
  CliConfig c;
  std::string err;
  ASSERT_TRUE(parseCliConfig(cfg, c, err)) << err;
  EXPECT_FALSE(c.output_verilog_path.has_value());
  EXPECT_EQ(c.top_module_name, "generated_top");  // default
}

// ---------------------------------------------------------------------------
// 2. LEF-mode gating: output_verilog_path without a tech LEF fails at
//    spec-build time (no files can be written — parse never returns config).
// ---------------------------------------------------------------------------

TEST(VerilogGatingTest, VerilogWithoutLefFailsFast)
{
  const std::string cfg = R"({
    "instance_count": 100,
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.0,
    "output_verilog_path": "run/generated.v"
  })";
  CliConfig c;
  std::string err;
  EXPECT_FALSE(parseCliConfig(cfg, c, err));
  EXPECT_NE(err.find("LEF"), std::string::npos) << err;
}

TEST(VerilogGatingTest, VerilogWithLefParses)
{
  const std::string cfg = R"({
    "instance_count": 100,
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.0,
    "tech_lef_path": "data/nangate45/Nangate45_tech.lef",
    "cell_lef_paths": ["data/nangate45/Nangate45_stdcell.lef"],
    "output_verilog_path": "run/generated.v"
  })";
  CliConfig c;
  std::string err;
  ASSERT_TRUE(parseCliConfig(cfg, c, err)) << err;
  ASSERT_TRUE(c.output_verilog_path.has_value());
}

// output_verilog_path alone (no DEF/ODB) satisfies the "at least one output"
// rule.
TEST(VerilogGatingTest, VerilogAloneIsAValidOutput)
{
  const std::string cfg = R"({
    "instance_count": 100,
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.0,
    "tech_lef_path": "data/nangate45/Nangate45_tech.lef",
    "cell_lef_paths": ["data/nangate45/Nangate45_stdcell.lef"],
    "output_verilog_path": "run/generated.v"
  })";
  CliConfig c;
  std::string err;
  EXPECT_TRUE(parseCliConfig(cfg, c, err)) << err;
}

// A malformed netlist blocks ALL three writers — including Verilog — before
// any file is produced (validateAndWrite runs validateNetlist first). This is
// the ordering guarantee the E2 brief requires be verified for the triplet.
TEST(VerilogGatingTest, MalformedNetlistWritesNoneOfTheThreeOutputs)
{
  NetlistBuilder nb("gate3");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbInst* u1 = nb.makeInst(inv, "u1");
  odb::dbNet* net = nb.makeNet("bad");
  ASSERT_TRUE(NetlistBuilder::connect(u0, "o0", net));  // two drivers,
  ASSERT_TRUE(NetlistBuilder::connect(u1, "o0", net));  // no sink -> malformed

  CliConfig config;
  config.spec.num_insts = 2;
  config.output_def_path = tmpPath("gate3.def");
  config.output_odb_path = tmpPath("gate3.odb");
  config.output_verilog_path = tmpPath("gate3.v");
  for (const auto& p : {config.output_def_path, config.output_odb_path,
                        config.output_verilog_path}) {
    std::remove(p->c_str());
  }

  std::ostringstream err;
  EXPECT_FALSE(validateAndWrite(nb, config, err));
  EXPECT_NE(err.str().find("validation failed"), std::string::npos) << err.str();
  EXPECT_FALSE(std::ifstream(*config.output_def_path).good());
  EXPECT_FALSE(std::ifstream(*config.output_odb_path).good());
  EXPECT_FALSE(std::ifstream(*config.output_verilog_path).good());
}

// ---------------------------------------------------------------------------
// 3. Basic Verilog output shape.
// ---------------------------------------------------------------------------

TEST(VerilogWriterTest, BasicOutputShape)
{
  NetlistBuilder nb("basicv");
  const SyntheticNetlistSpec spec = lefPortSpec();
  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  ASSERT_TRUE(stats.engaged);
  ASSERT_TRUE(validateNetlist(nb.block()).ok);

  const std::string v = tmpPath("basic.v");
  std::remove(v.c_str());
  ASSERT_TRUE(writeVerilog(nb.block(), v, "generated_top", nb.logger()));

  const std::string text = slurp(v);
  ASSERT_FALSE(text.empty());
  const ParsedVerilog p = parseVerilog(text);

  EXPECT_EQ(p.module_count, 1);
  EXPECT_EQ(p.endmodule_count, 1);
  EXPECT_EQ(p.module_name, "generated_top");
  EXPECT_EQ(p.open_parens, p.close_parens);  // balanced

  // All-combinational io_pin_type_distribution (default) => no boundary
  // buffer/FF instances, so the emitted instance count equals num_insts.
  EXPECT_EQ(stats.n_buffered, 0);
  EXPECT_EQ(stats.n_registered, 0);
  EXPECT_EQ(p.inst_count, spec.num_insts);
  EXPECT_EQ(static_cast<int>(nb.block()->getInsts().size()), spec.num_insts);

  // Every PI/PO name created by E1 appears as an input/output port.
  ASSERT_FALSE(stats.pi_nets.empty());
  ASSERT_FALSE(stats.po_nets.empty());
  for (odb::dbBTerm* bt : nb.block()->getBTerms()) {
    const std::string name = bt->getName();
    if (bt->getIoType() == odb::dbIoType::INPUT) {
      EXPECT_TRUE(p.inputs.count(name)) << "missing input port " << name;
    } else {
      EXPECT_TRUE(p.outputs.count(name)) << "missing output port " << name;
    }
  }

  // No stray content after endmodule.
  const size_t end_pos = text.rfind("endmodule");
  ASSERT_NE(end_pos, std::string::npos);
  EXPECT_EQ(text.find_first_not_of(" \t\r\n", end_pos + 9), std::string::npos);
}

// ---------------------------------------------------------------------------
// 4. Name consistency across .v / .def / .odb.
// ---------------------------------------------------------------------------

TEST(VerilogWriterTest, NameConsistencyAcrossOutputs)
{
  NetlistBuilder nb("consist");
  const SyntheticNetlistSpec spec = lefPortSpec();
  ASSERT_GT(generateSynthetic(nb, spec), 0);
  ASSERT_TRUE(validateNetlist(nb.block()).ok);

  const std::string v = tmpPath("consist.v");
  const std::string def = tmpPath("consist.def");
  const std::string odb = tmpPath("consist.odb");
  for (const std::string& f : {v, def, odb}) {
    std::remove(f.c_str());
  }

  nb.estimateDieArea(spec.num_insts);
  ASSERT_TRUE(writeVerilog(nb.block(), v, "generated_top", nb.logger()));
  ASSERT_TRUE(writeDef(nb.block(), def, nb.logger()));
  ASSERT_TRUE(writeOdb(nb.db(), odb));
  EXPECT_TRUE(std::ifstream(odb, std::ios::binary).good());

  const ParsedVerilog pv = parseVerilog(slurp(v));
  const ParsedDef pd = parseDef(def);

  // Instance names are identical across .v and .def (both walk getInsts()).
  EXPECT_EQ(pv.insts, pd.insts);
  EXPECT_EQ(static_cast<int>(pv.insts.size()),
            static_cast<int>(nb.block()->getInsts().size()));

  // Wires are the non-port nets; every wire name is a .def net name, and the
  // .def nets that are NOT wires are exactly the port nets (those carrying a
  // dbBTerm — declared as input/output, never re-declared as a wire).
  for (const std::string& w : pv.wires) {
    EXPECT_TRUE(pd.nets.count(w)) << "wire " << w << " missing from DEF nets";
  }
  int port_nets = 0;
  for (odb::dbNet* net : nb.block()->getNets()) {
    if (!net->getBTerms().empty()) {
      ++port_nets;
    }
  }
  EXPECT_EQ(static_cast<int>(pd.nets.size()) - static_cast<int>(pv.wires.size()),
            port_nets);
}

// ---------------------------------------------------------------------------
// 5. Every cell master emitted in .v is a real loaded LEF master.
// ---------------------------------------------------------------------------

TEST(VerilogWriterTest, MasterNamesAreRealLefCells)
{
  NetlistBuilder nb("masters");
  ASSERT_GT(generateSynthetic(nb, lefPortSpec()), 0);

  const std::string v = tmpPath("masters.v");
  std::remove(v.c_str());
  ASSERT_TRUE(writeVerilog(nb.block(), v, "generated_top", nb.logger()));

  // Ground truth: the master names actually on the block's instances.
  std::set<std::string> block_masters;
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    block_masters.insert(inst->getMaster()->getName());
  }
  const ParsedVerilog p = parseVerilog(slurp(v));
  ASSERT_FALSE(p.masters.empty());
  for (const std::string& m : p.masters) {
    EXPECT_TRUE(block_masters.count(m)) << "unknown master in .v: " << m;
  }
}

// ---------------------------------------------------------------------------
// 6. top_module_name: custom valid name is honored; malformed name is rejected.
// ---------------------------------------------------------------------------

TEST(VerilogWriterTest, CustomTopModuleName)
{
  NetlistBuilder nb("topname");
  ASSERT_GT(generateSynthetic(nb, lefPortSpec()), 0);

  const std::string v = tmpPath("topname.v");
  std::remove(v.c_str());
  ASSERT_TRUE(writeVerilog(nb.block(), v, "my_block", nb.logger()));

  EXPECT_EQ(parseVerilog(slurp(v)).module_name, "my_block");
}

TEST(VerilogGatingTest, InvalidTopModuleNameFails)
{
  const std::string cfg = R"({
    "instance_count": 100,
    "sequential_ratio": 0.15,
    "target_avg_fanout": 3.0,
    "tech_lef_path": "data/nangate45/Nangate45_tech.lef",
    "cell_lef_paths": ["data/nangate45/Nangate45_stdcell.lef"],
    "output_verilog_path": "run/generated.v",
    "top_module_name": "0bad"
  })";
  CliConfig c;
  std::string err;
  EXPECT_FALSE(parseCliConfig(cfg, c, err));
  EXPECT_NE(err.find("top_module_name"), std::string::npos) << err;
}

// ---------------------------------------------------------------------------
// 7. Bootstrap relaxation: sequential_ratio == 0 is valid iff E1 PI ports
//    (rent_k + rent_p) provide the alternate bootstrap source.
// ---------------------------------------------------------------------------

TEST(BootstrapRelaxationTest, ZeroSeqWithRentIsValidAndGenerates)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 500;
  spec.sequential_ratio = 0.0;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 3;
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;

  EXPECT_TRUE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb("zeroseq");
  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  EXPECT_TRUE(stats.engaged);
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

TEST(BootstrapRelaxationTest, ZeroSeqWithoutRentFails)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 500;
  spec.sequential_ratio = 0.0;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 3;
  // No rent_k/rent_p: no bootstrap source at all.

  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
  NetlistBuilder nb("zeroseq_norent");
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

// ---------------------------------------------------------------------------
// 8. CLI smoke: spawn the executable requesting all three outputs; confirm all
//    exist and the DEF round-trips to the configured instance/net counts.
// ---------------------------------------------------------------------------

std::pair<int, int> readDefCounts(const std::string& def_path)
{
  utl::Logger logger;
  odb::dbDatabase* db = odb::dbDatabase::create();
  odb::lefin lef(db, &logger, /*ignore_non_routing_layers=*/false);
  odb::dbLib* tech_lib =
      lef.createTechAndLib("tech", "tech_lib", techLef().c_str());
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

TEST(VerilogCliSmokeTest, AllThreeOutputsWrittenAndDefRoundTrips)
{
  // Expected in-memory counts for the identical spec/seed (determinism ties
  // the spawned run to this one). Default all-combinational pin type => no
  // boundary cells => instance count == instance_count.
  int expected_insts = 0, expected_nets = 0;
  {
    NetlistBuilder nb("expected");
    // generateSynthetic returns the count of Stage D nets only; Stage E1's PI
    // ports add fresh nets, so the block (and thus the DEF) has more. Compare
    // against the block's own net count.
    ASSERT_GT(generateSynthetic(nb, lefPortSpec()), 0);
    expected_insts = static_cast<int>(nb.block()->getInsts().size());
    expected_nets = static_cast<int>(nb.block()->getNets().size());
  }

  const std::string def = tmpPath("e2_smoke.def");
  const std::string odb = tmpPath("e2_smoke.odb");
  const std::string v = tmpPath("e2_smoke.v");
  const std::string cfg_path = tmpPath("e2_smoke.json");
  for (const std::string& f : {def, odb, v}) {
    std::remove(f.c_str());
  }

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
        << "  \"rent_k\": 2.5,\n"
        << "  \"rent_p\": 0.60,\n"
        << "  \"top_module_name\": \"generated_top\",\n"
        << "  \"output_def_path\": \"" << def << "\",\n"
        << "  \"output_odb_path\": \"" << odb << "\",\n"
        << "  \"output_verilog_path\": \"" << v << "\"\n"
        << "}\n";
  }

  const std::string cmd = std::string(NETLISTGEN_CLI_BIN) + " " + cfg_path;
  ASSERT_EQ(std::system(cmd.c_str()), 0) << "CLI exited nonzero";

  EXPECT_TRUE(std::ifstream(def).good());
  EXPECT_TRUE(std::ifstream(odb, std::ios::binary).good());
  EXPECT_TRUE(std::ifstream(v).good());

  const auto [insts, nets] = readDefCounts(def);
  EXPECT_EQ(insts, expected_insts);
  EXPECT_EQ(insts, 300);
  EXPECT_EQ(nets, expected_nets);

  // The spawned .v agrees with the DEF on instance and net names.
  const ParsedVerilog pv = parseVerilog(slurp(v));
  const ParsedDef pd = parseDef(def);
  EXPECT_EQ(pv.insts, pd.insts);
  EXPECT_EQ(pv.module_name, "generated_top");
}

}  // namespace
}  // namespace eda
