// Tests for the netlistgen Stage E1 feature: primary I/O port generation
// governed by Rent's rule (see docs/briefs/spike-netlistgen-E1-io-rent.md).
// Needs no data files (synthetic mode only).
//
// Two deliberate deviations from a literal reading of the brief, both
// documented in README.md's "Primary I/O generation (Stage E1)" section and
// exercised directly here:
//   - A PI-selected net's existing internal driver is DISCONNECTED and
//     replaced by the PI (never added "alongside" it) — real designs have a
//     net driven either by a primary input or by an instance output, never
//     both, and this repo treats "exactly one driver per net" as a hard,
//     tested invariant (validateNetlist, now bTerm-aware).
//   - netlistgen never touches the Hypergraph engine (see its "Input /
//     output contract"); it returns raw dbNet*/dbInst* lists via RentStats,
//     and it is the CALLER's job to translate them into hgm.is_pi/is_po
//     (hyperedge/net planes — a bare port has no dbInst and hence no vertex
//     in this codebase's model) and hgm.is_boundary_buf/is_boundary_reg
//     (vertex planes, on the real boundary instances Stage E1 creates).
//     `applyRentPlanes` below is exactly that translation, done once here
//     for test verification.

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/netlist_validation.h"
#include "engines/netlistgen/netlistgen.h"
#include "hypergraph/hypergraph.h"

#include "odb/db.h"

