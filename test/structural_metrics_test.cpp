// Tests for the structural_metrics engine (Spike SM1).
//
// Two flavours, mirroring the repo's other engine test suites:
//   * In-process: build a synthetic netlist with netlistgen::generateSynthetic,
//     build a Hypergraph from the resulting dbBlock, and call
//     sm::run_congestion_analysis / print_congestion_report directly. No data
//     files, no subprocess.
//   * CLI subprocess: spawn the built structural_metrics_cli binary via
//     fork/execv (same pattern as error_handling_test.cpp) so a crash surfaces
//     as a signal death rather than being masked by a shell. Needs
//     EDA_LAB_DATA_DIR + STRUCTURAL_METRICS_CLI_BIN compile definitions.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gtest/gtest.h"

#include "engines/netlistgen/netlistgen.h"
#include "engines/structural_metrics/structural_metrics.h"
#include "hypergraph/hypergraph.h"
#include "odb/db.h"

namespace {

// Build a statistical-mix synthetic netlist into `builder`, then a Hypergraph
// from its block. Returns the number of nets generated (> 0 on success).
int makeSyntheticHypergraph(eda::NetlistBuilder& builder, eda::Hypergraph& hg,
                            int num_insts, double sequential_ratio,
                            double target_avg_fanout, uint32_t seed)
{
  eda::SyntheticNetlistSpec spec;
  spec.num_insts = num_insts;
  spec.seed = seed;
  spec.sequential_ratio = sequential_ratio;
  spec.target_avg_fanout = target_avg_fanout;
  const int nets = eda::generateSynthetic(builder, spec);
  if (nets > 0) {
    hg.buildFromBlock(builder.block());
  }
  return nets;
}

// ---------------------------------------------------------------------------
// In-process tests
// ---------------------------------------------------------------------------

TEST(StructuralMetrics, BasicSanity)
{
  eda::NetlistBuilder builder("sm_basic");
  eda::Hypergraph hg;
  const int nets = makeSyntheticHypergraph(builder, hg, /*num_insts=*/100,
                                           /*sequential_ratio=*/0.3,
                                           /*target_avg_fanout=*/3.0,
                                           /*seed=*/42);
  ASSERT_GT(nets, 0);
  ASSERT_EQ(hg.numVertices(), 100);

  sm::CongestionAnalysisResult result;
  const eda::Status status =
      sm::run_congestion_analysis(hg, result, /*logger=*/nullptr);
  ASSERT_TRUE(status.ok()) << status.message;

  EXPECT_EQ(result.num_vertices, 100);

  // Score histogram sums to the vertex count, and every present score is a
  // valid 1-5 band with a non-degenerate spread (>= 2 distinct bands).
  //
  // NOTE: the brief's reference output shows a clean five-band spread, but that
  // is NOT reproducible with the current (unmodifiable) hg_metrics C1-C5 on any
  // available input. On this synthetic mix the C4 tangle score saturates to
  // ~1.0 for every vertex (locally dense: the 2-hop induced subgraph has more
  // terminals than cells, so the Rent exponent clamps to 1.0), which pins its
  // percentile rank at a constant 0.5 and caps the composite below band 5. (On
  // the real gcd design two metrics saturate instead — neighborhood density is
  // near-uniform and tangle collapses to 0 — leaving only bands 2-3.) So we
  // assert what the scoring plumbing genuinely guarantees: a valid, non-flat
  // 1-5 distribution summing to N. See README.md's "Score distribution" note.
  int sum = 0;
  for (const auto& [score, count] : result.congestion_report.score_histogram) {
    EXPECT_GE(score, 1);
    EXPECT_LE(score, 5);
    sum += count;
  }
  EXPECT_EQ(sum, 100);
  EXPECT_GE(result.congestion_report.score_histogram.size(), 2u)
      << "expected a non-degenerate score spread";

  EXPECT_GE(result.degeneracy, 0);
  EXPECT_LE(result.tangle_stats.max, 1.0 + 1e-6);
  EXPECT_FALSE(result.congestion_report.clusters.empty());
}

TEST(StructuralMetrics, EmptyHypergraph)
{
  eda::Hypergraph hg;  // no topology built
  sm::CongestionAnalysisResult result;
  const eda::Status status =
      sm::run_congestion_analysis(hg, result, /*logger=*/nullptr);
  ASSERT_TRUE(status.ok()) << status.message;

  EXPECT_EQ(result.num_vertices, 0);
  EXPECT_EQ(result.num_hyperedges, 0);
  EXPECT_TRUE(result.degree_histogram.empty());
  EXPECT_TRUE(result.fanout_histogram.empty());
  EXPECT_TRUE(result.congestion_report.score_histogram.empty());
  EXPECT_TRUE(result.congestion_report.clusters.empty());
}

TEST(StructuralMetrics, CustomHighFanoutThreshold)
{
  eda::NetlistBuilder builder("sm_hf");
  eda::Hypergraph hg;
  const int nets = makeSyntheticHypergraph(builder, hg, /*num_insts=*/50,
                                           /*sequential_ratio=*/0.3,
                                           /*target_avg_fanout=*/5.0,
                                           /*seed=*/7);
  ASSERT_GT(nets, 0);

  sm::CongestionAnalysisResult result;
  const eda::Status status = sm::run_congestion_analysis(
      hg, result, /*logger=*/nullptr, /*high_fanout_threshold=*/5);
  ASSERT_TRUE(status.ok()) << status.message;

  EXPECT_GE(result.high_fanout_count, 0);
  EXPECT_EQ(result.high_fanout_threshold, 5);
}

TEST(StructuralMetrics, CustomWeights)
{
  eda::NetlistBuilder builder("sm_weights");
  eda::Hypergraph hg;
  const int nets = makeSyntheticHypergraph(builder, hg, /*num_insts=*/100,
                                           /*sequential_ratio=*/0.3,
                                           /*target_avg_fanout=*/3.0,
                                           /*seed=*/11);
  ASSERT_GT(nets, 0);

  const hgm::CongestionWeights weights{0.6, 0.2, 0.2};
  sm::CongestionAnalysisResult result;
  const eda::Status status = sm::run_congestion_analysis(
      hg, result, /*logger=*/nullptr, /*high_fanout_threshold=*/20, weights);
  EXPECT_TRUE(status.ok()) << status.message;
}

TEST(StructuralMetrics, PrintReportSmoke)
{
  eda::NetlistBuilder builder("sm_print");
  eda::Hypergraph hg;
  const int nets = makeSyntheticHypergraph(builder, hg, /*num_insts=*/100,
                                           /*sequential_ratio=*/0.3,
                                           /*target_avg_fanout=*/3.0,
                                           /*seed=*/42);
  ASSERT_GT(nets, 0);

  sm::CongestionAnalysisResult result;
  ASSERT_TRUE(sm::run_congestion_analysis(hg, result, nullptr).ok());

  // A real logger so report() actually renders; content is not asserted (the
  // brief says visual inspection suffices for SM1). Just verify no crash.
  utl::Logger logger;
  sm::print_congestion_report(result, &logger);
  SUCCEED();
}

// ---------------------------------------------------------------------------
// CLI subprocess helpers + tests
// ---------------------------------------------------------------------------

struct RunResult
{
  bool exited_normally = false;  // false => killed by a signal (crash)
  int exit_code = -1;
};

// Run `args` directly via fork/execv (no shell), so a signal death is visible
// as WIFSIGNALED rather than hidden behind a shell's 128+signal exit.
RunResult runProcess(const std::vector<std::string>& args)
{
  RunResult r;
  const pid_t pid = fork();
  if (pid < 0) {
    return r;
  }
  if (pid == 0) {
    // Child: silence stdout/stderr so the analysis report doesn't clutter the
    // test log, then exec.
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    std::vector<char*> cargv;
    for (const std::string& a : args) {
      cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);
    execv(cargv[0], cargv.data());
    _exit(127);  // exec failed
  }
  int status = 0;
  waitpid(pid, &status, 0);
  r.exited_normally = WIFEXITED(status);
  if (r.exited_normally) {
    r.exit_code = WEXITSTATUS(status);
  }
  return r;
}

std::string dataDir()
{
  return EDA_LAB_DATA_DIR;
}

TEST(StructuralMetricsCli, LefDefHappyPath)
{
  const std::string tech = dataDir() + "/nangate45/Nangate45_tech.lef";
  const std::string cells = dataDir() + "/nangate45/Nangate45_stdcell.lef";
  const std::string def = dataDir() + "/gcd_nangate45.def";
  const RunResult r = runProcess({STRUCTURAL_METRICS_CLI_BIN, "--lef", tech,
                                  "--lef", cells, "--def", def});
  EXPECT_TRUE(r.exited_normally) << "CLI crashed instead of exiting cleanly";
  EXPECT_EQ(r.exit_code, 0);
}

TEST(StructuralMetricsCli, MissingDefIsError)
{
  const std::string tech = dataDir() + "/nangate45/Nangate45_tech.lef";
  const RunResult r =
      runProcess({STRUCTURAL_METRICS_CLI_BIN, "--lef", tech});
  EXPECT_TRUE(r.exited_normally);
  EXPECT_NE(r.exit_code, 0);
}

TEST(StructuralMetricsCli, NonexistentFileExitsCleanly)
{
  const RunResult r = runProcess({STRUCTURAL_METRICS_CLI_BIN, "--lef",
                                  "/nonexistent.lef", "--def",
                                  "/nonexistent.def"});
  EXPECT_TRUE(r.exited_normally) << "CLI crashed on a missing file";
  EXPECT_NE(r.exit_code, 0);
}

TEST(StructuralMetricsCli, HelpExitsZero)
{
  const RunResult r = runProcess({STRUCTURAL_METRICS_CLI_BIN, "--help"});
  EXPECT_TRUE(r.exited_normally);
  EXPECT_EQ(r.exit_code, 0);
}

TEST(StructuralMetricsCli, OdbMode)
{
  // Generate a small synthetic netlist and write it out as a native .odb, then
  // feed that file to the CLI in --odb mode.
  const std::filesystem::path odb_path =
      std::filesystem::temp_directory_path() / "eda_lab_sm1_odbmode.odb";
  {
    eda::NetlistBuilder builder("sm_odb");
    eda::SyntheticNetlistSpec spec;
    spec.num_insts = 60;
    spec.seed = 3;
    spec.sequential_ratio = 0.3;
    spec.target_avg_fanout = 3.0;
    ASSERT_GT(eda::generateSynthetic(builder, spec), 0);
    std::ofstream out(odb_path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    builder.db()->write(out);
  }
  ASSERT_TRUE(std::filesystem::exists(odb_path));

  const RunResult r =
      runProcess({STRUCTURAL_METRICS_CLI_BIN, "--odb", odb_path.string()});
  EXPECT_TRUE(r.exited_normally) << "CLI crashed in --odb mode";
  EXPECT_EQ(r.exit_code, 0);
  std::filesystem::remove(odb_path);
}

}  // namespace
