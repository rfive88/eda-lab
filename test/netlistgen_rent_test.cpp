// Tests for the netlistgen Stage E1 feature: primary I/O port generation
// governed by Rent's rule (see docs/briefs/spike-netlistgen-E1-io-rent.md).
// Needs no data files (synthetic mode only).
//
// Deliberate deviations from a literal reading of the brief, all documented
// in README.md's "Primary I/O generation (Stage E1)" section and exercised
// directly here:
//   - PI/PO never touch a live driver at all (this codebase treats both
//     "exactly one driver per net" AND "no dangling instances" as hard,
//     strict invariants — confirmed explicitly before implementing, after
//     an earlier revision of this feature that disconnected an existing
//     net's driver for PI was found to leave that driver's instance
//     dangling in some cases). PI targets Stage D's own leftover,
//     never-connected internal input pins first, falling back to stealing
//     a non-last sink of an already-multi-sink net (never dangling that
//     net's driver, since >=1 sink always remains) only if that pool runs
//     dry. PO prefers claiming a leftover, never-connected internal output
//     pin (repairing what would otherwise be a dead-output instance),
//     falling back to adding one more sink onto any existing net (always
//     safe) once exhausted. `NoDanglingInstancesAfterE1` below is the
//     direct correctness test for this.
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
#include <cstdint>
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

// Per-instance connectivity tally used by the strict "no dangling
// instances" checks below. `fully_isolated`: instances with zero connected
// pins at all. `dead_output`: instances with >=1 output pin, none
// connected — "useless" in the sense the design decision behind Stage E1's
// PI/PO wiring is built around (an instance that drives nothing).
struct ConnectivityTally
{
  int fully_isolated = 0;
  int dead_output = 0;
};

ConnectivityTally tallyConnectivity(odb::dbBlock* block)
{
  ConnectivityTally t;
  for (odb::dbInst* inst : block->getInsts()) {
    int connected = 0;
    int outputs = 0;
    int connected_outputs = 0;
    for (odb::dbITerm* it : inst->getITerms()) {
      const odb::dbSigType st = it->getSigType();
      if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
        continue;
      }
      if (it->getNet() != nullptr) {
        ++connected;
      }
      if (it->getIoType() == odb::dbIoType::OUTPUT) {
        ++outputs;
        if (it->getNet() != nullptr) {
          ++connected_outputs;
        }
      }
    }
    if (connected == 0) {
      ++t.fully_isolated;
    } else if (outputs > 0 && connected_outputs == 0) {
      ++t.dead_output;
    }
  }
  return t;
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
  // Explicit mixed distribution: the default is all-combinational (no
  // boundary cells — see DefaultPinTypeDistributionIsAllCombinational), so
  // this test's boundary buf/FF plane assertions need a mix that creates
  // some of each.
  spec.io_pin_type_distribution =
      IoPinTypeDistribution{/*combinational=*/0.70, /*buffered=*/0.20,
                            /*registered=*/0.10};

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

  // Strict rules: no multiply-driven nets (validateNetlist, now bTerm-aware)
  // and no dangling instances (see NoDanglingInstancesAfterE1 for the
  // detailed, pre/post-E1 comparison version of this check).
  const NetlistValidation v = validateNetlist(nb.block());
  EXPECT_TRUE(v.ok) << v.message;
  EXPECT_EQ(tallyConnectivity(nb.block()).fully_isolated, 0);

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

// An UNSET io_pin_type_distribution defaults to {combinational: 1.0,
// buffered: 0.0, registered: 0.0} (the E1 brief's default): every port is
// a bare bTerm and no boundary buf/FF cell is ever created. Pins the
// default itself, not just the explicit all-combinational config above.
TEST(RentTest, DefaultPinTypeDistributionIsAllCombinational)
{
  NetlistBuilder nb("defaultDist");
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;
  ASSERT_FALSE(spec.io_pin_type_distribution.has_value());

  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, nullptr, &stats), 0);
  ASSERT_TRUE(stats.engaged);
  EXPECT_GT(stats.n_combinational, 0);
  EXPECT_EQ(stats.n_combinational, stats.T_actual);
  EXPECT_EQ(stats.n_buffered, 0);
  EXPECT_EQ(stats.n_registered, 0);
  EXPECT_EQ(stats.n_boundary_ff, 0);
  EXPECT_TRUE(stats.boundary_buf_insts.empty());
  EXPECT_TRUE(stats.boundary_reg_insts.empty());
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
}

// ---------------------------------------------------------------------------
// Strict rule: Stage E1's PI/PO wiring must never create a dangling
// instance (one whose output drives nothing), and must never increase the
// count of Stage D's own pre-existing dead-output instances — it may only
// ever *repair* some of them (PO claiming a leftover output pin). Checked
// across several instance counts, seeds, and pin-type distributions.
// ---------------------------------------------------------------------------

