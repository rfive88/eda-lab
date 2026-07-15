// eda-lab: structural_metrics_cli — thin main() over structural_metrics_core.
//
// Loads a design (LEF+DEF, or a native .odb), builds an eda::Hypergraph, runs
// sm::run_congestion_analysis, and prints the report. All ODB loading and
// argument parsing live here; the core library does none. Follows the repo's
// three-layer error-handling convention (see CLAUDE.md): file-existence
// prechecks (layer 1), a try/catch at every OpenROAD reader / ODB stream call
// (layer 2), and a top-level catch in main() (layer 3).

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "engines/structural_metrics/structural_metrics.h"
#include "hypergraph/hypergraph.h"
#include "odb/db.h"
#include "odb/defin.h"
#include "odb/lefin.h"
#include "support/cli.h"
#include "support/logging.h"
#include "support/status.h"
#include "utl/Logger.h"

namespace {

constexpr const char* kGroup = "structural_metrics";
// CLI phase-marker / diagnostic ids (utl::UKN; core library owns 350-374, the
// CLI 375-399 — see src/support/logging.h).
constexpr int kMsgLoadLef = 375;
constexpr int kMsgLoadDef = 376;
constexpr int kMsgLoadOdb = 377;
constexpr int kMsgBuild = 378;
constexpr int kMsgAnalyze = 379;
constexpr int kMsgFail = 380;

constexpr int kDefaultHfThreshold = 20;

// Load tech + cells from one-or-more LEF files then a DEF, returning the block
// via *out_block. lef_paths[0] supplies the technology (createTechAndLib); each
// remaining LEF adds cells against that tech (createLib) — the same reader
// pattern as hello_odb.cpp's loadDesign, generalised to N cell libraries so it
// works with this repo's split Nangate45 tech/stdcell files. The OpenROAD
// reader calls are wrapped in a boundary try/catch: a malformed (but present)
// LEF/DEF makes lefin/defin call logger->error(), which THROWS at the pinned
// SHA — caught here, close to the throw, and converted to a Status.
eda::Status loadLefDef(odb::dbDatabase* db, utl::Logger& logger,
                       const std::vector<std::string>& lef_paths,
                       const std::string& def_path,
                       odb::dbBlock** out_block)
{
  try {
    odb::lefin lef_reader(db, &logger, false);
    logger.info(utl::UKN, kMsgLoadLef, "Loading tech LEF: {}", lef_paths[0]);
    odb::dbLib* tech_lib =
        lef_reader.createTechAndLib("tech", lef_paths[0].c_str(),
                                    lef_paths[0].c_str());
    if (tech_lib == nullptr) {
      return eda::makeError(eda::ErrorCode::LefLoadFailed,
                            "failed to load tech LEF: " + lef_paths[0]);
    }
    odb::dbTech* tech = tech_lib->getTech();

    std::vector<odb::dbLib*> search_libs = {tech_lib};
    for (std::size_t i = 1; i < lef_paths.size(); ++i) {
      logger.info(utl::UKN, kMsgLoadLef, "Loading cell LEF: {}", lef_paths[i]);
      odb::dbLib* cell_lib = lef_reader.createLib(
          tech, ("cells" + std::to_string(i)).c_str(), lef_paths[i].c_str());
      if (cell_lib == nullptr) {
        return eda::makeError(eda::ErrorCode::LefLoadFailed,
                              "failed to load cell LEF: " + lef_paths[i]);
      }
      search_libs.push_back(cell_lib);
    }

    logger.info(utl::UKN, kMsgLoadDef, "Reading DEF: {}", def_path);
    odb::dbChip* chip = odb::dbChip::create(db, tech, "chip");
    odb::defin def_reader(db, &logger);
    def_reader.readChip(search_libs, def_path.c_str(), chip, false);

    // readChip destroys `chip` on failure, so re-fetch from the database rather
    // than touching the (possibly dangling) local pointer.
    odb::dbChip* loaded = db->getChip();
    odb::dbBlock* block = loaded != nullptr ? loaded->getBlock() : nullptr;
    if (block == nullptr) {
      return eda::makeError(eda::ErrorCode::DefLoadFailed,
                            "failed to read DEF: " + def_path);
    }
    *out_block = block;
    return eda::okStatus();
  } catch (const std::exception& e) {
    return eda::makeError(eda::ErrorCode::ParseError,
                          std::string("LEF/DEF load failed: ") + e.what());
  }
}

// Load a native .odb file, returning the block via *out_block. db->read throws
// ZIOError on a malformed/truncated stream; the boundary catch converts it.
eda::Status loadOdb(odb::dbDatabase* db, utl::Logger& logger,
                    const std::string& odb_path, odb::dbBlock** out_block)
{
  logger.info(utl::UKN, kMsgLoadOdb, "Reading .odb: {}", odb_path);
  std::ifstream odb_stream(odb_path, std::ios::binary);
  if (!odb_stream.is_open()) {
    return eda::makeError(eda::ErrorCode::FileNotFound,
                          "cannot open .odb file: " + odb_path);
  }
  try {
    db->read(odb_stream);
  } catch (const std::exception& e) {
    return eda::makeError(eda::ErrorCode::ParseError,
                          std::string("ODB read failed: ") + e.what());
  }
  odb::dbChip* chip = db->getChip();
  odb::dbBlock* block = chip != nullptr ? chip->getBlock() : nullptr;
  if (block == nullptr) {
    return eda::makeError(eda::ErrorCode::DefLoadFailed,
                          "no chip/block in .odb file: " + odb_path);
  }
  *out_block = block;
  return eda::okStatus();
}

// The CLI's real work; main() is a thin try/catch wrapper. Returns an exit code.
int runStructuralMetricsCli(int argc, char** argv)
{
  const eda::CliSpec spec{
      argv[0],
      "Structural congestion metrics (hg_metrics C1-C5) over a LEF+DEF or .odb "
      "design.",
      {{"--lef", "<path>", false,
        "Technology LEF (first) then cell LEF(s); repeatable. Required with "
        "--def."},
       {"--def", "<path>", false, "DEF design file. Requires --lef."},
       {"--odb", "<path>", false,
        "Native .odb design (mutually exclusive with --lef/--def)."},
       {"--hf-threshold", "<n>", false,
        "High-fanout net pin-count threshold (C1). Default 20."},
       eda::verbosityOption()}};

  if (eda::wantsHelp(argc, argv)) {
    eda::printHelp(std::cout, spec);
    return 0;
  }

  std::vector<std::string> lef_paths;
  std::string def_path;
  std::string odb_path;
  int hf_threshold = kDefaultHfThreshold;
  int verbosity = eda::kVerbosityDefault;

  // Small helper for "flag needs a value".
  auto needValue = [&](const std::string& flag, int& i) -> const char* {
    if (i + 1 >= argc) {
      return nullptr;
    }
    return argv[++i];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--lef") {
      const char* v = needValue(arg, i);
      if (v == nullptr) {
        eda::printUsageError(std::cerr, spec, "missing value for --lef");
        return 1;
      }
      lef_paths.emplace_back(v);
    } else if (arg == "--def") {
      const char* v = needValue(arg, i);
      if (v == nullptr) {
        eda::printUsageError(std::cerr, spec, "missing value for --def");
        return 1;
      }
      def_path = v;
    } else if (arg == "--odb") {
      const char* v = needValue(arg, i);
      if (v == nullptr) {
        eda::printUsageError(std::cerr, spec, "missing value for --odb");
        return 1;
      }
      odb_path = v;
    } else if (arg == "--hf-threshold") {
      const char* v = needValue(arg, i);
      if (v == nullptr) {
        eda::printUsageError(std::cerr, spec, "missing value for --hf-threshold");
        return 1;
      }
      hf_threshold = std::atoi(v);
    } else if (arg == "-verbosity" || arg == "--verbosity") {
      const char* v = needValue(arg, i);
      if (v != nullptr) {
        verbosity = std::atoi(v);
      }
    } else if (arg.rfind("--verbosity=", 0) == 0) {
      verbosity = std::atoi(arg.c_str() + std::strlen("--verbosity="));
    } else {
      eda::printUsageError(std::cerr, spec,
                           "unrecognized argument: " + arg);
      return 1;
    }
  }

