#include "odb/db.h"
#include "odb/lefin.h"
#include "odb/defin.h"
#include "odb/defout.h"
#include "utl/Logger.h"

#include <iostream>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " <tech_lef> <cell_lef> <def>\n";
    return 1;
  }

  utl::Logger logger;
  odb::dbDatabase* db = odb::dbDatabase::create();

  odb::lefin lef_reader(db, &logger, false);
  std::cout << "Loading tech LEF: " << argv[1] << "\n";
  odb::dbLib* tech_lib = lef_reader.createTechAndLib("tech", argv[1], argv[1]);
  
  odb::dbTech* tech = tech_lib->getTech();
  std::cout << "Tech loaded, now loading cells LEF: " << argv[2] << "\n";
  odb::dbLib* cell_lib = lef_reader.createLib(tech, "nangate", argv[2]);

  std::cout << "LEFs loaded. Now reading DEF: " << argv[3] << "\n";
  odb::defin def_reader(db, &logger);
  std::vector<odb::dbLib*> search_libs = {tech_lib, cell_lib};
  def_reader.readChip(search_libs, argv[3], nullptr, false);

  odb::dbChip* chip = db->getChip();
  if (!chip) {
    std::cerr << "Failed to read chip from DEF\n";
    return 1;
  }
  
  odb::dbBlock* block = chip->getBlock();

  std::cout << "Instances: " << block->getInsts().size() << "\n";
  std::cout << "Nets: " << block->getNets().size() << "\n";

  std::ofstream odb_file("out.odb");
  db->write(odb_file);
  odb_file.close();

  odb::DefOut def_writer(&logger);
  def_writer.writeBlock(block, "out.def");

  std::cout << "Wrote out.odb and out.def\n";
  return 0;
}