TEST(RentTest, NoDanglingInstancesAfterE1)
{
  for (const int num_insts : {50, 500, 3000}) {
    for (const uint32_t seed : {1u, 7u, 42u}) {
      SyntheticNetlistSpec base = baseSpec();
      base.num_insts = num_insts;
      base.seed = seed;

      NetlistBuilder before("before");
      ASSERT_GT(generateSynthetic(before, base), 0)
          << "insts " << num_insts << " seed " << seed;
      const ConnectivityTally pre = tallyConnectivity(before.block());
      ASSERT_EQ(pre.fully_isolated, 0)
          << "Stage D itself produced an isolated instance — unexpected";

      NetlistBuilder after("after");
      SyntheticNetlistSpec spec = base;
      spec.rent_k = 2.5;
      spec.rent_p = 0.60;
      RentStats stats;
      ASSERT_GT(generateSynthetic(after, spec, nullptr, &stats), 0)
          << "insts " << num_insts << " seed " << seed;
      ASSERT_TRUE(stats.engaged);

      const ConnectivityTally post = tallyConnectivity(after.block());
      EXPECT_EQ(post.fully_isolated, 0)
          << "insts " << num_insts << " seed " << seed
          << ": E1 introduced a dangling instance";
      EXPECT_LE(post.dead_output, pre.dead_output)
          << "insts " << num_insts << " seed " << seed
          << ": E1 increased the dead-output-instance count instead of "
             "only ever repairing it";

      EXPECT_TRUE(validateNetlist(after.block()).ok)
          << "insts " << num_insts << " seed " << seed;
    }
  }
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
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
  EXPECT_EQ(tallyConnectivity(nb.block()).fully_isolated, 0);
  ASSERT_TRUE(stats.has_clusters);
  ASSERT_EQ(stats.cluster_rent.size(), 2u);
  EXPECT_EQ(stats.cluster_rent[0].cluster_idx, 0);
  EXPECT_EQ(stats.cluster_rent[1].cluster_idx, 1);
  for (const ClusterRentStats& cr : stats.cluster_rent) {
    EXPECT_GT(cr.G_c, 0);
    EXPECT_GT(cr.T_c, 0);
    EXPECT_TRUE(std::isfinite(cr.p_c));
    EXPECT_GT(cr.avg_fanout_c, 0.0);
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

// avg_fanout_c independently recomputed from raw block data, per the exact
// membership rule: a net counts toward cluster c's average if AT LEAST ONE
// of its connected iterms is owned by an instance with cluster_id == c
// (broader than T_c's cut-net rule — no requirement the net also reach
// outside the cluster). Fanout itself is load iterms, driver excluded,
// matching reportDesignSummary's convention (bTerms not counted).
TEST(RentTest, ClusterAvgFanoutMatchesIndependentRecomputation)
{
  NetlistBuilder nb("clusterFanout");
  SyntheticNetlistSpec spec = baseSpec();
  spec.rent_k = 2.5;
  spec.rent_p = 0.60;
  spec.peak_avg_fanout = 12.0;
  spec.peak_cluster_pct = 0.15;
  spec.num_peak_clusters = 2;

  std::vector<int> cluster_id;
  RentStats stats;
  ASSERT_GT(generateSynthetic(nb, spec, &cluster_id, &stats), 0);
  ASSERT_TRUE(stats.has_clusters);
  ASSERT_FALSE(stats.cluster_rent.empty());

  auto instClusterOf = [&](odb::dbInst* inst) -> int {
    const int idx = std::atoi(inst->getName().c_str() + 1);
    return idx < static_cast<int>(cluster_id.size()) ? cluster_id[idx] : -1;
  };

  for (const ClusterRentStats& cr : stats.cluster_rent) {
    long fanout_sum = 0;
    int net_count = 0;
    for (odb::dbNet* net : nb.block()->getNets()) {
      bool in_cluster = false;
      int sinks = 0;
      for (odb::dbITerm* it : net->getITerms()) {
        if (instClusterOf(it->getInst()) == cr.cluster_idx) {
          in_cluster = true;
        }
        if (it->getIoType() != odb::dbIoType::OUTPUT) {
          ++sinks;
        }
      }
      if (in_cluster) {
        fanout_sum += sinks;
        ++net_count;
      }
    }
    ASSERT_GT(net_count, 0) << "cluster " << cr.cluster_idx;
    const double expected =
        static_cast<double>(fanout_sum) / net_count;
    EXPECT_NEAR(cr.avg_fanout_c, expected, 1e-9) << "cluster " << cr.cluster_idx;
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
  EXPECT_TRUE(validateNetlist(nb.block()).ok);
  EXPECT_EQ(tallyConnectivity(nb.block()).fully_isolated, 0);
}

}  // namespace
}  // namespace eda
