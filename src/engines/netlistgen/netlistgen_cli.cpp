// eda-lab: netlistgen standalone CLI executable (Stage C).
//
// Thin wrapper over runCliFromFile (cli_config.h): parse a JSON config file,
// generate a netlist through the same generateSynthetic in-memory callers use,
// validate well-formedness, and write the requested DEF / .odb outputs. Same
// pattern as hello_odb — a plain C++ executable linking the netlistgen library.
//
// Usage:  netlistgen_cli <config.json>
//
// CAVEAT: combinational-loop avoidance has not landed yet (Stage D). Generated
// output is structurally well-formed (single driver, >=1 sink, no dangling
// nets) but MAY contain combinational loops; treat files as preview /
// manual-inspection artifacts, not yet valid fixtures for downstream flows.

#include <iostream>

#include "engines/netlistgen/cli_config.h"

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <config.json>\n";
    return 1;
  }
  return eda::runCliFromFile(argv[1], std::cout, std::cerr);
}
