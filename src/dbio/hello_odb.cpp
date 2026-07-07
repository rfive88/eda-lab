#include "odb/db.h"
#include "odb/lefin.h"
#include "odb/defin.h"
#include "odb/defout.h"
#include "utl/Logger.h"

#include <iostream>

int main(int argc, char** argv) {
  // argv[1] = tech LEF, argv[2] = cell LEF, argv[3] = DEF
  utl::Logger logger;
  odb::dbDatabase* db = odb::dbDatabase::create();

  odb::lefin lef_reader(db, &logger, /*ignore_non_routing_layers*/ false);
  odb::dbLib* tech_lib = lef_reader.createTechAndLib("tech", argv[1]);
  // load additional cell LEF(s) against the same tech as needed — check
  // current lefin API for the right call at your pinned SHA

  odb::defin def_reader(db, &logger);
  odb::dbChip* chip = def_reader.createChip({tech_lib}, argv[3]);
  odb::dbBlock* block = chip->getBlock();

  std::cout << "Instances: " << block->getInsts().size() << "\n";
  std::cout << "Nets: "      << block->getNets().size()  << "\n";

  db->write("out.odb" /* or stream, check current signature */);

  odb::defout def_writer(&logger);
  def_writer.writeBlock(block, "out.def");

  return 0;
}