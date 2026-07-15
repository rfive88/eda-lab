// Tests for hg_metrics congestion metrics: vertex degree distribution,
// hyperedge size (fanout) distribution, and high-fanout net identification
// (Spike C1). All cases run on dbBlock-free hypergraphs built by
// Hypergraph::buildFromTopology — no data files needed.

#include <algorithm>
#include <vector>

#include "gtest/gtest.h"
#include "hg_metrics/congestion_metrics.h"
#include "hypergraph/hypergraph.h"

namespace hgm {
namespace {

using eda::Hypergraph;

TEST(CongestionMetricsTest, EmptyHypergraph)
{
  Hypergraph hg;

  EXPECT_TRUE(vertex_degree_histogram(hg).empty());
  EXPECT_TRUE(hyperedge_size_histogram(hg).empty());

  const DistributionStats vstats = vertex_degree_stats(hg);
  EXPECT_EQ(vstats.mean, 0.0);
  EXPECT_EQ(vstats.p90, 0.0);
  EXPECT_EQ(vstats.p99, 0.0);
  EXPECT_EQ(vstats.max, 0);

  const DistributionStats estats = hyperedge_size_stats(hg);
  EXPECT_EQ(estats.mean, 0.0);
  EXPECT_EQ(estats.max, 0);

  EXPECT_TRUE(high_fanout_nets(hg, 1).empty());
}

TEST(CongestionMetricsTest, SingleVertexNoHyperedges)
{
  Hypergraph hg;
  hg.buildFromTopology(1, {});

  const std::map<int, int> vhist = vertex_degree_histogram(hg);
  ASSERT_EQ(vhist.size(), 1u);
  EXPECT_EQ(vhist.at(0), 1);

  EXPECT_TRUE(hyperedge_size_histogram(hg).empty());
  EXPECT_TRUE(high_fanout_nets(hg, 1).empty());
}

TEST(CongestionMetricsTest, KnownSmallHypergraphSizeDistribution)
{
  // 4 vertices, 3 hyperedges of sizes 2, 3, 4.
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {0, 1, 2}, {0, 1, 2, 3}});

  const std::map<int, int> hist = hyperedge_size_histogram(hg);
  ASSERT_EQ(hist.size(), 3u);
  EXPECT_EQ(hist.at(2), 1);
  EXPECT_EQ(hist.at(3), 1);
  EXPECT_EQ(hist.at(4), 1);

  const DistributionStats stats = hyperedge_size_stats(hg);
  EXPECT_EQ(stats.max, 4);
  EXPECT_DOUBLE_EQ(stats.mean, 3.0);
  EXPECT_GE(stats.p90, 2.0);
  EXPECT_LE(stats.p90, 4.0);
  EXPECT_GE(stats.p99, 2.0);
  EXPECT_LE(stats.p99, 4.0);
}

TEST(CongestionMetricsTest, HighFanoutThreshold)
{
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {0, 1, 2}, {0, 1, 2, 3}});

  const std::vector<HyperedgeId> at_least_3 = high_fanout_nets(hg, 3);
  ASSERT_EQ(at_least_3.size(), 2u);
  EXPECT_NE(std::find(at_least_3.begin(), at_least_3.end(), 1),
           at_least_3.end());
  EXPECT_NE(std::find(at_least_3.begin(), at_least_3.end(), 2),
           at_least_3.end());

  EXPECT_TRUE(high_fanout_nets(hg, 99).empty());
}

TEST(CongestionMetricsTest, StarHypergraphDegreeDistribution)
{
  // Vertex 0 appears in every hyperedge; vertices 1-4 appear in exactly one.
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1}, {0, 2}, {0, 3}, {0, 4}});

  const std::map<int, int> hist = vertex_degree_histogram(hg);
  ASSERT_EQ(hist.size(), 2u);
  EXPECT_EQ(hist.at(1), 4);  // vertices 1-4, degree 1 each
  EXPECT_EQ(hist.at(4), 1);  // vertex 0, degree == numHyperedges()

  const DistributionStats stats = vertex_degree_stats(hg);
  EXPECT_EQ(stats.max, hg.numHyperedges());
}

}  // namespace
}  // namespace hgm
