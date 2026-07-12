// Tests for the netlistgen peak fanout sub-cluster feature (see
// docs/briefs/spike-netlistgen-peak-fanout-clusters.md): congestion
// hot-spot generation layered on top of the Stage B statistical mix and
// Stage D's acyclic-by-construction net formation. Needs no data files
// (synthetic mode only).
//
// Cluster membership is deliberately NOT part of the persistent
// dbBlock/Hypergraph model (per the brief) — generateSynthetic's optional
// `out_cluster_id` out-parameter and the directly-exposed
// `assignPeakClusters` helper exist purely so these tests can observe it
// without adding that bookkeeping to the data model.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gtest/gtest.h"

#include "engines/netlistgen/netlistgen.h"

#include "odb/db.h"

namespace eda {
namespace {

// Same DFS cycle-detection model as netlistgen_staged_test.cpp (kept
// self-contained per this repo's one-file-per-test-suite convention):
// combinational instances are nodes, edges follow driver -> receiver
// connectivity, sequential instances are cut at the D/Q boundary.
bool hasCombinationalCycle(odb::dbBlock* block)
{
  std::unordered_map<odb::dbInst*, int> node_id;
  int num_nodes = 0;
  for (odb::dbInst* inst : block->getInsts()) {
    if (!isSequentialMaster(inst->getMaster())) {
      node_id.emplace(inst, num_nodes++);
    }
  }

  std::vector<std::vector<int>> adj(num_nodes);
  for (odb::dbNet* net : block->getNets()) {
    int driver = -1;
    std::vector<int> receivers;
    for (odb::dbITerm* iterm : net->getITerms()) {
      const odb::dbSigType st = iterm->getSigType();
      if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
        continue;
      }
      const auto it = node_id.find(iterm->getInst());
      if (it == node_id.end()) {
        continue;
      }
      if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
        driver = it->second;
      } else {
        receivers.push_back(it->second);
      }
    }
    if (driver >= 0) {
      for (const int r : receivers) {
        adj[driver].push_back(r);
      }
    }
  }

  std::vector<char> color(num_nodes, 0);
  std::vector<std::pair<int, size_t>> stack;
  for (int s = 0; s < num_nodes; ++s) {
    if (color[s] != 0) {
      continue;
    }
    color[s] = 1;
    stack.emplace_back(s, 0);
    while (!stack.empty()) {
      const int u = stack.back().first;
      const size_t next = stack.back().second;
      if (next < adj[u].size()) {
        ++stack.back().second;
        const int v = adj[u][next];
        if (color[v] == 1) {
          return true;
        }
        if (color[v] == 0) {
          color[v] = 1;
          stack.emplace_back(v, 0);
        }
      } else {
        color[u] = 2;
        stack.pop_back();
      }
    }
  }
  return false;
}

SyntheticNetlistSpec basePeakSpec()
{
  SyntheticNetlistSpec spec;
  spec.num_insts = 2000;
  spec.sequential_ratio = 0.1;
  spec.combinational_pin_distribution =
      std::array<double, 5>{20, 20, 20, 20, 20};
  spec.min_fanout = 2;
  spec.max_fanout = 5;  // background average fanout = 3.5
  spec.seed = 21;
  return spec;
}

// ---------------------------------------------------------------------------
// 1. No peak params: unaffected. (Full-suite regression is covered by the
// existing netlistgen_test / _stageb_test / _stagec_test / _staged_test
// suites all passing unchanged; this is a direct sanity check that a spec
// with no peak fields set never engages clustering.)
// ---------------------------------------------------------------------------

TEST(PeakClusterTest, NoPeakParamsLeavesClusterIdEmpty)
{
  NetlistBuilder nb("nopeak");
  const SyntheticNetlistSpec spec = basePeakSpec();  // no peak_* fields
  std::vector<int> cluster_id;
  const int nets = generateSynthetic(nb, spec, &cluster_id);
  ASSERT_GT(nets, 0);
  EXPECT_TRUE(cluster_id.empty());
}

// ---------------------------------------------------------------------------
// 2. Basic cluster generation.
// ---------------------------------------------------------------------------

