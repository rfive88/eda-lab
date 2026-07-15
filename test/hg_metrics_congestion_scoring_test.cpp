// Tests for hg_metrics composite congestion scoring (Spike C5):
// score_congestion (percentile-blend of the C2/C3/C4 planes onto a 1-5 score)
// and find_congestion_clusters (BFS connected components over high-score
// vertices). The three required input planes are populated by hand via
// vertexIntPlane/vertexDoublePlane, bypassing the C2/C3/C4 functions, so every
// case is self-contained and needs no data files.

#include <algorithm>
#include <vector>

#include "gtest/gtest.h"
#include "hg_metrics/congestion_scoring.h"
#include "hypergraph/hypergraph.h"

namespace hgm {
namespace {

using eda::Hypergraph;

// Populate the three required input planes on `hg` from explicit per-vertex
// values. Sizes must equal hg.numVertices().
void set_input_planes(Hypergraph& hg, const std::vector<int>& kcore,
                      const std::vector<double>& density,
                      const std::vector<double>& tangle)
{
  std::vector<int>& kc = hg.vertexIntPlane("hgm.k_core");
  std::vector<double>& nd = hg.vertexDoublePlane("hgm.neighborhood_density");
  std::vector<double>& ts = hg.vertexDoublePlane("hgm.tangle_score");
  for (int v = 0; v < hg.numVertices(); ++v) {
    kc[v] = kcore[v];
    nd[v] = density[v];
    ts[v] = tangle[v];
  }
}

// --- score_congestion ---

TEST(ScoreCongestionTest, MissingPlaneReturnsErrorAndWritesNothing)
{
  Hypergraph hg;
  hg.buildFromTopology(3, {{0, 1, 2}});
  // Only two of the three required planes exist.
  hg.vertexDoublePlane("hgm.neighborhood_density");
  hg.vertexDoublePlane("hgm.tangle_score");

  CongestionReport report;
  const eda::Status status = score_congestion(hg, report);
  EXPECT_FALSE(status.ok());
  EXPECT_FALSE(hg.hasVertexPlane("hgm.congestion_score"));
}

TEST(ScoreCongestionTest, WeightsMustSumToOne)
{
  Hypergraph hg;
  hg.buildFromTopology(3, {{0, 1, 2}});
  set_input_planes(hg, {1, 2, 3}, {1.0, 2.0, 3.0}, {0.1, 0.2, 0.3});

  CongestionReport report;
  CongestionWeights bad;  // sums to 1.3
  bad.k_core_weight = 0.5;
  bad.neighborhood_weight = 0.4;
  bad.tangle_weight = 0.4;
  const eda::Status status = score_congestion(hg, report, bad);
  EXPECT_FALSE(status.ok());
  EXPECT_FALSE(hg.hasVertexPlane("hgm.congestion_score"));
}

TEST(ScoreCongestionTest, KnownPercentileToScoreMapping)
{
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1, 2, 3, 4}});
  set_input_planes(hg, {1, 2, 3, 4, 5}, {1.0, 2.0, 3.0, 4.0, 5.0},
                   {0.1, 0.3, 0.5, 0.7, 0.9});

  CongestionReport report;
  const eda::Status status = score_congestion(hg, report);
  ASSERT_TRUE(status.ok());

  const std::vector<int>& score = hg.vertexIntPlane("hgm.congestion_score");
  EXPECT_EQ(score[0], 1);
  EXPECT_EQ(score[1], 2);
  EXPECT_EQ(score[2], 3);
  EXPECT_EQ(score[3], 4);
  EXPECT_EQ(score[4], 5);

  EXPECT_EQ(report.score_histogram[1], 1);
  EXPECT_EQ(report.score_histogram[2], 1);
  EXPECT_EQ(report.score_histogram[3], 1);
  EXPECT_EQ(report.score_histogram[4], 1);
  EXPECT_EQ(report.score_histogram[5], 1);
}

TEST(ScoreCongestionTest, AllIdenticalValuesScoreMidpoint)
{
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1, 2, 3}});
  set_input_planes(hg, {7, 7, 7, 7}, {2.5, 2.5, 2.5, 2.5},
                   {0.4, 0.4, 0.4, 0.4});

  CongestionReport report;
  ASSERT_TRUE(score_congestion(hg, report).ok());

  const std::vector<int>& score = hg.vertexIntPlane("hgm.congestion_score");
  for (int v = 0; v < 4; ++v) {
    EXPECT_EQ(score[v], 3) << "vertex " << v;
  }
  EXPECT_EQ(report.score_histogram[3], 4);
}

