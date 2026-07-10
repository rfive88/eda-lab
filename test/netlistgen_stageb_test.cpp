// Stage B tests for the netlistgen engine: LEF-backed generation, the
// statistical cell mix (both modes), the max-entropy solve, spec validation,
// and the shared signal-pin counting rule. Needs EDA_LAB_DATA_DIR (Nangate45
// LEF fixtures + the hand-written data/synth_cells/twobucket.lef fixture).

#include <array>
#include <cmath>
#include <string>

#include "gtest/gtest.h"
#include "engines/netlistgen/netlistgen.h"
#include "hypergraph/hypergraph.h"
#include "odb/db.h"

namespace eda {
namespace {

std::string dataDir()
{
  return std::string(EDA_LAB_DATA_DIR);
}
std::string techLef()
{
  return dataDir() + "/nangate45/Nangate45_tech.lef";
}
std::string stdcellLef()
{
  return dataDir() + "/nangate45/Nangate45_stdcell.lef";
}
std::string twoBucketLef()
{
  return dataDir() + "/synth_cells/twobucket.lef";
}

// ---------------------------------------------------------------------------
// Max-entropy solve
// ---------------------------------------------------------------------------

TEST(MaxEntropyTest, HitsTargetAndSpreadsMass)
{
  const std::array<double, kNumCombBuckets> anchors = {2, 3, 4, 5, 6};
  for (double target : {2.5, 3.0, 3.5, 4.0, 4.7}) {
    const auto p = maxEntropyDistribution(anchors, target);
    double sum = 0.0, mean = 0.0;
    int spread = 0;
    for (int i = 0; i < kNumCombBuckets; ++i) {
      sum += p[i];
      mean += p[i] * anchors[i];
      if (p[i] > 0.01) {
        ++spread;
      }
    }
    EXPECT_NEAR(sum, 1.0, 1e-9) << "target " << target;
    EXPECT_NEAR(mean, target, 1e-6) << "target " << target;
    // Real tilting, not a 2-point interpolation: mass on >2 buckets.
    EXPECT_GT(spread, 2) << "target " << target;
  }
}

TEST(MaxEntropyTest, SymmetricTargetIsUniform)
{
  // The midpoint anchor (4) yields the uniform distribution (theta = 0).
  const auto p = maxEntropyDistribution({2, 3, 4, 5, 6}, 4.0);
  for (double v : p) {
    EXPECT_NEAR(v, 0.2, 1e-6);
  }
}

// ---------------------------------------------------------------------------
// Spec-config validation (no LEF needed)
// ---------------------------------------------------------------------------

TEST(SpecValidationTest, ModeAValid)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));
}

TEST(SpecValidationTest, ModeBValid)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.target_avg_fanout = 3.5;
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));
}

TEST(SpecValidationTest, BothModesFail)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  spec.target_avg_fanout = 3.5;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(SpecValidationTest, NeitherModeFails)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.sequential_ratio = 0.1;  // engages statistical mix, but no mode set
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(SpecValidationTest, DistributionMustSumTo100)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.combinational_pin_distribution = std::array<double, 5>{10, 20, 20, 20, 20};
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(SpecValidationTest, TargetOutOfRangeFails)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.target_avg_fanout = 6.0;  // must be strictly inside (2, 6)
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
  spec.target_avg_fanout = 1.5;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(SpecValidationTest, SequentialRatioRangeFails)
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.sequential_ratio = 1.5;
  spec.target_avg_fanout = 3.5;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