TEST(PeakClusterTest, BasicClusterGeneration)
{
  NetlistBuilder nb("peakbasic");
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_avg_fanout = 12.0;
  spec.peak_cluster_pct = 0.15;
  spec.num_peak_clusters = 1;

  std::vector<int> cluster_id;
  const int nets = generateSynthetic(nb, spec, &cluster_id);
  ASSERT_GT(nets, 0);
  EXPECT_EQ(static_cast<int>(nb.block()->getInsts().size()), spec.num_insts);
  ASSERT_EQ(static_cast<int>(cluster_id.size()), spec.num_insts);

  // Stage D still produces a valid DAG (no crash, no cycle) even though
  // cluster-preferred sink selection is new attack surface for one.
  EXPECT_FALSE(hasCombinationalCycle(nb.block()));

  // Clustered instance count matches the brief's formula.
  const int expected_cluster_size =
      static_cast<int>(std::floor(*spec.peak_cluster_pct * spec.num_insts
                                  / *spec.num_peak_clusters));
  int clustered = 0;
  for (int c : cluster_id) {
    if (c >= 0) {
      EXPECT_EQ(c, 0);  // only cluster 0 exists
      ++clustered;
    }
  }
  EXPECT_EQ(clustered, expected_cluster_size);

  // Map instance name ("u<i>") -> cluster id, then bucket nets by whether
  // their driver instance is clustered.
  auto instIndex = [](odb::dbInst* inst) {
    return std::atoi(inst->getName().c_str() + 1);
  };
  long cluster_fanout_sum = 0;
  int cluster_net_count = 0;
  long global_fanout_sum = 0;
  int global_net_count = 0;
  for (odb::dbNet* net : nb.block()->getNets()) {
    odb::dbITerm* driver = nullptr;
    int sinks = 0;
    for (odb::dbITerm* iterm : net->getITerms()) {
      if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
        driver = iterm;
      } else {
        ++sinks;
      }
    }
    ASSERT_NE(driver, nullptr) << net->getName();
    ++global_net_count;
    global_fanout_sum += sinks;
    if (cluster_id[instIndex(driver->getInst())] >= 0) {
      ++cluster_net_count;
      cluster_fanout_sum += sinks;
    }
  }
  ASSERT_GT(cluster_net_count, 0);
  const double cluster_avg =
      static_cast<double>(cluster_fanout_sum) / cluster_net_count;
  const double global_avg =
      static_cast<double>(global_fanout_sum) / global_net_count;

  EXPECT_GT(cluster_avg, global_avg)
      << "cluster avg " << cluster_avg << " global avg " << global_avg;
  // Within 20% of the requested peak_avg_fanout (small-test-size tolerance).
  EXPECT_NEAR(cluster_avg, *spec.peak_avg_fanout,
              0.20 * *spec.peak_avg_fanout);
}

// ---------------------------------------------------------------------------
// 3. Multiple clusters.
// ---------------------------------------------------------------------------

TEST(PeakClusterTest, MultipleClustersDistinctAndSized)
{
  std::mt19937 rng(99);
  const int num_insts = 3000;
  const double pct = 0.15;
  const int num_clusters = 3;
  const std::vector<int> cluster_id =
      assignPeakClusters(num_insts, pct, num_clusters, rng);
  ASSERT_EQ(static_cast<int>(cluster_id.size()), num_insts);

  const int expected_size =
      static_cast<int>(std::floor(pct * num_insts / num_clusters));
  std::array<int, 3> counts{};
  std::unordered_set<int> seen_ids;
  for (int c : cluster_id) {
    ASSERT_GE(c, -1);
    ASSERT_LT(c, num_clusters);
    if (c >= 0) {
      ++counts[c];
      seen_ids.insert(c);
    }
  }
  // Three distinct cluster groups actually got assigned.
  EXPECT_EQ(seen_ids.size(), 3u);
  for (int c = 0; c < num_clusters; ++c) {
    EXPECT_NEAR(counts[c], expected_size, 1) << "cluster " << c;
  }
}

TEST(PeakClusterTest, ClusterAssignmentDeterministicForSeed)
{
  auto run = [](uint32_t seed) {
    std::mt19937 rng(seed);
    return assignPeakClusters(500, 0.15, 2, rng);
  };
  EXPECT_EQ(run(5), run(5));
  EXPECT_NE(run(5), run(6));
}