  // --- Validate input-mode selection (mutually exclusive) ---
  const bool odb_mode = !odb_path.empty();
  const bool lefdef_mode = !lef_paths.empty() || !def_path.empty();
  if (odb_mode && lefdef_mode) {
    eda::printUsageError(std::cerr, spec,
                         "--odb is mutually exclusive with --lef/--def");
    return 1;
  }
  if (!odb_mode && !lefdef_mode) {
    eda::printUsageError(std::cerr, spec,
                         "no input given: supply --lef + --def, or --odb");
    return 1;
  }
  if (lefdef_mode) {
    if (lef_paths.empty()) {
      eda::printUsageError(std::cerr, spec, "missing required argument --lef");
      return 1;
    }
    if (def_path.empty()) {
      eda::printUsageError(std::cerr, spec, "missing required argument --def");
      return 1;
    }
  }

  utl::Logger logger;
  utl::Logger* lg = &logger;
  eda::applyVerbosity(lg, kGroup, verbosity);

  // --- Layer 1: precheck every input path exists before handing to readers ---
  std::vector<std::string> inputs;
  if (odb_mode) {
    inputs.push_back(odb_path);
  } else {
    inputs = lef_paths;
    inputs.push_back(def_path);
  }
  for (const std::string& path : inputs) {
    if (!std::filesystem::exists(path)) {
      logger.warn(utl::UKN, kMsgFail, "input file not found: {}", path);
      return 1;
    }
  }

  odb::dbDatabase* db = odb::dbDatabase::create();
  odb::dbBlock* block = nullptr;
  eda::Status status = odb_mode
                           ? loadOdb(db, logger, odb_path, &block)
                           : loadLefDef(db, logger, lef_paths, def_path, &block);
  if (!status.ok()) {
    logger.warn(utl::UKN, kMsgFail, "{}", status.message);
    return 1;
  }

  logger.info(utl::UKN, kMsgBuild, "Building hypergraph (instances: {}, nets: {})",
              block->getInsts().size(), block->getNets().size());
  eda::Hypergraph hg(&logger);
  hg.buildFromBlock(block);

  logger.info(utl::UKN, kMsgAnalyze,
              "Running congestion analysis (hf-threshold {})", hf_threshold);
  sm::CongestionAnalysisResult result;
  status = sm::run_congestion_analysis(hg, result, &logger, hf_threshold);
  if (!status.ok()) {
    logger.warn(utl::UKN, kMsgFail, "congestion analysis failed: {}",
                status.message);
    return 1;
  }

  sm::print_congestion_report(result, &logger);
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  // Layer 3 backstop for ordinary catchable exceptions. An odb error() throw is
  // contained at the reader boundary in loadLefDef/loadOdb, not here.
  try {
    return runStructuralMetricsCli(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Fatal error: unknown exception." << std::endl;
    return 1;
  }
}