// A bad-config spec is rejected end to end by generateSynthetic too.
TEST(SpecValidationTest, GenerateRejectsBadConfig)
{
  NetlistBuilder nb;
  SyntheticNetlistSpec spec;
  spec.num_insts = 10;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  spec.target_avg_fanout = 3.5;  // both modes -> reject
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

// ---------------------------------------------------------------------------
// Signal-pin counting (excludes power/ground)
// ---------------------------------------------------------------------------

TEST(SignalPinCountTest, ExcludesPowerGround)
{
  NetlistBuilder nb("pincount");
  ASSERT_TRUE(nb.loadLef(techLef(), {stdcellLef()}));

  odb::dbMaster* nand2 = nullptr;
  odb::dbMaster* fa = nullptr;
  for (odb::dbMaster* m : nb.masters()) {
    if (m->getName() == "NAND2_X1") {
      nand2 = m;
    }
    if (m->getName() == "FA_X1") {
      fa = m;
    }
  }
  ASSERT_NE(nand2, nullptr);
  // A1, A2 (in), ZN (out) are SIGNAL; VDD/VSS are POWER/GROUND.
  EXPECT_EQ(signalPinCount(nand2), 3);
  EXPECT_FALSE(isSequentialMaster(nand2));

  // FA_X1: A, B, CI in + CO, S out = 5 signal pins, 2 outputs.
  ASSERT_NE(fa, nullptr);
  EXPECT_EQ(signalPinCount(fa), 5);
}

// ---------------------------------------------------------------------------
// LEF-backed generation
// ---------------------------------------------------------------------------

SyntheticNetlistSpec lefSpec()
{
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {stdcellLef()};
  spec.num_insts = 500;
  spec.sequential_ratio = 0.0;  // Nangate45 tags no CLOCK pins (see README)
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 3;
  return spec;
}

TEST(LefGenerationTest, GeneratesValidNetlist)
{
  NetlistBuilder nb("lefgen");
  const SyntheticNetlistSpec spec = lefSpec();
  const int nets = generateSynthetic(nb, spec);
  ASSERT_GT(nets, 0);

  odb::dbBlock* block = nb.block();
  EXPECT_EQ(static_cast<int>(block->getInsts().size()), spec.num_insts);

  Hypergraph hg;
  hg.buildFromBlock(block);
  EXPECT_EQ(hg.numVertices(), spec.num_insts);
  EXPECT_EQ(hg.numHyperedges(), nets);
  for (int e = 0; e < hg.numHyperedges(); ++e) {
    const int fanout = hg.hyperedgeOffsets()[e + 1] - hg.hyperedgeOffsets()[e];
    EXPECT_GE(fanout, 2) << "net " << e;
    EXPECT_LE(fanout, spec.max_fanout) << "net " << e;
  }
}

// Multi-output combinational masters (FA_X1, the DFF variants whose CK is not
// CLOCK-tagged so they read as combinational with Q+QN) never get chosen.
TEST(LefGenerationTest, ExcludesMultiOutputMasters)
{
  NetlistBuilder nb("lefexcl");
  const int nets = generateSynthetic(nb, lefSpec());
  ASSERT_GT(nets, 0);
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    EXPECT_NE(signalPinCount(inst->getMaster()), 0);
    // Any chosen master must be single-output combinational.
    int outs = 0;
    for (odb::dbMTerm* mt : inst->getMaster()->getMTerms()) {
      if (mt->getIoType() == odb::dbIoType::OUTPUT
          && (mt->getSigType() == odb::dbSigType::SIGNAL
              || mt->getSigType() == odb::dbSigType::CLOCK)) {
        ++outs;
      }
    }
    EXPECT_EQ(outs, 1) << inst->getMaster()->getName();
    EXPECT_NE(inst->getMaster()->getName(), "FA_X1");
  }
}

// ---------------------------------------------------------------------------
// Empty-bucket fail-fast
// ---------------------------------------------------------------------------

