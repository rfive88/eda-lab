// eda-lab: JSON config layer for the netlistgen standalone CLI (Stage C).
//
// JSON is an input to the CLI executable ONLY — it is deliberately kept out of
// the netlistgen library (NetlistBuilder / generateSynthetic), which in-memory
// callers drive by constructing a SyntheticNetlistSpec in C++. The CLI's job
// is: parse JSON -> populate a SyntheticNetlistSpec -> call the same
// generateSynthetic in-memory callers use -> validate well-formedness ->
// write whichever of DEF / .odb were requested. One generation code path
// underneath, multiple entry points into it.
//
// The JSON schema is a serialization of SyntheticNetlistSpec's fields plus
// CLI-only I/O fields (output_def_path, output_odb_path) that have no
// equivalent on the C++ struct. Stage E adds output_verilog_path and
// primary-port fields to CliConfig and the parser without restructuring this.

#pragma once

#include <optional>
#include <ostream>
#include <string>

#include "engines/netlistgen/netlistgen.h"

namespace eda {

// Parsed CLI configuration: the in-memory generation spec plus CLI-only output
// paths. Each output path is independently optional; at least one must be set.
struct CliConfig
{
  SyntheticNetlistSpec spec;
  std::optional<std::string> output_def_path;
  std::optional<std::string> output_odb_path;
  // Stage E: std::optional<std::string> output_verilog_path; primary ports.
};

// Parse a JSON config string into `out`. Returns true on success; on failure
// returns false and sets `error` to a human-readable diagnostic. Enforces
// CLI-level rules here (well-formed JSON, required instance_count, at least one
// output path). Spec-level rules (mode exclusivity, distribution sum, etc.) are
// left to validateSpecConfig at generation time — one validation authority.
bool parseCliConfig(const std::string& json_text,
                    CliConfig& out,
                    std::string& error);

// Validate-before-write policy for the CLI: run validateNetlist on the
// builder's block and, only if it passes, write whichever outputs are set.
// Writes NOTHING when validation fails (no partial/invalid files). Returns
// true only if validation passed and all requested writes succeeded;
// otherwise writes a diagnostic to `err` and returns false. Exposed so the
// fail-fast gating can be tested directly on a hand-built malformed block.
bool validateAndWrite(NetlistBuilder& builder,
                      const CliConfig& config,
                      std::ostream& err);

// Full CLI run: read the config file at `config_path`, parse it, generate,
// auto-size the die area, validate, and write requested outputs, printing
// instance/net/pin counts to `out`. Returns a process exit code (0 = success);
// diagnostics go to `err`. main() is a thin wrapper over this.
int runCliFromFile(const std::string& config_path,
                   std::ostream& out,
                   std::ostream& err);

}  // namespace eda