namespace eda {
namespace {

// See the file-level comment: netlistgen never sets Hypergraph planes
// itself. This is the translation a caller (or a future hg_metrics module)
// would perform from RentStats into the plane names/semantics the brief
// specifies.
void applyRentPlanes(Hypergraph& hg, const RentStats& stats)
{
  if (!stats.engaged) {
    return;
  }
  std::vector<bool>& is_pi = hg.hyperedgeBoolPlane("hgm.is_pi");
  for (odb::dbNet* n : stats.pi_nets) {
    const int idx = hg.hyperedgeIndex(odb::dbId<odb::dbNet>(n->getId()));
    if (idx != Hypergraph::kInvalidIndex) {
      is_pi[idx] = true;
    }
  }
  std::vector<bool>& is_po = hg.hyperedgeBoolPlane("hgm.is_po");
  for (odb::dbNet* n : stats.po_nets) {
    const int idx = hg.hyperedgeIndex(odb::dbId<odb::dbNet>(n->getId()));
    if (idx != Hypergraph::kInvalidIndex) {
      is_po[idx] = true;
    }
  }
  std::vector<bool>& is_buf = hg.vertexBoolPlane("hgm.is_boundary_buf");
  for (odb::dbInst* i : stats.boundary_buf_insts) {
    const int idx = hg.vertexIndex(odb::dbId<odb::dbInst>(i->getId()));
    if (idx != Hypergraph::kInvalidIndex) {
      is_buf[idx] = true;
    }
  }
  std::vector<bool>& is_reg = hg.vertexBoolPlane("hgm.is_boundary_reg");
  for (odb::dbInst* i : stats.boundary_reg_insts) {
    const int idx = hg.vertexIndex(odb::dbId<odb::dbInst>(i->getId()));
    if (idx != Hypergraph::kInvalidIndex) {
      is_reg[idx] = true;
    }
  }
}

SyntheticNetlistSpec baseSpec()
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 2000;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;
  spec.seed = 11;
  return spec;
}

// ---------------------------------------------------------------------------
// 1. No E1 params: unaffected.
// ---------------------------------------------------------------------------

TEST(RentTest, NoE1ParamsLeavesStatsUnengagedAndPlanesAbsent)
{
  NetlistBuilder nb("noE1");
  const SyntheticNetlistSpec spec = baseSpec();  // no rent_k/rent_p
  RentStats stats;
  const int nets = generateSynthetic(nb, spec, nullptr, &stats);
  ASSERT_GT(nets, 0);
  EXPECT_FALSE(stats.engaged);
  EXPECT_EQ(stats.T_target, 0);
  EXPECT_TRUE(stats.pi_nets.empty());
  EXPECT_TRUE(stats.po_nets.empty());

  Hypergraph hg;
  hg.buildFromBlock(nb.block());
  EXPECT_FALSE(hg.hasHyperedgePlane("hgm.is_pi"));
  EXPECT_FALSE(hg.hasHyperedgePlane("hgm.is_po"));
  EXPECT_FALSE(hg.hasVertexPlane("hgm.is_boundary_buf"));
  EXPECT_FALSE(hg.hasVertexPlane("hgm.is_boundary_reg"));

  // validateNetlist's bTerm-folding is additive-only: no bterms exist here,
  // so well-formedness is unaffected by the Stage E1 extension.
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

// ---------------------------------------------------------------------------
// 2. Basic E1.
// ---------------------------------------------------------------------------

TEST(RentTest, BasicE1)
{
  NetlistBuilder nb("basicE1");
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;

  RentStats stats;
  const int nets = generateSynthetic(nb, spec, nullptr, &stats);
  ASSERT_GT(nets, 0);
  ASSERT_TRUE(stats.engaged);
  EXPECT_FALSE(stats.capped);

  const int expected_T =
      static_cast<int>(std::lround(2.5 * std::pow(2000, 0.60)));
  EXPECT_EQ(stats.T_target, expected_T);
  EXPECT_NEAR(stats.T_actual, stats.T_target, 0.05 * stats.T_target + 1);

  // Default io_input_ratio (0.60).
  EXPECT_NEAR(stats.T_in, std::lround(0.60 * stats.T_actual), 1);
  EXPECT_EQ(stats.T_in + stats.T_out, stats.T_actual);

  EXPECT_TRUE(std::isfinite(stats.p_actual));
  EXPECT_GT(stats.p_actual, 0.0);

  Hypergraph hg;
  hg.buildFromBlock(nb.block());
  applyRentPlanes(hg, stats);

  const std::vector<bool>& is_pi = hg.hyperedgeBoolPlane("hgm.is_pi");
  const std::vector<bool>& is_po = hg.hyperedgeBoolPlane("hgm.is_po");
  for (odb::dbNet* n : stats.pi_nets) {
    const int idx = hg.hyperedgeIndex(odb::dbId<odb::dbNet>(n->getId()));
    ASSERT_NE(idx, Hypergraph::kInvalidIndex);
    EXPECT_TRUE(is_pi[idx]);
  }
  for (odb::dbNet* n : stats.po_nets) {
    const int idx = hg.hyperedgeIndex(odb::dbId<odb::dbNet>(n->getId()));
    ASSERT_NE(idx, Hypergraph::kInvalidIndex);
    EXPECT_TRUE(is_po[idx]);
  }

  const std::vector<bool>& is_buf = hg.vertexBoolPlane("hgm.is_boundary_buf");
  const std::vector<bool>& is_reg = hg.vertexBoolPlane("hgm.is_boundary_reg");
  EXPECT_EQ(static_cast<int>(stats.boundary_buf_insts.size()),
            stats.n_buffered);
  EXPECT_EQ(static_cast<int>(stats.boundary_reg_insts.size()),
            stats.n_registered);
  ASSERT_GT(stats.boundary_buf_insts.size() + stats.boundary_reg_insts.size(),
            0u)
      << "expected at least one boundary cell across 2000 insts / T="
      << stats.T_actual;
  for (odb::dbInst* i : stats.boundary_buf_insts) {
    const int idx = hg.vertexIndex(odb::dbId<odb::dbInst>(i->getId()));
    ASSERT_NE(idx, Hypergraph::kInvalidIndex);
    EXPECT_TRUE(is_buf[idx]);
    EXPECT_FALSE(is_reg[idx]);
  }
  for (odb::dbInst* i : stats.boundary_reg_insts) {
    const int idx = hg.vertexIndex(odb::dbId<odb::dbInst>(i->getId()));
    ASSERT_NE(idx, Hypergraph::kInvalidIndex);
    EXPECT_TRUE(is_reg[idx]);
    EXPECT_FALSE(is_buf[idx]);
  }

  // is_boundary_reg must NOT be set on any Stage A/B internal FF: every
  // sequential instance not in boundary_reg_insts stays false.
  std::unordered_set<odb::dbInst*> boundary_reg_set(
      stats.boundary_reg_insts.begin(), stats.boundary_reg_insts.end());
  int internal_seq_checked = 0;
  for (odb::dbInst* inst : nb.block()->getInsts()) {
    if (isSequentialMaster(inst->getMaster())
        && boundary_reg_set.find(inst) == boundary_reg_set.end()) {
      const int idx = hg.vertexIndex(odb::dbId<odb::dbInst>(inst->getId()));
      ASSERT_NE(idx, Hypergraph::kInvalidIndex);
      EXPECT_FALSE(is_reg[idx]) << inst->getName();
      ++internal_seq_checked;
    }
  }
  EXPECT_GT(internal_seq_checked, 0);
}

// ---------------------------------------------------------------------------
// 3. Custom io_input_ratio.
// ---------------------------------------------------------------------------

TEST(RentTest, CustomIoInputRatio)
{
  NetlistBuilder nb("customRatio");
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;
  spec.io_input_ratio = 0.75;

  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  ASSERT_TRUE(stats.engaged);
  EXPECT_NEAR(stats.T_in, std::lround(0.75 * stats.T_actual), 1);
}

// ---------------------------------------------------------------------------
// 4. Custom pin type distribution: all combinational.
// ---------------------------------------------------------------------------

TEST(RentTest, AllCombinationalPinTypeCreatesNoBoundaryCells)
{
  NetlistBuilder nb("allComb");
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;
  spec.io_pin_type_distribution =
      IoPinTypeDistribution{/*combinational=*/1.0, /*buffered=*/0.0,
                            /*registered=*/0.0};

  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  ASSERT_TRUE(stats.engaged);
  EXPECT_EQ(stats.n_buffered, 0);
  EXPECT_EQ(stats.n_registered, 0);
  EXPECT_EQ(stats.n_boundary_ff, 0);
  EXPECT_TRUE(stats.boundary_buf_insts.empty());
  EXPECT_TRUE(stats.boundary_reg_insts.empty());
  EXPECT_GT(stats.n_combinational, 0);

  Hypergraph hg;
  hg.buildFromBlock(nb.block());
  applyRentPlanes(hg, stats);
  const std::vector<bool>& is_buf = hg.vertexBoolPlane("hgm.is_boundary_buf");
  const std::vector<bool>& is_reg = hg.vertexBoolPlane("hgm.is_boundary_reg");
  EXPECT_TRUE(std::none_of(is_buf.begin(), is_buf.end(), [](bool v) { return v; }));
  EXPECT_TRUE(std::none_of(is_reg.begin(), is_reg.end(), [](bool v) { return v; }));
}

// ---------------------------------------------------------------------------
// 5. With sub-clusters.
// ---------------------------------------------------------------------------

TEST(RentTest, WithSubClusters)
{
  NetlistBuilder nb("withClusters");
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;
  spec.peak_avg_fanout = 12.0;
  spec.peak_cluster_pct = 0.15;
  spec.num_peak_clusters = 2;

  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  ASSERT_TRUE(stats.engaged);
  ASSERT_TRUE(stats.has_clusters);
  ASSERT_EQ(stats.cluster_rent.size(), 2u);
  EXPECT_EQ(stats.cluster_rent[0].cluster_idx, 0);
  EXPECT_EQ(stats.cluster_rent[1].cluster_idx, 1);
  for (const ClusterRentStats& cr : stats.cluster_rent) {
    EXPECT_GT(cr.G_c, 0);
    EXPECT_GT(cr.T_c, 0);
    EXPECT_TRUE(std::isfinite(cr.p_c));
  }
  ASSERT_TRUE(stats.background_valid);
  EXPECT_GT(stats.G_bg, 0);
  EXPECT_GT(stats.T_bg, 0);
  EXPECT_TRUE(std::isfinite(stats.p_bg));

  // Soft check only (brief: "warn if not") — peak clusters are more
  // densely/intensely wired, so their p tends to run higher than the
  // background's, but this is a statistical tendency, not a guarantee, and
  // must never fail the test on its own.
  for (const ClusterRentStats& cr : stats.cluster_rent) {
    if (!(cr.p_c > stats.p_bg)) {
      std::cerr << "[soft check] cluster " << cr.cluster_idx << " p_c="
                << cr.p_c << " did not exceed background p_bg=" << stats.p_bg
                << " (not a failure)\n";
    }
  }
}

// ---------------------------------------------------------------------------
// 6. Validation errors.
// ---------------------------------------------------------------------------

TEST(RentValidationTest, OnlyRentKPresentFails)
{
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(RentValidationTest, OnlyRentPPresentFails)
{
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_p = 0.6;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(RentValidationTest, RentPAboveHardLimitFails)
{
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 1.5;  // > 1.2, hard error
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(RentValidationTest, RentPInDegenerateRangeWarnsButPasses)
{
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 1.1;  // (1.0, 1.2]: accepted, clamped to 1.0 internally
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  EXPECT_DOUBLE_EQ(stats.rent_p_target, 1.0);
}

TEST(RentValidationTest, PinTypeDistributionSumOutOfRangeFails)
{
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.6;
  spec.io_pin_type_distribution =
      IoPinTypeDistribution{0.3, 0.1, 0.1};  // sums to 0.5
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(RentValidationTest, IoInputRatioOutOfRangeFails)
{
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.6;
  spec.io_input_ratio = 1.1;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

TEST(RentValidationTest, RentRequiresStatisticalMix)
{
  SyntheticNetlistSpec spec;
  spec.masters = {{"INV", 1, 1, 1.0}};
  spec.num_insts = 64;
  spec.rent_k = 2.5;
  spec.rent_p = 0.6;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));
}

// ---------------------------------------------------------------------------
// 7. Small design cap.
// ---------------------------------------------------------------------------

TEST(RentTest, SmallDesignCapsWithoutCrashing)
{
  NetlistBuilder nb("smallcap");
  SyntheticNetlistSpec spec = baseSpec();
  spec.num_insts = 10;
  spec.sequential_ratio = 0.5;
  spec.rent_k = 5.0;
  spec.rent_p = 0.8;

  RentStats stats;
  const int nets = generateSynthetic(nb, spec, nullptr, &stats);
  ASSERT_GE(nets, 0);
  ASSERT_TRUE(stats.engaged);
  EXPECT_TRUE(stats.capped);
  EXPECT_EQ(stats.T_in + stats.T_out, stats.T_actual);
}

}  // namespace
}  // namespace eda
