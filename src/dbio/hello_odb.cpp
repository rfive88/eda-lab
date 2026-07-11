#include "odb/db.h"
#include "odb/lefin.h"
#include "odb/defin.h"
#include "odb/defout.h"
#include "support/cli.h"
#include "support/logging.h"
#include "support/status.h"
#include "utl/Logger.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr const char* kGroup = "hello_odb";
// hello_odb info phase-marker ids (utl::UKN; see support/logging.h).
constexpr int kMsgTechLef = 200;
constexpr int kMsgCellLef = 201;
constexpr int kMsgDef = 202;
constexpr int kMsgCounts = 203;
constexpr int kMsgWriteOdb = 204;
constexpr int kMsgWriteDef = 205;
constexpr int kMsgDone = 206;
constexpr int kMsgReadFail = 207;

// Load tech LEF + cell LEF + DEF into `db`, returning the resulting block via
// `*out_block`. Every OpenROAD reader call is checked before its result is
// used: createTechAndLib/createLib can return nullptr (e.g. duplicate
// tech/lib), and readChip DESTROYS the chip we pass in on failure — so failure
// is detected via db->getChip(), never by dereferencing the local chip pointer
// (which would be a use-after-free). A genuinely malformed LEF makes OpenROAD's
// own lefin call logger->error(), which throws; that is caught by main()'s
// top-level handler, not here.
eda::Status loadDesign(odb::dbDatabase* db,
                       utl::Logger& logger,
                       const char* tech_lef,
                       const char* cell_lef,
                       const char* def,
                       odb::dbBlock** out_block) {
  // Check each input exists BEFORE handing it to lefin/defin: on a missing file
  // OpenROAD's readers call logger->error(), which THROWS (see
  // support/logging.h). Prechecking keeps the common "file not found" case an
  // ordinary Status failure at its source rather than relying on unwinding a
  // thrown exception out of OpenROAD.
  for (const char* path : {tech_lef, cell_lef, def}) {
    if (!std::filesystem::exists(path)) {
      return eda::makeError(eda::ErrorCode::FileNotFound,
                            std::string("input file not found: ") + path);
    }
  }

  // Wrap the OpenROAD reader calls in a try/catch AT this boundary. A malformed
  // (but present) LEF/DEF makes lefin/defin call logger->error(), which throws;
  // catching it here — close to the throw, where the stack unwinds cleanly —
  // converts it to a Status. (A catch only in main() does NOT work: unwinding
  // an odb throw all the way up to main fails and calls std::terminate; the
  // handler must sit near the reader call. This is why prechecks + a boundary
  // catch, not just main()'s backstop, are load-bearing here.)
  try {
    odb::lefin lef_reader(db, &logger, false);
    logger.info(utl::UKN, kMsgTechLef, "Loading tech LEF: {}", tech_lef);
    odb::dbLib* tech_lib =
        lef_reader.createTechAndLib("tech", tech_lef, tech_lef);
    if (tech_lib == nullptr) {
      return eda::makeError(
          eda::ErrorCode::LefLoadFailed,
          std::string("failed to load tech LEF: ") + tech_lef);
    }

    odb::dbTech* tech = tech_lib->getTech();
    logger.info(utl::UKN, kMsgCellLef, "Loading cell LEF: {}", cell_lef);
    odb::dbLib* cell_lib = lef_reader.createLib(tech, "nangate", cell_lef);
    if (cell_lib == nullptr) {
      return eda::makeError(
          eda::ErrorCode::LefLoadFailed,
          std::string("failed to load cell LEF: ") + cell_lef);
    }

    logger.info(utl::UKN, kMsgDef, "Reading DEF: {}", def);
    // readChip at the pinned OpenROAD SHA requires a pre-created chip.
    odb::dbChip* chip = odb::dbChip::create(db, tech, "chip");
    odb::defin def_reader(db, &logger);
    std::vector<odb::dbLib*> search_libs = {tech_lib, cell_lib};
    def_reader.readChip(search_libs, def, chip, false);

    // readChip destroys `chip` on failure, so re-fetch from the database rather
    // than touching the (possibly dangling) local pointer.
    odb::dbChip* loaded = db->getChip();
    odb::dbBlock* block = loaded != nullptr ? loaded->getBlock() : nullptr;
    if (block == nullptr) {
      return eda::makeError(eda::ErrorCode::DefLoadFailed,
                            std::string("failed to read DEF: ") + def);
    }
    *out_block = block;
    return eda::okStatus();
  } catch (const std::exception& e) {
    return eda::makeError(eda::ErrorCode::ParseError,
                          std::string("LEF/DEF load failed: ") + e.what());
  }
}

