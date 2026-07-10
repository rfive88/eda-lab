// eda-lab: DEF / `.odb` output writers for the netlistgen engine (Stage C).
//
// Thin, dependency-light wrappers around OpenDB's DefOut and dbDatabase::write
// so a generated block can be persisted to disk. Both are callable
// independently of the CLI (Stage E's Verilog writer, pybind11 bindings, or
// Stage 3 test code can invoke them directly). No JSON, no config — just a
// block/db and a path.

#pragma once

#include <string>

namespace odb {
class dbBlock;
class dbDatabase;
}  // namespace odb

namespace utl {
class Logger;
}

namespace eda {

// Write `block` to a DEF file (DEF 5.8). Returns false if DefOut reports a
// write failure. `logger` is optional — a local throwaway logger is used when
// null. No PINS section is emitted (no primary ports exist until Stage E).
bool writeDef(odb::dbBlock* block,
              const std::string& path,
              utl::Logger* logger = nullptr);

// Write the full database (tech + libs + block) to a `.odb` stream file.
// dbDatabase::write takes a std::ostream, not a filename, so this opens an
// ofstream and checks it. Returns false if the file cannot be opened or the
// stream goes bad during the write.
bool writeOdb(odb::dbDatabase* db, const std::string& path);

}  // namespace eda
