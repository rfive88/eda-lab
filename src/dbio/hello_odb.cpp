#include "odb/db.h"
#include "odb/lefin.h"
#include "odb/defin.h"
#include "odb/defout.h"
#include "support/logging.h"
#include "utl/Logger.h"

#include <cstdlib>
#include <cstring>
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
}  // namespace

int main(int argc, char** argv) {
  // Positional: <tech_lef> <cell_lef> <def>. Optional: -verbosity <level>
  // (or --verbosity=<level>) — repo-wide debug verbosity, see support/logging.h.
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
  if (pos.size() != 3) {
    std::cerr << "Usage: " << argv[0]
              << " <tech_lef> <cell_lef> <def> [-verbosity <level>]\n";
    return 1;
  }

  utl::Logger logger;
  utl::Logger* lg = &logger;  // debugPrint's macro needs a pointer expression
  eda::applyVerbosity(lg, kGroup, verbosity);
  odb::dbDatabase* db = odb::dbDatabase::create();

  odb::lefin lef_reader(db, &logger, false);
  logger.info(utl::UKN, kMsgTechLef, "Loading tech LEF: {}", pos[0]);
  odb::dbLib* tech_lib = lef_reader.createTechAndLib("tech", pos[0], pos[0]);

  odb::dbTech* tech = tech_lib->getTech();
  logger.info(utl::UKN, kMsgCellLef, "Loading cell LEF: {}", pos[1]);
  odb::dbLib* cell_lib = lef_reader.createLib(tech, "nangate", pos[1]);

  logger.info(utl::UKN, kMsgDef, "Reading DEF: {}", pos[2]);
  // readChip at the pinned OpenROAD SHA requires a pre-created chip.
  odb::dbChip* chip = odb::dbChip::create(db, tech, "chip");
  odb::defin def_reader(db, &logger);
  std::vector<odb::dbLib*> search_libs = {tech_lib, cell_lib};
  def_reader.readChip(search_libs, pos[2], chip, false);

  odb::dbBlock* block = chip->getBlock();
  if (!block) {
    logger.warn(utl::UKN, kMsgReadFail, "Failed to read chip from DEF");
    return 1;
  }

  logger.info(utl::UKN, kMsgCounts, "Instances: {}, Nets: {}",
              block->getInsts().size(), block->getNets().size());
  debugPrint(lg, utl::UKN, kGroup, eda::kVerbosityDetail,
             "block dbu/micron = {}", block->getDbUnitsPerMicron());

  logger.info(utl::UKN, kMsgWriteOdb, "Writing .odb: out.odb");
  std::ofstream odb_file("out.odb");
  db->write(odb_file);
  odb_file.close();

  logger.info(utl::UKN, kMsgWriteDef, "Writing DEF: out.def");
  odb::DefOut def_writer(&logger);
  def_writer.writeBlock(block, "out.def");

  logger.info(utl::UKN, kMsgDone, "Done. Wrote out.odb and out.def");
  return 0;
}
