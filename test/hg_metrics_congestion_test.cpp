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

// --- k-core decomposition (Spike C2) ---

TEST(KCoreTest, IsolatedVertices)
{
  // Four vertices, no hyperedges — every vertex is 0-degenerate.
  Hypergraph hg;
  hg.buildFromTopology(4, {});

  const int degeneracy = k_core_numbers(hg);
  EXPECT_EQ(degeneracy, 0);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  ASSERT_EQ(k_core.size(), 4u);
  for (const int c : k_core) {
    EXPECT_EQ(c, 0);
  }
}

TEST(KCoreTest, PathGraph)
{
  // Chain 0-1-2-3 via 2-pin hyperedges. A path is 1-degenerate: every vertex
  // (ends and interior alike) has k-core 1.
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {1, 2}, {2, 3}});

  const int degeneracy = k_core_numbers(hg);
  EXPECT_EQ(degeneracy, 1);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  ASSERT_EQ(k_core.size(), 4u);
  for (const int c : k_core) {
    EXPECT_EQ(c, 1);
  }
}

TEST(KCoreTest, Clique)
{
  // Four vertices, every pair a 2-pin hyperedge. A 4-clique is 3-degenerate:
  // all vertices have k-core 3.
  Hypergraph hg;
  hg.buildFromTopology(
      4, {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}});

  const int degeneracy = k_core_numbers(hg);
  EXPECT_EQ(degeneracy, 3);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  ASSERT_EQ(k_core.size(), 4u);
  for (const int c : k_core) {
    EXPECT_EQ(c, 3);
  }
}

TEST(KCoreTest, StarHyperedge)
{
  // A single 5-pin hyperedge. All five vertices share exactly one hyperedge,
  // so each has effective degree 1 and k-core 1.
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1, 2, 3, 4}});

  const int degeneracy = k_core_numbers(hg);
  EXPECT_EQ(degeneracy, 1);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  ASSERT_EQ(k_core.size(), 5u);
  for (const int c : k_core) {
    EXPECT_EQ(c, 1);
  }
}

TEST(KCoreTest, DenseCoreVsSparsePeriphery)
{
  // Vertices 0-3 form a 4-clique (2-pin edges); vertices 4,5 are pendant
  // vertices each attached to the core by a single 2-pin edge. Core vertices
  // must have strictly higher k-core than the periphery.
  Hypergraph hg;
  hg.buildFromTopology(6,
                       {{0, 1},
                        {0, 2},
                        {0, 3},
                        {1, 2},
                        {1, 3},
                        {2, 3},  // dense core
                        {0, 4},   // pendant 4
                        {1, 5}}); // pendant 5

  const int degeneracy = k_core_numbers(hg);
  EXPECT_EQ(degeneracy, 3);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  ASSERT_EQ(k_core.size(), 6u);

  const int core_min
      = std::min({k_core[0], k_core[1], k_core[2], k_core[3]});
  const int periphery_max = std::max(k_core[4], k_core[5]);
  EXPECT_GT(core_min, periphery_max);
  EXPECT_EQ(k_core[4], 1);
  EXPECT_EQ(k_core[5], 1);
  for (int v = 0; v < 4; ++v) {
    EXPECT_EQ(k_core[v], 3);
  }
}

TEST(KCoreTest, ReturnValueMatchesPlaneMax)
{
  Hypergraph hg;
  hg.buildFromTopology(
      6, {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}, {0, 4}, {1, 5}});

  const int degeneracy = k_core_numbers(hg);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  const int plane_max = *std::max_element(k_core.begin(), k_core.end());
  EXPECT_EQ(degeneracy, plane_max);
}

TEST(KCoreTest, OverwritesExistingPlane)
{
  // A second run on the same build must recompute cleanly, not accumulate.
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {1, 2}, {2, 3}});

  EXPECT_EQ(k_core_numbers(hg), 1);
  const int second = k_core_numbers(hg);
  EXPECT_EQ(second, 1);

  const std::vector<int>& k_core = hg.vertexIntPlane("hgm.k_core");
  for (const int c : k_core) {
    EXPECT_EQ(c, 1);
  }
}

// --- NESS neighborhood density (Spike C3) ---

TEST(OneHopNeighborhoodTest, StarHyperedge)
{
  // A single 5-pin hyperedge: every vertex shares it, so each has 4 distinct
  // 1-hop neighbors (itself excluded).
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1, 2, 3, 4}});

  one_hop_neighborhood_size(hg);
  const std::vector<int>& size = hg.vertexIntPlane("hgm.neighborhood_size_1hop");
  ASSERT_EQ(size.size(), 5u);
  for (const int s : size) {
    EXPECT_EQ(s, 4);
  }
}

TEST(OneHopNeighborhoodTest, DisjointHyperedges)
{
  // Two disjoint 3-pin hyperedges sharing no vertex: each vertex's 1-hop
  // neighborhood is its own hyperedge minus itself = 2.
  Hypergraph hg;
  hg.buildFromTopology(6, {{0, 1, 2}, {3, 4, 5}});

  one_hop_neighborhood_size(hg);
  const std::vector<int>& size = hg.vertexIntPlane("hgm.neighborhood_size_1hop");
  ASSERT_EQ(size.size(), 6u);
  for (const int s : size) {
    EXPECT_EQ(s, 2);
  }
}

