// Tests for the repo-wide CLI --help / usage convention (support/cli.h):
//   1. The shared renderer prints each option's one-line description in BOTH
//      --help and the usage/error block (single-source-of-truth enforcement).
//   2. Each real CLI (hello_odb, netlistgen_cli): --help exits 0 and lists the
//      options; a missing required argument exits nonzero with the SAME
//      description text as --help; a valid invocation is unaffected (covered by
//      the Stage C smoke test, not re-spawned here).
//
// Needs HELLO_ODB_BIN / NETLISTGEN_CLI_BIN (built CLI binary paths). No data
// files: only the --help and missing-argument paths are exercised.

#include <cstdio>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <utility>

#include "gtest/gtest.h"

#include "support/cli.h"

namespace eda {
namespace {

// Run `cmd` (stderr merged into stdout) and return {combined output, exit code}.
std::pair<std::string, int> runCapture(const std::string& cmd)
{
  FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (pipe == nullptr) {
    return {"", -1};
  }
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
    out.append(buf, n);
  }
  const int status = pclose(pipe);
  return {out, WIFEXITED(status) ? WEXITSTATUS(status) : -1};
}

bool contains(const std::string& hay, const std::string& needle)
{
  return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Shared renderer: the single-source-of-truth guarantee, unit-level
// ---------------------------------------------------------------------------

CliSpec sampleSpec()
{
  return CliSpec{"prog",
                 "does a thing",
                 {{"<in>", "", true, "the input path"}, verbosityOption()}};
}

TEST(CliRenderTest, HelpListsEveryDescription)
{
  std::ostringstream os;
  printHelp(os, sampleSpec());
  const std::string s = os.str();
  EXPECT_TRUE(contains(s, "does a thing"));   // summary
  EXPECT_TRUE(contains(s, "<in>"));
  EXPECT_TRUE(contains(s, "the input path"));
  EXPECT_TRUE(contains(s, "-verbosity"));
  EXPECT_TRUE(contains(s, "Debug verbosity"));  // shared verbosity wording
  EXPECT_TRUE(contains(s, "--help"));           // implicit help entry listed
}

TEST(CliRenderTest, UsageErrorReusesSameDescriptions)
{
  const CliSpec spec = sampleSpec();
  std::ostringstream help;
  printHelp(help, spec);
  std::ostringstream err;
  printUsageError(err, spec, "missing required argument <in>");
  const std::string e = err.str();

  // Names the specific problem, and points at --help.
  EXPECT_TRUE(contains(e, "missing required argument <in>"));
  EXPECT_TRUE(contains(e, "--help' for more information"));

  // Every option description from --help reappears verbatim in the error
  // block — the same strings, not a divergent/generic message.
  for (const std::string& desc :
       {std::string("the input path"), std::string("Debug verbosity")}) {
    EXPECT_TRUE(contains(help.str(), desc));
    EXPECT_TRUE(contains(e, desc)) << "usage error dropped: " << desc;
  }
}

// ---------------------------------------------------------------------------
// Real CLIs: --help and missing-argument behaviour, end to end
// ---------------------------------------------------------------------------

// Asserts the convention for one CLI binary: --help exits 0 and contains the
// expected description; no-args exits nonzero and repeats that same text.
void checkCli(const std::string& bin, const std::string& desc_substr)
{
  const auto [help_out, help_rc] = runCapture(bin + " --help");
  EXPECT_EQ(help_rc, 0) << bin << " --help should exit 0";
  EXPECT_TRUE(contains(help_out, desc_substr)) << help_out;
  EXPECT_TRUE(contains(help_out, "Debug verbosity")) << help_out;

  const auto [err_out, err_rc] = runCapture(bin);  // no required args
  EXPECT_NE(err_rc, 0) << bin << " with no args should exit nonzero";
  EXPECT_TRUE(contains(err_out, "missing required argument")) << err_out;
  // Single source of truth: the missing-arg block carries the same one-line
  // descriptions as --help.
  EXPECT_TRUE(contains(err_out, desc_substr)) << err_out;
  EXPECT_TRUE(contains(err_out, "Debug verbosity")) << err_out;
}

TEST(CliBinaryTest, NetlistgenCliHelpAndUsage)
{
  checkCli(NETLISTGEN_CLI_BIN, "JSON generation config");
}

TEST(CliBinaryTest, HelloOdbHelpAndUsage)
{
  checkCli(HELLO_ODB_BIN, "Technology LEF");
}

}  // namespace
}  // namespace eda