TEST(ScoreCongestionTest, ScopedScoringWritesOnlyScopeVertices)
{
  Hypergraph hg;
  hg.buildFromTopology(6, {{0, 1, 2, 3, 4, 5}});
  set_input_planes(hg, {10, 20, 1, 3, 5, 40},
                   {10.0, 20.0, 1.0, 3.0, 5.0, 40.0},
                   {0.9, 0.8, 0.1, 0.3, 0.5, 0.95});

  CongestionReport report;
  const std::vector<int> scope = {2, 3, 4};
  ASSERT_TRUE(score_congestion(hg, report, {}, scope).ok());

  ASSERT_TRUE(hg.hasVertexPlane("hgm.congestion_score"));
  const std::vector<int>& score = hg.vertexIntPlane("hgm.congestion_score");
  // Within the scope {2,3,4}: vertex 2 is lowest -> score 1, vertex 3 mid ->
  // score 3, vertex 4 highest -> score 5.
  EXPECT_EQ(score[2], 1);
  EXPECT_EQ(score[3], 3);
  EXPECT_EQ(score[4], 5);
  // Out-of-scope vertices are untouched -> default 0 (plane freshly created).
  EXPECT_EQ(score[0], 0);
  EXPECT_EQ(score[1], 0);
  EXPECT_EQ(score[5], 0);
  EXPECT_EQ(report.num_vertices_scored, 3);
}

TEST(ScoreCongestionTest, CustomWeightsShiftScore)
{
  // Vertex 4: k_core in the top percentile, density/tangle in the bottom.
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1, 2, 3, 4}});
  set_input_planes(hg, {1, 2, 3, 4, 5},          // vertex 4 -> k_core rank 1.0
                   {5.0, 4.0, 3.0, 2.0, 1.0},    // vertex 4 -> density rank 0.0
                   {0.9, 0.7, 0.5, 0.3, 0.1});   // vertex 4 -> tangle rank 0.0

  CongestionReport equal_report;
  ASSERT_TRUE(score_congestion(hg, equal_report).ok());
  const int equal_score = hg.vertexIntPlane("hgm.congestion_score")[4];

  CongestionWeights kcore_heavy;
  kcore_heavy.k_core_weight = 0.6;
  kcore_heavy.neighborhood_weight = 0.2;
  kcore_heavy.tangle_weight = 0.2;
  CongestionReport weighted_report;
  ASSERT_TRUE(score_congestion(hg, weighted_report, kcore_heavy).ok());
  const int weighted_score = hg.vertexIntPlane("hgm.congestion_score")[4];

  EXPECT_GT(weighted_score, equal_score);
}

TEST(ScoreCongestionTest, SingleVertexScopeScoresMidpoint)
{
  Hypergraph hg;
  hg.buildFromTopology(3, {{0, 1, 2}});
  set_input_planes(hg, {5, 2, 9}, {5.0, 2.0, 9.0}, {0.5, 0.2, 0.9});

  CongestionReport report;
  const std::vector<int> scope = {0};
  ASSERT_TRUE(score_congestion(hg, report, {}, scope).ok());
  // Sole member of its scope -> percentile 0.5 for every metric -> score 3.
  EXPECT_EQ(hg.vertexIntPlane("hgm.congestion_score")[0], 3);
  EXPECT_EQ(report.num_vertices_scored, 1);
}

TEST(ScoreCongestionTest, ReportSummaryFields)
{
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1, 2, 3, 4}});
  set_input_planes(hg, {1, 2, 3, 4, 5}, {1.0, 2.0, 3.0, 4.0, 5.0},
                   {0.1, 0.3, 0.5, 0.7, 0.9});

  CongestionReport report;
  ASSERT_TRUE(score_congestion(hg, report).ok());

  EXPECT_EQ(report.num_vertices_scored, 5);
  EXPECT_NEAR(report.mean_composite, 0.5, 1e-9);
  int histogram_sum = 0;
  for (const auto& [band, count] : report.score_histogram) {
    histogram_sum += count;
  }
  EXPECT_EQ(histogram_sum, 5);
}

// --- find_congestion_clusters ---

// Directly write a "hgm.congestion_score" int plane, bypassing score_congestion.
void set_scores(Hypergraph& hg, const std::vector<int>& scores)
{
  std::vector<int>& plane = hg.vertexIntPlane("hgm.congestion_score");
  for (int v = 0; v < hg.numVertices(); ++v) {
    plane[v] = scores[v];
  }
}