TEST(OneHopNeighborhoodTest, IsolatedVertexScoresZero)
{
  // Vertex 3 is in no hyperedge -> no neighbors.
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1, 2}});

  one_hop_neighborhood_size(hg);
  const std::vector<int>& size = hg.vertexIntPlane("hgm.neighborhood_size_1hop");
  EXPECT_EQ(size[0], 2);
  EXPECT_EQ(size[1], 2);
  EXPECT_EQ(size[2], 2);
  EXPECT_EQ(size[3], 0);
}

TEST(NeighborhoodDensityTest, OneHopHalfDecay)
{
  // Star of 2-pin edges: v0 has degree 3, leaves v1..v3 degree 1 each.
  // h=1, alpha=0.5: A(u) = 0.5 * sum of degree over 1-hop neighbors.
  //   A(0) = 0.5 * (deg1 + deg2 + deg3) = 0.5 * 3 = 1.5
  //   A(leaf) = 0.5 * deg0 = 0.5 * 3 = 1.5
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {0, 2}, {0, 3}});

  neighborhood_density(hg, 0.5, 1);
  const std::vector<double>& d = hg.vertexDoublePlane("hgm.neighborhood_density");
  ASSERT_EQ(d.size(), 4u);
  EXPECT_DOUBLE_EQ(d[0], 1.5);
  EXPECT_DOUBLE_EQ(d[1], 1.5);
  EXPECT_DOUBLE_EQ(d[2], 1.5);
  EXPECT_DOUBLE_EQ(d[3], 1.5);
}

TEST(NeighborhoodDensityTest, AlphaZeroAndHZeroAreAllZeros)
{
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {0, 2}, {0, 3}});

  neighborhood_density(hg, 0.0, 2);
  {
    const std::vector<double>& d
        = hg.vertexDoublePlane("hgm.neighborhood_density");
    for (const double v : d) {
      EXPECT_DOUBLE_EQ(v, 0.0);
    }
  }

  neighborhood_density(hg, 0.5, 0);
  {
    const std::vector<double>& d
        = hg.vertexDoublePlane("hgm.neighborhood_density");
    for (const double v : d) {
      EXPECT_DOUBLE_EQ(v, 0.0);
    }
  }
}

TEST(NeighborhoodDensityTest, ChainMiddleDenserThanEnds)
{
  // Chain 0-1-2-3-4 via 2-pin edges. Degrees: 1,2,2,2,1.
  // h=2, alpha=0.5. Hand computation:
  //   A(2) = 0.5*(deg1+deg3) + 0.25*(deg0+deg4) = 0.5*4 + 0.25*2 = 2.5
  //   A(0) = 0.5*deg1        + 0.25*deg2        = 0.5*2 + 0.25*2 = 1.5
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}});

  neighborhood_density(hg, 0.5, 2);
  const std::vector<double>& d = hg.vertexDoublePlane("hgm.neighborhood_density");
  ASSERT_EQ(d.size(), 5u);
  EXPECT_DOUBLE_EQ(d[2], 2.5);
  EXPECT_DOUBLE_EQ(d[0], 1.5);
  EXPECT_DOUBLE_EQ(d[4], 1.5);
  EXPECT_GT(d[2], d[0]);
  EXPECT_GT(d[2], d[4]);
}

TEST(NetIntersectionScoreTest, SharedTripleMinusSelf)
{
  // e0={0,1,2,3}, e1={0,1,2,4}. Vertices 0,1,2 are incident to both edges;
  // for each the two nets intersect in {0,1,2} (size 3), minus self = 2.
  // Vertex 3 (only e0) and vertex 4 (only e1) have no pair -> 0. Vertex 5 is
  // isolated -> 0.
  Hypergraph hg;
  hg.buildFromTopology(6, {{0, 1, 2, 3}, {0, 1, 2, 4}});

  net_intersection_score(hg);
  const std::vector<int>& score = hg.vertexIntPlane("hgm.net_intersection_score");
  ASSERT_EQ(score.size(), 6u);
  EXPECT_EQ(score[0], 2);
  EXPECT_EQ(score[1], 2);
  EXPECT_EQ(score[2], 2);
  EXPECT_EQ(score[3], 0);  // incident to one hyperedge only
  EXPECT_EQ(score[4], 0);  // incident to one hyperedge only
  EXPECT_EQ(score[5], 0);  // no incident hyperedge
}

TEST(NessPlanesTest, AllThreePlanesExistWithCorrectShape)
{
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}});

  neighborhood_density(hg);
  one_hop_neighborhood_size(hg);
  net_intersection_score(hg);

  ASSERT_TRUE(hg.hasVertexPlane("hgm.neighborhood_density"));
  ASSERT_TRUE(hg.hasVertexPlane("hgm.neighborhood_size_1hop"));
  ASSERT_TRUE(hg.hasVertexPlane("hgm.net_intersection_score"));

  EXPECT_EQ(hg.vertexDoublePlane("hgm.neighborhood_density").size(), 5u);
  EXPECT_EQ(hg.vertexIntPlane("hgm.neighborhood_size_1hop").size(), 5u);
  EXPECT_EQ(hg.vertexIntPlane("hgm.net_intersection_score").size(), 5u);
}

}  // namespace
}  // namespace hgm
