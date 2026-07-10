// eda-lab: shared CLI --help / usage helper (repo-wide convention).
//
// Every standalone CLI in this repo renders its `--help` output AND its
// missing-argument usage/error block from ONE registered option list, so each
// option's one-line description is written exactly once in code and can never
// drift between the two paths. See the "CLI --help / usage" section of
// CLAUDE.md. Header-only: a CLI includes "support/cli.h", builds a CliSpec,
// and calls printHelp / printUsageError / wantsHelp.
//
// Scope: argv-level options only (flags, positional args). JSON config-field
// validation is a separate concern with its own fail-fast path (see
// netlistgen's cli_config).

#pragma once

#include <algorithm>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace eda {

// One argv-level option. `description` is the single source of truth: it is
// printed verbatim in both --help and the usage/error block.
struct CliOption
{
  std::string name;         // "<tech_lef>", "-verbosity", ...
  std::string metavar;      // "<level>" for value-taking flags; "" otherwise
  bool required = false;    // positional args are typically required
  std::string description;  // the one and only one-line description
};

struct CliSpec
{
  std::string program;             // usually argv[0]
  std::string summary;             // one-line program summary
  std::vector<CliOption> options;  // positionals first, then flags (convention)
};

// The standard -verbosity flag, so its one-line wording is identical across
// every CLI in the repo (single source repo-wide, not just per file).
inline CliOption verbosityOption()
{
  return CliOption{"-verbosity", "<level>", false,
                   "Debug verbosity: 0 = phase markers only (default), higher "
                   "= more detail (see the engine README)."};
}

namespace cli_detail {

// The implicit help option, rendered in every options block so its wording is
// also single-sourced.
inline CliOption helpOption()
{
  return CliOption{"-h, --help", "", false, "Show this help and exit."};
}

inline std::string optionToken(const CliOption& o)
{
  std::string t = o.name;
  if (!o.metavar.empty()) {
    t += ' ' + o.metavar;
  }
  return t;
}

inline std::string usageLine(const CliSpec& spec)
{
  std::string s = "Usage: " + spec.program;
  for (const CliOption& o : spec.options) {
    const std::string token = optionToken(o);
    s += ' ' + (o.required ? token : "[" + token + "]");
  }
  return s;
}

// Every option plus the trailing help entry, for both render paths.
inline std::vector<CliOption> allOptions(const CliSpec& spec)
{
  std::vector<CliOption> all = spec.options;
  all.push_back(helpOption());
  return all;
}

inline void writeOptions(std::ostream& os, const CliSpec& spec)
{
  const std::vector<CliOption> all = allOptions(spec);
  std::size_t width = 0;
  for (const CliOption& o : all) {
    width = std::max(width, optionToken(o).size());
  }
  os << "Options:\n";
  for (const CliOption& o : all) {
    const std::string token = optionToken(o);
    os << "  " << token << std::string(width - token.size() + 2, ' ')
       << o.description << "\n";
  }
}

}  // namespace cli_detail

// Full --help: summary, usage line, then every option with its description.
inline void printHelp(std::ostream& os, const CliSpec& spec)
{
  if (!spec.summary.empty()) {
    os << spec.program << " — " << spec.summary << "\n\n";
  }
  os << cli_detail::usageLine(spec) << "\n\n";
  cli_detail::writeOptions(os, spec);
}

// Missing / invalid argument: name the problem, print the same usage block
// (same descriptions as --help), and point at --help.
inline void printUsageError(std::ostream& os,
                            const CliSpec& spec,
                            const std::string& problem)
{
  os << spec.program << ": " << problem << "\n\n"
     << cli_detail::usageLine(spec) << "\n\n";
  cli_detail::writeOptions(os, spec);
  os << "\nTry '" << spec.program << " --help' for more information.\n";
}

// True if any argument is -h or --help.
inline bool wantsHelp(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-h") == 0
        || std::strcmp(argv[i], "--help") == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace eda