TEST(PeakClusterTest, GenerateSyntheticThreadsClusterIdForMultipleClusters)
{
  NetlistBuilder nb("peakmulti");
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_avg_fanout = 10.0;
  spec.peak_cluster_pct = 0.15;
  spec.num_peak_clusters = 3;

  std::vector<int> cluster_id;
  const int nets = generateSynthetic(nb, spec, &cluster_id);
  ASSERT_GT(nets, 0);
  EXPECT_FALSE(hasCombinationalCycle(nb.block()));

  std::unordered_set<int> seen_ids;
  for (int c : cluster_id) {
    if (c >= 0) {
      seen_ids.insert(c);
    }
  }
  EXPECT_EQ(seen_ids.size(), 3u);
}

// ---------------------------------------------------------------------------
// 4. Validation errors.
// ---------------------------------------------------------------------------

TEST(PeakClusterTest, PeakBelowBackgroundAverageFails)
{
  SyntheticNetlistSpec spec = basePeakSpec();  // background avg = 3.5
  spec.peak_avg_fanout = 3.0;                  // <= background average
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(PeakClusterTest, PeakClusterPctOutOfRangeFails)
{
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_avg_fanout = 12.0;
  spec.peak_cluster_pct = 1.5;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(PeakClusterTest, NumPeakClustersZeroFails)
{
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_avg_fanout = 12.0;
  spec.num_peak_clusters = 0;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

TEST(PeakClusterTest, PeakParamsIgnoredWhenPeakAvgFanoutAbsent)
{
  // peak_cluster_pct out of range and num_peak_clusters == 0 would each fail
  // validation on their own, but with peak_avg_fanout unset they must be
  // ignored entirely (brief rule 4).
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_cluster_pct = 1.5;
  spec.num_peak_clusters = 0;
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));
}

TEST(PeakClusterTest, PeakAvgFanoutRequiresStatisticalMix)
{
  // Legacy weighted mix has no per-instance sequential/combinational
  // classification to build cluster-safe eligibility from.
  SyntheticNetlistSpec spec;
  spec.masters = {{"INV", 1, 1, 1.0}};
  spec.num_insts = 64;
  spec.peak_avg_fanout = 12.0;
  EXPECT_FALSE(validateSpecConfig(spec, nullptr));

  NetlistBuilder nb;
  EXPECT_EQ(generateSynthetic(nb, spec), -1);
}

// ---------------------------------------------------------------------------
// 5. Default num_peak_clusters.
// ---------------------------------------------------------------------------

TEST(PeakClusterTest, DefaultNumPeakClustersIsOne)
{
  NetlistBuilder nb("peakdefault");
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_avg_fanout = 12.0;
  spec.peak_cluster_pct = 0.15;
  // num_peak_clusters omitted -> defaults to 1.
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));

  std::vector<int> cluster_id;
  const int nets = generateSynthetic(nb, spec, &cluster_id);
  ASSERT_GT(nets, 0);
  for (int c : cluster_id) {
    EXPECT_LE(c, 0);  // -1 (background) or 0 (the single default cluster)
  }
}

// Same check via the JSON CLI config path (peak_cluster_pct present,
// num_peak_clusters absent).
TEST(PeakClusterTest, DefaultPeakClusterPctViaValidation)
{
  // peak_avg_fanout set, peak_cluster_pct omitted -> the 0.10 default is
  // applied at generation time (buildPlan / formNetsAcyclic), not asserted
  // by validateSpecConfig itself (which only range-checks an explicit
  // value) — confirm the unset case still validates and generates.
  NetlistBuilder nb("peakpctdefault");
  SyntheticNetlistSpec spec = basePeakSpec();
  spec.peak_avg_fanout = 12.0;
  // peak_cluster_pct and num_peak_clusters both omitted.
  EXPECT_TRUE(validateSpecConfig(spec, nullptr));

  std::vector<int> cluster_id;
  const int nets = generateSynthetic(nb, spec, &cluster_id);
  ASSERT_GT(nets, 0);
  const int expected_cluster_size =
      static_cast<int>(std::floor(0.10 * spec.num_insts / 1));
  int clustered = 0;
  for (int c : cluster_id) {
    if (c >= 0) {
      ++clustered;
    }
  }
  EXPECT_EQ(clustered, expected_cluster_size);
}

}  // namespace
}  // namespace eda