TEST(FindClustersTest, SingleClusterAllAdjacent)
{
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1, 2, 3}});
  set_scores(hg, {5, 5, 5, 5});

  const std::vector<CongestionCluster> clusters = find_congestion_clusters(hg);
  ASSERT_EQ(clusters.size(), 1u);
  EXPECT_EQ(clusters[0].peak_score, 5);
  EXPECT_EQ(clusters[0].size, 4);
}

TEST(FindClustersTest, TwoIsolatedClusters)
{
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {2, 3}});
  set_scores(hg, {3, 4, 5, 3});

  const std::vector<CongestionCluster> clusters = find_congestion_clusters(hg);
  ASSERT_EQ(clusters.size(), 2u);
}

TEST(FindClustersTest, MinScoreFilter)
{
  // Chain 0-1-2-3-4 with scores 1..5. min_score=4 -> only {3,4} eligible, and
  // they share edge {3,4} -> one cluster of size 2.
  Hypergraph hg;
  hg.buildFromTopology(5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}});
  set_scores(hg, {1, 2, 3, 4, 5});

  const std::vector<CongestionCluster> clusters =
      find_congestion_clusters(hg, /*min_score=*/4);
  ASSERT_EQ(clusters.size(), 1u);
  EXPECT_EQ(clusters[0].size, 2);
  EXPECT_EQ(clusters[0].peak_score, 5);
  EXPECT_EQ(clusters[0].members,
            (std::vector<int>{3, 4}));  // BFS from seed 3
}

TEST(FindClustersTest, SortOrderPeakThenSize)
{
  // Three disjoint components:
  //   A: 6 vertices, all score 5  (peak 5, size 6)
  //   B: 2 vertices, all score 5  (peak 5, size 2)
  //   C: 4 vertices, all score 3  (peak 3, size 4)
  // Expected order: (peak5,size6), (peak5,size2), (peak3,size4).
  Hypergraph hg;
  hg.buildFromTopology(
      12, {{0, 1, 2, 3, 4, 5}, {6, 7}, {8, 9, 10, 11}});
  set_scores(hg, {5, 5, 5, 5, 5, 5, 5, 5, 3, 3, 3, 3});

  const std::vector<CongestionCluster> clusters = find_congestion_clusters(hg);
  ASSERT_EQ(clusters.size(), 3u);
  EXPECT_EQ(clusters[0].peak_score, 5);
  EXPECT_EQ(clusters[0].size, 6);
  EXPECT_EQ(clusters[1].peak_score, 5);
  EXPECT_EQ(clusters[1].size, 2);
  EXPECT_EQ(clusters[2].peak_score, 3);
  EXPECT_EQ(clusters[2].size, 4);
}

TEST(FindClustersTest, ScopeRestrictsMembership)
{
  // Vertices 3,4,5 are high-score, but only 4,5 are in scope. Vertex 3 must not
  // appear in any cluster.
  Hypergraph hg;
  hg.buildFromTopology(6, {{3, 4, 5}, {0, 1, 2}});
  set_scores(hg, {1, 1, 1, 5, 5, 5});

  const std::vector<int> scope = {4, 5};
  const std::vector<CongestionCluster> clusters =
      find_congestion_clusters(hg, /*min_score=*/1, scope);
  for (const CongestionCluster& c : clusters) {
    EXPECT_EQ(std::count(c.members.begin(), c.members.end(), 3), 0);
  }
  // The eligible set is exactly {4,5}, adjacent via edge {3,4,5} -> one cluster.
  ASSERT_EQ(clusters.size(), 1u);
  EXPECT_EQ(clusters[0].size, 2);
}

TEST(EmptyHypergraphTest, BothFunctionsReturnEmpty)
{
  Hypergraph hg;  // no vertices
  // Create the required (empty) input planes so score_congestion's plane guard
  // passes; the scope is then empty and nothing is scored.
  hg.vertexIntPlane("hgm.k_core");
  hg.vertexDoublePlane("hgm.neighborhood_density");
  hg.vertexDoublePlane("hgm.tangle_score");

  CongestionReport report;
  EXPECT_TRUE(score_congestion(hg, report).ok());
  EXPECT_EQ(report.num_vertices_scored, 0);
  EXPECT_TRUE(report.clusters.empty());

  EXPECT_TRUE(find_congestion_clusters(hg).empty());
}

}  // namespace
}  // namespace hgm
