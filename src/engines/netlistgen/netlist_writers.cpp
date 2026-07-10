// See netlist_writers.h.

#include "engines/netlistgen/netlist_writers.h"

#include <fstream>

#include "odb/db.h"
#include "odb/defout.h"
#include "utl/Logger.h"

namespace eda {

bool writeDef(odb::dbBlock* block,
              const std::string& path,
              utl::Logger* logger)
{
  if (block == nullptr) {
    return false;
  }
  // DefOut requires a non-null logger; supply a local one when the caller has
  // none so the writer never dereferences null on a diagnostic.
  utl::Logger local_logger;
  utl::Logger* lg = logger != nullptr ? logger : &local_logger;

  odb::DefOut writer(lg);
  writer.setVersion(odb::DefOut::DEF_5_8);
  return writer.writeBlock(block, path.c_str());
}

bool writeOdb(odb::dbDatabase* db, const std::string& path)
{
  if (db == nullptr) {
    return false;
  }
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  db->write(file);
  return file.good();
}

}  // namespace eda
