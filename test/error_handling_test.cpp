// Regression tests for the repo-wide graceful-error-handling convention
// (support/status.h + the top-level try/catch in every main(); see the "Error
// handling" section of CLAUDE.md). Every case here confirms a bad input yields
// a clean, nonzero exit / failure return with a readable message — never a
// crash (segfault / abort / std::terminate).
//
// Two flavours:
//   * In-process: call eda::runCliFromFile() directly and check the exit code +
//     diagnostic. Covers failures that are caught at their source and return
//     cleanly WITHOUT throwing (missing config file, malformed JSON, an
//     auto-created output directory, an uncreatable output directory).
//   * Subprocess: run the real CLI binaries. Covers failures that surface as a
//     thrown exception inside OpenROAD (a nonexistent LEF path makes lefin call
//     utl::Logger::error(), which throws) — the point is to prove the
//     top-level catch in main() turns that throw into a clean nonzero exit
//     rather than letting it terminate the process. Needs HELLO_ODB_BIN /
//     NETLISTGEN_CLI_BIN (built CLI binary paths).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/cli_config.h"

namespace eda {
namespace {

// A subprocess result: combined stdout+stderr, whether it exited normally
// (vs. died on a signal — a crash), and the exit code if it did.
struct RunResult
{
  std::string output;
  bool exited_normally = false;  // false => killed by a signal (crash)
  int exit_code = -1;
};

// Run `argv` DIRECTLY via fork/execv (no intervening shell), so a child that
// dies on a signal (segfault/abort) is reported as WIFSIGNALED — a shell
// wrapper (popen/system) would instead exit normally with code 128+signal and
// hide the crash. This distinction is the whole point of these tests.
RunResult runProcess(const std::vector<std::string>& args)
{
  RunResult r;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return r;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    return r;
  }
  if (pid == 0) {
    // Child: redirect stdout+stderr into the pipe, then exec.
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    std::vector<char*> cargv;
    for (const std::string& a : args) {
      cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);
    execv(cargv[0], cargv.data());
    _exit(127);  // exec failed
  }
  // Parent: drain the pipe, then reap.
  close(pipefd[1]);
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    r.output.append(buf, static_cast<size_t>(n));
  }
  close(pipefd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  r.exited_normally = WIFEXITED(status);
  if (r.exited_normally) {
    r.exit_code = WEXITSTATUS(status);
  }
  return r;
}

bool contains(const std::string& hay, const std::string& needle)
{
  return hay.find(needle) != std::string::npos;
}

// A unique temp path under the system temp dir (no Date/random needed: the
// test name + a counter keeps them distinct within a run).
std::filesystem::path tempPath(const std::string& leaf)
{
  return std::filesystem::temp_directory_path()
         / ("eda_lab_errtest_" + leaf);
}

std::string writeTempFile(const std::string& leaf, const std::string& contents)
{
  const std::filesystem::path p = tempPath(leaf);
  std::ofstream f(p);
  f << contents;
  f.close();
  return p.string();
}

// ---------------------------------------------------------------------------
// In-process: failures caught at their source, returning cleanly (no throw)
// ---------------------------------------------------------------------------

TEST(ErrorHandling, MissingConfigFileReturnsNonzero)
{
  std::ostringstream err;
  const int rc = runCliFromFile("/no/such/config_does_not_exist.json", err, 0);
  EXPECT_EQ(rc, 1);
  EXPECT_TRUE(contains(err.str(), "cannot open config file"));
}

TEST(ErrorHandling, MalformedJsonReturnsNonzero)
{
  const std::string path =
      writeTempFile("malformed.json", "{ this is not valid json ");
  std::ostringstream err;
  const int rc = runCliFromFile(path, err, 0);
  EXPECT_EQ(rc, 1);
  EXPECT_TRUE(contains(err.str(), "config error"));
  std::filesystem::remove(path);
}

TEST(ErrorHandling, MissingOutputDirectoryIsCreated)
{
  // Valid statistical spec (no LEF needed), but the requested DEF output is in
  // a directory that does not exist yet. The CLI must create it (including any
  // missing parents) and succeed, rather than failing on the missing dir.
  const std::filesystem::path out_dir = tempPath("made_dir_xyz") / "nested";
  std::filesystem::remove_all(tempPath("made_dir_xyz"));
  const std::string out = (out_dir / "out.def").string();
  const std::string cfg = "{\"instance_count\": 200, \"sequential_ratio\": "
                          "0.2, \"target_avg_fanout\": 3.0, "
                          "\"output_def_path\": \""
                          + out + "\"}";
  const std::string path = writeTempFile("made_outdir.json", cfg);
  std::ostringstream err;
  const int rc = runCliFromFile(path, err, 0);
  EXPECT_EQ(rc, 0) << err.str();
  EXPECT_TRUE(std::filesystem::exists(out));
  std::filesystem::remove(path);
  std::filesystem::remove_all(tempPath("made_dir_xyz"));
}

TEST(ErrorHandling, UncreatableOutputDirectoryReturnsNonzero)
{
  // The requested DEF output nests a directory *under an existing regular file*,
  // so create_directories cannot make it. Generation succeeds; the write must
  // fail cleanly first with a directory diagnostic — no crash, no partial file.
  const std::string blocker = writeTempFile("blocker_file", "x");
  const std::string bad_out = (std::filesystem::path(blocker) / "sub" / "out.def")
                                  .string();
  const std::string cfg = "{\"instance_count\": 200, \"sequential_ratio\": "
                          "0.2, \"target_avg_fanout\": 3.0, "
                          "\"output_def_path\": \""
                          + bad_out + "\"}";
  const std::string path = writeTempFile("uncreatable_outdir.json", cfg);
  std::ostringstream err;
  const int rc = runCliFromFile(path, err, 0);
  EXPECT_EQ(rc, 1);
  EXPECT_TRUE(contains(err.str(), "cannot create output directory"));
  std::filesystem::remove(path);
  std::filesystem::remove(blocker);
}

// ---------------------------------------------------------------------------
// Subprocess: failures that throw inside OpenROAD must be contained by main()'s
// top-level catch and become a clean nonzero exit, never a crash.
// ---------------------------------------------------------------------------

TEST(ErrorHandling, HelloOdbNonexistentLefExitsCleanly)
{
  const RunResult r = runProcess({HELLO_ODB_BIN, "/no/such/tech.lef",
                                  "/no/such/cell.lef", "/no/such.def"});
  EXPECT_TRUE(r.exited_normally) << "process crashed (signal) instead of "
                                    "exiting cleanly; output:\n"
                                 << r.output;
  EXPECT_NE(r.exit_code, 0);
}

TEST(ErrorHandling, HelloOdbMalformedLefExitsCleanly)
{
  // A present-but-malformed LEF makes OpenROAD's lefin throw. Before the
  // boundary try/catch this aborted (std::terminate); it must now exit cleanly.
  const std::string lef = writeTempFile("garbage_tech.lef", "not valid lef\n");
  const std::string lef2 = writeTempFile("garbage_cell.lef", "not valid lef\n");
  const std::string def = writeTempFile("garbage.def", "not valid def\n");
  const RunResult r = runProcess({HELLO_ODB_BIN, lef, lef2, def});
  EXPECT_TRUE(r.exited_normally) << "process crashed (signal) instead of "
                                    "exiting cleanly; output:\n"
                                 << r.output;
  EXPECT_NE(r.exit_code, 0);
  std::filesystem::remove(lef);
  std::filesystem::remove(lef2);
  std::filesystem::remove(def);
}

TEST(ErrorHandling, NetlistgenNonexistentLefExitsCleanly)
{
  // LEF-backed mode with a nonexistent tech LEF: OpenROAD's createTechAndLib
  // throws; the CLI's top-level catch must turn it into a clean nonzero exit.
  const std::string out_odb = tempPath("nlgen_out.odb").string();
  const std::string cfg =
      "{\"instance_count\": 10, \"tech_lef_path\": \"/no/such/tech.lef\", "
      "\"sequential_ratio\": 0.2, \"target_avg_fanout\": 3.0, "
      "\"output_odb_path\": \"" + out_odb + "\"}";
  const std::string path = writeTempFile("nlgen_badlef.json", cfg);
  const RunResult r = runProcess({NETLISTGEN_CLI_BIN, path});
  EXPECT_TRUE(r.exited_normally) << "process crashed (signal) instead of "
                                    "exiting cleanly; output:\n"
                                 << r.output;
  EXPECT_NE(r.exit_code, 0);
  std::filesystem::remove(path);
}

TEST(ErrorHandling, NetlistgenMalformedLefExitsCleanly)
{
  // A present-but-malformed tech LEF: OpenROAD's lefin throws inside loadLef.
  // Before the boundary try/catch this SEGFAULTED; it must now exit cleanly.
  const std::string lef = writeTempFile("nlgen_garbage.lef", "not valid lef\n");
  const std::string out_odb = tempPath("nlgen_out2.odb").string();
  const std::string cfg =
      "{\"instance_count\": 10, \"tech_lef_path\": \"" + lef
      + "\", \"sequential_ratio\": 0.2, \"target_avg_fanout\": 3.0, "
      "\"output_odb_path\": \"" + out_odb + "\"}";
  const std::string path = writeTempFile("nlgen_malformedlef.json", cfg);
  const RunResult r = runProcess({NETLISTGEN_CLI_BIN, path});
  EXPECT_TRUE(r.exited_normally) << "process crashed (signal) instead of "
                                    "exiting cleanly; output:\n"
                                 << r.output;
  EXPECT_NE(r.exit_code, 0);
  std::filesystem::remove(path);
  std::filesystem::remove(lef);
}

}  // namespace
}  // namespace eda