// Round-trip the loaded design back out to out.odb + out.def, checking every
// stream/write before trusting it.
eda::Status roundTripWrite(odb::dbDatabase* db,
                           odb::dbBlock* block,
                           utl::Logger& logger) {
  logger.info(utl::UKN, kMsgWriteOdb, "Writing .odb: out.odb");
  std::ofstream odb_file("out.odb");
  if (!odb_file.is_open()) {
    return eda::makeError(eda::ErrorCode::OutputWriteFailed,
                          "cannot open out.odb for writing");
  }
  db->write(odb_file);
  odb_file.close();

  logger.info(utl::UKN, kMsgWriteDef, "Writing DEF: out.def");
  odb::DefOut def_writer(&logger);
  if (!def_writer.writeBlock(block, "out.def")) {
    return eda::makeError(eda::ErrorCode::OutputWriteFailed,
                          "failed to write out.def");
  }
  return eda::okStatus();
}

// The CLI's real work: argument parsing plus the load/round-trip flow. Returns
// a process exit code. main() is a thin try/catch wrapper over this so an
// unanticipated throw (e.g. OpenROAD's logger->error()) still exits cleanly.
int runHelloOdb(int argc, char** argv) {
  // Positional: <tech_lef> <cell_lef> <def>. Optional: -verbosity <level>
  // (or --verbosity=<level>) — repo-wide debug verbosity, see support/logging.h.
  const eda::CliSpec spec{
      argv[0],
      "LEF/DEF round-trip smoke test against OpenDB.",
      {{"<tech_lef>", "", true, "Technology LEF file (loaded first)."},
       {"<cell_lef>", "", true, "Cell library LEF file (cells against the tech)."},
       {"<def>", "", true, "DEF file to read into a chip/block."},
       eda::verbosityOption()}};
  if (eda::wantsHelp(argc, argv)) {
    eda::printHelp(std::cout, spec);
    return 0;
  }

  std::vector<const char*> pos;
  int verbosity = eda::kVerbosityDefault;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-verbosity" || arg == "--verbosity") {
      if (i + 1 < argc) {
        verbosity = std::atoi(argv[++i]);
      }
    } else if (arg.rfind("--verbosity=", 0) == 0) {
      verbosity = std::atoi(arg.c_str() + std::strlen("--verbosity="));
    } else {
      pos.push_back(argv[i]);
    }
  }
  static const char* const kPositionals[] = {"<tech_lef>", "<cell_lef>", "<def>"};
  if (pos.size() != 3) {
    const std::string problem =
        pos.size() < 3
            ? "missing required argument " + std::string(kPositionals[pos.size()])
            : "too many positional arguments";
    eda::printUsageError(std::cerr, spec, problem);
    return 1;
  }

  utl::Logger logger;
  utl::Logger* lg = &logger;  // debugPrint's macro needs a pointer expression
  eda::applyVerbosity(lg, kGroup, verbosity);
  odb::dbDatabase* db = odb::dbDatabase::create();

  odb::dbBlock* block = nullptr;
  eda::Status status = loadDesign(db, logger, pos[0], pos[1], pos[2], &block);
  if (!status.ok()) {
    logger.warn(utl::UKN, kMsgReadFail, "{}", status.message);
    return 1;
  }

  logger.info(utl::UKN, kMsgCounts, "Instances: {}, Nets: {}",
              block->getInsts().size(), block->getNets().size());
  debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
             "block dbu/micron = {}", block->getDbUnitsPerMicron());

  status = roundTripWrite(db, block, logger);
  if (!status.ok()) {
    logger.warn(utl::UKN, kMsgReadFail, "{}", status.message);
    return 1;
  }

  logger.info(utl::UKN, kMsgDone, "Done. Wrote out.odb and out.def");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Top-level backstop for ordinary catchable exceptions (std::bad_alloc, an
  // STL throw, a bug this audit missed). NOTE: an odb utl::Logger::error()
  // throw is NOT reliably catchable this far from its origin — unwinding it all
  // the way to main() fails and calls std::terminate — so those are contained
  // at the reader boundary inside loadDesign(), not here.
  try {
    return runHelloOdb(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Fatal error: unknown exception." << std::endl;
    return 1;
  }
}