TEST(LefBucketTest, EmptyRequestedBucketFailsFast)
{
  // twobucket.lef populates only buckets 2 (BUFB2) and 4 (AND3B4). A
  // distribution that puts weight on the empty 3-pin bucket must fail.
  NetlistBuilder nb("emptybucket");
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {twoBucketLef()};
  spec.num_insts = 50;
  spec.combinational_pin_distribution = std::array<double, 5>{50, 50, 0, 0, 0};
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(LefBucketTest, PopulatedBucketsSucceed)
{
  NetlistBuilder nb("okbucket");
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {twoBucketLef()};
  spec.num_insts = 50;
  // Weight only on buckets 2 (index 0) and 4 (index 2), both populated.
  spec.combinational_pin_distribution = std::array<double, 5>{50, 0, 50, 0, 0};
  EXPECT_GT(generateSynthetic(nb, spec), 0);
}

TEST(LefBucketTest, EmptySequentialClassFailsFast)
{
  // Nangate45 tags no CLOCK pins, so the sequential class is empty; asking
  // for sequential cells must fail fast.
  NetlistBuilder nb("emptyseq");
  SyntheticNetlistSpec spec;
  spec.tech_lef_path = techLef();
  spec.cell_lef_paths = {stdcellLef()};
  spec.num_insts = 50;
  spec.sequential_ratio = 0.3;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

// ---------------------------------------------------------------------------
// Statistical validation on a large synthetic run
// ---------------------------------------------------------------------------

TEST(StatisticalMixTest, SyntheticProportionsWithinTolerance)
{
  NetlistBuilder nb;
  SyntheticNetlistSpec spec;
  spec.num_insts = 10000;
  spec.sequential_ratio = 0.2;
  spec.combinational_pin_distribution = std::array<double, 5>{30, 25, 20, 15, 10};
  spec.seed = 11;
  const int nets = generateSynthetic(nb, spec);
  ASSERT_GT(nets, 0);

  // Recover empirical proportions from the created instances.
  int seq = 0;
  std::array<int, 5> bucket{};
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    const int pins = signalPinCount(inst->getMaster());
    // Synthetic sequential representative is SEQ (2 in + 1 out = 3 pins);
    // combinational reps are COMB_k. Classify by master name prefix.
    if (inst->getMaster()->getName() == "SEQ") {
      ++seq;
    } else {
      const int b = (pins >= 6) ? 4 : pins - 2;
      ++bucket[b];
    }
  }
  const double tol = 2.0;
  EXPECT_NEAR(100.0 * seq / spec.num_insts, 20.0, tol);
  const int comb = spec.num_insts - seq;
  const std::array<double, 5> want = {30, 25, 20, 15, 10};
  for (int i = 0; i < 5; ++i) {
    EXPECT_NEAR(100.0 * bucket[i] / comb, want[i], tol) << "bucket " << i;
  }
}

TEST(StatisticalMixTest, ModeBSyntheticMeanMatchesTarget)
{
  NetlistBuilder nb;
  SyntheticNetlistSpec spec;
  spec.num_insts = 10000;
  spec.target_avg_fanout = 3.5;
  spec.seed = 21;
  ASSERT_GT(generateSynthetic(nb, spec), 0);

  long pins = 0;
  int n = 0;
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    pins += signalPinCount(inst->getMaster());
    ++n;
  }
  EXPECT_NEAR(static_cast<double>(pins) / n, 3.5, 0.15);
}

// A deliberately-tight tolerance still succeeds (the mismatch is a logged
// warning, never a generation failure).
TEST(StatisticalMixTest, ToleranceMismatchWarnsButSucceeds)
{
  NetlistBuilder nb;
  SyntheticNetlistSpec spec;
  spec.num_insts = 2000;
  spec.sequential_ratio = 0.2;
  spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
  spec.distribution_tolerance_pct = 0.0001;  // impossible to satisfy exactly
  spec.seed = 5;
  EXPECT_GT(generateSynthetic(nb, spec), 0);
}

TEST(StatisticalMixTest, DeterministicForSeed)
{
  auto run = [](uint32_t seed) {
    SyntheticNetlistSpec spec;
    spec.num_insts = 800;
    spec.sequential_ratio = 0.15;
    spec.combinational_pin_distribution = std::array<double, 5>{20, 20, 20, 20, 20};
    spec.seed = seed;
    NetlistBuilder nb;
    generateSynthetic(nb, spec);
    Hypergraph hg;
    hg.buildFromBlock(nb.block());
    return std::make_pair(hg.hyperedgeOffsets(), hg.pinList());
  };
  EXPECT_EQ(run(9), run(9));
  EXPECT_NE(run(9), run(10));
}

// ---------------------------------------------------------------------------
// Die-area sizing helper
// ---------------------------------------------------------------------------

TEST(DieAreaTest, ScalesWithInstanceCount)
{
  NetlistBuilder nb("die");
  ASSERT_TRUE(nb.loadLef(techLef(), {stdcellLef()}));
  const auto small = nb.estimateDieArea(100);
  const auto large = nb.estimateDieArea(10000);
  EXPECT_GT(small.ux, 0);
  EXPECT_GT(small.uy, 0);
  const long small_area = static_cast<long>(small.ux) * small.uy;
  const long large_area = static_cast<long>(large.ux) * large.uy;
  EXPECT_GT(large_area, small_area);
}

}  // namespace
}  // namespace eda
