// eda-lab: structural Verilog output writer for the netlistgen engine
// (Stage E2).
//
// Completes the LEF-backed output triplet (.v + .def + .odb): walks the
// in-memory dbBlock — including the dbBTerm primary ports Stage E1 creates —
// and emits syntactically valid *structural* Verilog whose instance and net
// names are identical, by construction, to those in the DEF and .odb outputs
// (all three are serialized from the same block). That name-consistency is the
// point of the triplet: a generated design can be cross-checked across
// external EDA tools (OpenROAD, Yosys, ...) that read Verilog + DEF + LEF.
//
// LEF-backed mode only. A synthetic (connectivity-only) master has no
// synthesizable cell identity to emit as a module type, so the CLI/spec layer
// fails fast when output_verilog_path is set without a tech/cell LEF (see
// cli_config.cpp) — this writer is never reached in synthetic mode.
//
// Read-only: never mutates block state. Callable independently of the CLI
// (same contract as writeDef / writeOdb in netlist_writers.h), and — like
// them — invoked ONLY after validateNetlist passes, so a malformed netlist
// blocks all three writers before any file is produced.

#pragma once

#include <string>

namespace odb {
class dbBlock;
}  // namespace odb

namespace utl {
class Logger;
}

namespace eda {

// Write a structural Verilog netlist for `block` to `path`, with the top module
// named `top_module_name`. Returns false if the output file cannot be opened or
// the stream goes bad during the write (mirroring writeDef / writeOdb's bool
// contract — the CLI reports the failing path). `logger`, when non-null, gets a
// debug-gated (verbosity >= 1) trace of the write; the happy path is otherwise
// silent, exactly like the sibling writers. `top_module_name` is assumed to be
// a valid Verilog identifier — the CLI validates it at spec-build time.
bool writeVerilog(odb::dbBlock* block,
                  const std::string& path,
                  const std::string& top_module_name,
                  utl::Logger* logger = nullptr);

}  // namespace eda
