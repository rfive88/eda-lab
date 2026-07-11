// eda-lab: netlistgen standalone CLI executable (Stage C).
//
// Thin wrapper over runCliFromFile (cli_config.h): parse a JSON config file,
// generate a netlist through the same generateSynthetic in-memory callers use,
// validate well-formedness, and write the requested DEF / .odb outputs. Same
// pattern as hello_odb — a plain C++ executable linking the netlistgen library.
//
// Usage:  netlistgen_cli <config.json> [-verbosity <level>]
//
// -verbosity <level> (or --verbosity=<level>) raises the run's debug verbosity;
// unset = default phase markers only (see support/logging.h). Phase markers and
// the summary go to stdout via utl::Logger; hard errors go to stderr.
//
// CAVEAT: combinational-loop avoidance has not landed yet (Stage D). Generated
// output is structurally well-formed (single driver, >=1 sink, no dangling
// nets) but MAY contain combinational loops; treat files as preview /
// manual-inspection artifacts, not yet valid fixtures for downstream flows.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "engines/netlistgen/cli_config.h"
#include "support/cli.h"

int main(int argc, char** argv)
{
  const eda::CliSpec spec{
      argv[0],
      "Generate a synthetic netlist from a JSON config; write DEF / .odb.",
      {{"<config.json>", "", true,
        "JSON generation config (schema in the netlistgen README)."},
       eda::verbosityOption()}};
  if (eda::wantsHelp(argc, argv)) {
    eda::printHelp(std::cout, spec);
    return 0;
  }

  const char* config_path = nullptr;
  int verbosity = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-verbosity" || arg == "--verbosity") {
      if (i + 1 < argc) {
        verbosity = std::atoi(argv[++i]);
      }
    } else if (arg.rfind("--verbosity=", 0) == 0) {
      verbosity = std::atoi(arg.c_str() + std::strlen("--verbosity="));
    } else if (config_path == nullptr) {
      config_path = argv[i];
    }
  }
  if (config_path == nullptr) {
    eda::printUsageError(std::cerr, spec,
                         "missing required argument <config.json>");
    return 1;
  }

  // Top-level backstop for ordinary catchable exceptions (std::bad_alloc, an
  // STL throw, a bug this audit missed). NOTE: an odb utl::Logger::error()
  // throw is NOT reliably catchable this far up — unwinding it to main() fails
  // and crashes — so odb throws are contained at the reader boundary inside
  // NetlistBuilder::loadLef(), and missing files are prechecked before that.
  try {
    return eda::runCliFromFile(config_path, std::cerr, verbosity);
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Fatal error: unknown exception." << std::endl;
    return 1;
  }
}
