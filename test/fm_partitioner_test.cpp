// Tests for the partitioning engine: the procedural random hypergraph
// generator and the flat K-way FM partitioner (Stage 1 = 2-way spanning
// cut, Stage 2 = K-way connectivity-1). Only the FMOdbTest cases touch
// LEF/DEF data; everything else runs on dbBlock-free hypergraphs (that
// being the point of buildFromTopology).

#include <algorithm>
#include <set>
#include <vector>

#include "engines/partitioning/fm_partitioner.h"
#include "engines/partitioning/random_hypergraph.h"
#include "gtest/gtest.h"
#include "hypergraph/hypergraph.h"
#include "odb/db.h"
#include "odb/defin.h"
#include "odb/lefin.h"
#include "utl/Logger.h"

namespace eda {
namespace {

// Reference weighted-cut evaluation, independent of the engine's
// incremental bookkeeping: sum of weights of hyperedges with pins in both
// parts ("weight" plane if present, else 1.0 per edge).
double computeCut(const Hypergraph& hg, const std::vector<int>& part)
{
  const std::vector<double>* weight = hg.findHyperedgeDoublePlane("weight");
  const std::vector<int>& eoff = hg.hyperedgeOffsets();
  const std::vector<int>& pins = hg.pinList();
  double cut = 0.0;
  for (int e = 0; e < hg.numHyperedges(); ++e) {
    bool in[2] = {false, false};
    for (int p = eoff[e]; p < eoff[e + 1]; ++p) {
      in[part[pins[p]]] = true;
    }
    if (in[0] && in[1]) {
      cut += weight != nullptr ? (*weight)[e] : 1.0;
    }
  }
  return cut;
}

// Reference connectivity-1 evaluation: sum over hyperedges of
// weight * (lambda - 1), lambda = number of distinct parts touched.
// For k == 2 this must agree with computeCut above.
double computeConnectivityCost(const Hypergraph& hg,
                               const std::vector<int>& part,
                               const int k)
{
  const std::vector<double>* weight = hg.findHyperedgeDoublePlane("weight");
  const std::vector<int>& eoff = hg.hyperedgeOffsets();
  const std::vector<int>& pins = hg.pinList();
  double cost = 0.0;
  for (int e = 0; e < hg.numHyperedges(); ++e) {
    std::set<int> parts;
    for (int p = eoff[e]; p < eoff[e + 1]; ++p) {
      parts.insert(part[pins[p]]);
    }
    if (parts.size() > 1) {
      cost += (weight != nullptr ? (*weight)[e] : 1.0)
              * (static_cast<int>(parts.size()) - 1);
    }
  }
  EXPECT_TRUE(std::all_of(part.begin(), part.end(), [k](const int p) {
    return p >= 0 && p < k;
  }));
  return cost;
}

// Unit-weight balance check: every part within (1 +- tol) * n/k vertices.
void expectBalanced(const std::vector<int>& part,
                    const double tol,
                    const int k = 2)
{
  const double share = part.size() / static_cast<double>(k);
  for (int p = 0; p < k; ++p) {
    const int size = static_cast<int>(std::count(part.begin(), part.end(), p));
    EXPECT_GE(size, (1.0 - tol) * share - 1e-9) << "part " << p;
    EXPECT_LE(size, (1.0 + tol) * share + 1e-9) << "part " << p;
  }
}

// From-scratch invariant check for any FMResult, independent of the
// engine's incremental bookkeeping: one part per vertex in [0, k)
// (enforced inside computeConnectivityCost), reported cost equal to the
// reference evaluation, and the reported `balanced` flag agreeing with
// the balance constraint recomputed from the "area" plane (1.0 default)
// against the engine's [(1 - tol) W/k, (1 + tol) W/k] bounds.
void expectInvariants(const Hypergraph& hg,
                      const FMResult& result,
                      const int k,
                      const double tol)
{
  ASSERT_EQ(result.partition.size(), static_cast<size_t>(hg.numVertices()));
  EXPECT_DOUBLE_EQ(result.cut_cost,
                   computeConnectivityCost(hg, result.partition, k));

  const std::vector<double>* area = hg.findVertexDoublePlane("area");
  std::vector<double> part_weight(k, 0.0);
  double total = 0.0;
  for (int v = 0; v < hg.numVertices(); ++v) {
    const double w = area != nullptr ? (*area)[v] : 1.0;
    part_weight[result.partition[v]] += w;
    total += w;
  }
  const double lo = (1.0 - tol) * total / k;
  const double hi = (1.0 + tol) * total / k;
  bool feasible = true;
  for (int p = 0; p < k; ++p) {
    feasible = feasible && part_weight[p] >= lo - 1e-9
               && part_weight[p] <= hi + 1e-9;
  }
  EXPECT_EQ(result.balanced, feasible);
}

RandomHypergraphParams typicalParams(const unsigned seed)
{
  RandomHypergraphParams params;
  params.num_vertices = 200;
  params.num_hyperedges = 320;
  params.min_degree = 2;
  params.max_degree = 6;
  params.seed = seed;
  return params;
}

TEST(RandomHypergraphTest, GeneratorDeterminism)
{
  const Hypergraph a = generateRandomHypergraph(typicalParams(7));
  const Hypergraph b = generateRandomHypergraph(typicalParams(7));
  EXPECT_EQ(a.hyperedgeOffsets(), b.hyperedgeOffsets());
  EXPECT_EQ(a.pinList(), b.pinList());

  const Hypergraph c = generateRandomHypergraph(typicalParams(8));
  EXPECT_NE(a.pinList(), c.pinList());
}

TEST(RandomHypergraphTest, GeneratorValidity)
{
  const RandomHypergraphParams params = typicalParams(3);
  const Hypergraph hg = generateRandomHypergraph(params);

  EXPECT_EQ(hg.numVertices(), params.num_vertices);
  EXPECT_EQ(hg.numHyperedges(), params.num_hyperedges);

  const std::vector<int>& eoff = hg.hyperedgeOffsets();
  const std::vector<int>& pins = hg.pinList();
  for (int e = 0; e < hg.numHyperedges(); ++e) {
    const int degree = eoff[e + 1] - eoff[e];
    EXPECT_GE(degree, params.min_degree) << "edge " << e;
    EXPECT_LE(degree, params.max_degree) << "edge " << e;

    std::set<int> distinct;
    for (int p = eoff[e]; p < eoff[e + 1]; ++p) {
      EXPECT_GE(pins[p], 0);
      EXPECT_LT(pins[p], params.num_vertices);
      distinct.insert(pins[p]);
    }
    EXPECT_EQ(static_cast<int>(distinct.size()), degree)
        << "duplicate pin in edge " << e;
  }
}

TEST(FMPartitionerTest, FMDeterminism)
{
  const Hypergraph hg = generateRandomHypergraph(typicalParams(11));
  FMParams params;
  params.seed = 5;

  const FMResult a = partitionFM(hg, params);
  const FMResult b = partitionFM(hg, params);
  EXPECT_EQ(a.partition, b.partition);
  EXPECT_EQ(a.cut_cost, b.cut_cost);
  EXPECT_EQ(a.passes_run, b.passes_run);
  EXPECT_EQ(a.balanced, b.balanced);
}

TEST(FMPartitionerTest, FMBalance)
{
  FMParams params;
  params.balance_tolerance = 0.10;
  for (const unsigned seed : {1u, 2u, 3u, 4u}) {
    const Hypergraph hg = generateRandomHypergraph(typicalParams(seed));
    params.seed = seed + 100;
    const FMResult result = partitionFM(hg, params);
    ASSERT_EQ(result.partition.size(),
              static_cast<size_t>(hg.numVertices()));
    EXPECT_TRUE(result.balanced) << "seed " << seed;
    expectBalanced(result.partition, params.balance_tolerance);
    // Sanity: the reported cut matches an independent evaluation.
    EXPECT_DOUBLE_EQ(result.cut_cost, computeCut(hg, result.partition));
  }
}

TEST(FMPartitionerTest, FMImprovesOverRandom)
{
  for (const unsigned seed : {21u, 22u, 23u}) {
    const Hypergraph hg = generateRandomHypergraph(typicalParams(seed));

    // A balanced but topology-blind initial: alternate by vertex index.
    std::vector<int> initial(hg.numVertices());
    for (int v = 0; v < hg.numVertices(); ++v) {
      initial[v] = v % 2;
    }
    const double initial_cut = computeCut(hg, initial);

    FMParams params;
    params.initial = FMParams::InitialPartition::kProvided;
    const FMResult result = partitionFM(hg, params, &initial);
    EXPECT_LE(result.cut_cost, initial_cut + 1e-9) << "seed " << seed;
    EXPECT_TRUE(result.balanced);
  }
}

// Two 4-cliques joined by one bridge edge: the unique optimal balanced
// bisection separates the cliques and cuts only the bridge (cost 1).
// Splitting any clique instead cuts >= 3 of its internal edges.
TEST(FMPartitionerTest, FMKnownOptimal)
{
  std::vector<std::vector<int>> edges;
  for (const int base : {0, 4}) {
    for (int i = 0; i < 4; ++i) {
      for (int j = i + 1; j < 4; ++j) {
        edges.push_back({base + i, base + j});
      }
    }
  }
  edges.push_back({3, 4});  // the bridge

  Hypergraph hg;
  hg.buildFromTopology(8, edges);

  // Tolerance must admit the 3/5 intermediate states a single-vertex
  // move passes through (see FMParams::balance_tolerance); 0.30 allows
  // sides of 3..5 while the 4/4 optimum still wins on cut.
  FMParams params;
  params.balance_tolerance = 0.30;
  const FMResult result = partitionFM(hg, params);

  EXPECT_TRUE(result.balanced);
  EXPECT_DOUBLE_EQ(result.cut_cost, 1.0);
  for (int v = 1; v < 4; ++v) {
    EXPECT_EQ(result.partition[v], result.partition[0]) << "vertex " << v;
  }
  for (int v = 5; v < 8; ++v) {
    EXPECT_EQ(result.partition[v], result.partition[4]) << "vertex " << v;
  }
  EXPECT_NE(result.partition[0], result.partition[4]);
}

// One topology (an 8-cycle), three weight configurations. A balanced
// bisection of a cycle must cut at least two edges, so the optimum is
// the split whose two cut edges are the two cheapest — moving the cheap
// pair around the ring must drag the FM solution with it. This is the
// proof that the objective really follows the "weight" plane.
TEST(FMPartitionerTest, FMWeightedCut)
{
  std::vector<std::vector<int>> edges;
  for (int v = 0; v < 8; ++v) {
    edges.push_back({v, (v + 1) % 8});  // edge e_v = {v, v+1 mod 8}
  }

  FMParams params;
  params.balance_tolerance = 0.30;

  auto same_side = [](const FMResult& r, std::vector<int> group) {
    for (const int v : group) {
      if (r.partition[v] != r.partition[group[0]]) {
        return false;
      }
    }
    return true;
  };

  {
    // Unweighted: every contiguous 4/4 split cuts exactly two unit edges.
    Hypergraph hg;
    hg.buildFromTopology(8, edges);
    const FMResult result = partitionFM(hg, params);
    EXPECT_TRUE(result.balanced);
    EXPECT_DOUBLE_EQ(result.cut_cost, 2.0);
  }
  {
    // Cheap edges e3={3,4} and e7={7,0}: optimum is {0,1,2,3}|{4,5,6,7}
    // at 0.2; any other balanced split cuts a unit edge (>= 1.1).
    Hypergraph hg;
    hg.buildFromTopology(8, edges);
    std::vector<double>& weight = hg.hyperedgeDoublePlane("weight");
    weight.assign(8, 1.0);
    weight[3] = weight[7] = 0.1;
    const FMResult result = partitionFM(hg, params);
    EXPECT_TRUE(result.balanced);
    EXPECT_NEAR(result.cut_cost, 0.2, 1e-9);
    EXPECT_TRUE(same_side(result, {0, 1, 2, 3}));
    EXPECT_TRUE(same_side(result, {4, 5, 6, 7}));
    EXPECT_NE(result.partition[0], result.partition[4]);
  }
  {
    // Same topology, cheap pair rotated to e1={1,2} and e5={5,6}: the
    // optimal split rotates to {2,3,4,5}|{6,7,0,1}.
    Hypergraph hg;
    hg.buildFromTopology(8, edges);
    std::vector<double>& weight = hg.hyperedgeDoublePlane("weight");
    weight.assign(8, 1.0);
    weight[1] = weight[5] = 0.1;
    const FMResult result = partitionFM(hg, params);
    EXPECT_TRUE(result.balanced);
    EXPECT_NEAR(result.cut_cost, 0.2, 1e-9);
    EXPECT_TRUE(same_side(result, {2, 3, 4, 5}));
    EXPECT_TRUE(same_side(result, {6, 7, 0, 1}));
    EXPECT_NE(result.partition[2], result.partition[6]);
  }
}

// The objective really is connectivity-1, not spanning cut: one triangle
// hyperedge with each vertex pinned (by a tight balance tolerance) to its
// own part costs 2 * weight. A spanning-cut objective would report
// 1 * weight.
TEST(FMPartitionerTest, KWayLambdaMinusOneObjective)
{
  Hypergraph hg;
  hg.buildFromTopology(3, {{0, 1, 2}});

  FMParams params;
  params.num_parts = 3;
  params.balance_tolerance = 0.10;  // every part must hold exactly 1 vertex
  params.initial = FMParams::InitialPartition::kProvided;
  const std::vector<int> initial = {0, 1, 2};

  const FMResult result = partitionFM(hg, params, &initial);
  EXPECT_EQ(result.partition, initial);  // no feasible move exists
  EXPECT_TRUE(result.balanced);
  EXPECT_DOUBLE_EQ(result.cut_cost, 2.0);

  // Same with a weighted edge: cost scales as weight * (lambda - 1).
  Hypergraph whg;
  whg.buildFromTopology(3, {{0, 1, 2}});
  whg.hyperedgeDoublePlane("weight")[0] = 2.5;
  const FMResult weighted = partitionFM(whg, params, &initial);
  EXPECT_DOUBLE_EQ(weighted.cut_cost, 5.0);
}

// Three 4-cliques joined by two bridges: the unique optimal balanced
// 3-way split gives each clique its own part and cuts only the two
// bridges (lambda 2 each, cost 2). Splitting any clique cuts >= 3 of its
// internal edges instead.
TEST(FMPartitionerTest, KWayKnownOptimal)
{
  std::vector<std::vector<int>> edges;
  for (const int base : {0, 4, 8}) {
    for (int i = 0; i < 4; ++i) {
      for (int j = i + 1; j < 4; ++j) {
        edges.push_back({base + i, base + j});
      }
    }
  }
  edges.push_back({3, 4});  // bridge clique 0 - clique 1
  edges.push_back({7, 8});  // bridge clique 1 - clique 2

  Hypergraph hg;
  hg.buildFromTopology(12, edges);

  FMParams params;
  params.num_parts = 3;
  params.balance_tolerance = 0.30;  // W/K = 4; allow the 3/5 transit states
  const FMResult result = partitionFM(hg, params);

  EXPECT_TRUE(result.balanced);
  EXPECT_DOUBLE_EQ(result.cut_cost, 2.0);
  for (const int base : {0, 4, 8}) {
    for (int v = base + 1; v < base + 4; ++v) {
      EXPECT_EQ(result.partition[v], result.partition[base]) << "vertex " << v;
    }
  }
  EXPECT_NE(result.partition[0], result.partition[4]);
  EXPECT_NE(result.partition[4], result.partition[8]);
  EXPECT_NE(result.partition[0], result.partition[8]);
}

TEST(FMPartitionerTest, KWayDeterminismBalanceAndReportedCost)
{
  for (const int k : {3, 4}) {
    for (const unsigned seed : {31u, 32u}) {
      const Hypergraph hg = generateRandomHypergraph(typicalParams(seed));
      FMParams params;
      params.num_parts = k;
      params.seed = seed;

      const FMResult a = partitionFM(hg, params);
      const FMResult b = partitionFM(hg, params);
      EXPECT_EQ(a.partition, b.partition) << "k " << k << " seed " << seed;
      EXPECT_EQ(a.cut_cost, b.cut_cost);
      EXPECT_EQ(a.passes_run, b.passes_run);

      EXPECT_TRUE(a.balanced) << "k " << k << " seed " << seed;
      expectBalanced(a.partition, params.balance_tolerance, k);
      EXPECT_DOUBLE_EQ(a.cut_cost, computeConnectivityCost(hg, a.partition, k));
    }
  }
}

TEST(FMPartitionerTest, KWayImprovesOverBlind)
{
  const int k = 4;
  for (const unsigned seed : {41u, 42u, 43u}) {
    const Hypergraph hg = generateRandomHypergraph(typicalParams(seed));

    // A balanced but topology-blind initial: stripe by vertex index.
    std::vector<int> initial(hg.numVertices());
    for (int v = 0; v < hg.numVertices(); ++v) {
      initial[v] = v % k;
    }
    const double initial_cost = computeConnectivityCost(hg, initial, k);

    FMParams params;
    params.num_parts = k;
    params.initial = FMParams::InitialPartition::kProvided;
    const FMResult result = partitionFM(hg, params, &initial);
    EXPECT_LE(result.cut_cost, initial_cost + 1e-9) << "seed " << seed;
    EXPECT_TRUE(result.balanced);
  }
}

// A provided initial that leaves parts empty is maximally unbalanced;
// recovery mode must walk the solution back into tolerance.
TEST(FMPartitionerTest, KWayRecoversFromEmptyParts)
{
  const Hypergraph hg = generateRandomHypergraph(typicalParams(51));
  std::vector<int> initial(hg.numVertices());
  for (int v = 0; v < hg.numVertices(); ++v) {
    initial[v] = v % 2;  // parts 2 and 3 start empty
  }

  FMParams params;
  params.num_parts = 4;
  params.initial = FMParams::InitialPartition::kProvided;
  const FMResult result = partitionFM(hg, params, &initial);
  EXPECT_TRUE(result.balanced);
  expectBalanced(result.partition, params.balance_tolerance, 4);
  EXPECT_DOUBLE_EQ(result.cut_cost,
                   computeConnectivityCost(hg, result.partition, 4));
}

// Out-of-range values in a provided initial make it unusable: same
// fallback as Stage 1, now keyed on [0, num_parts).
TEST(FMPartitionerTest, KWayProvidedOutOfRangeFallsBack)
{
  const Hypergraph hg = generateRandomHypergraph(typicalParams(61));
  std::vector<int> bad(hg.numVertices(), 0);
  bad[0] = 3;  // == num_parts: out of range

  FMParams provided;
  provided.num_parts = 3;
  provided.initial = FMParams::InitialPartition::kProvided;
  const FMResult from_bad = partitionFM(hg, provided, &bad);

  FMParams random = provided;
  random.initial = FMParams::InitialPartition::kRandom;
  const FMResult from_random = partitionFM(hg, random);
  EXPECT_EQ(from_bad.partition, from_random.partition);
  EXPECT_EQ(from_bad.cut_cost, from_random.cut_cost);
}

// K = 2 through the K-way machinery is Stage 1: the connectivity-1 cost
// and the spanning cut coincide, and the explicit num_parts = 2 run
// matches the default-params run exactly.
TEST(FMPartitionerTest, TwoWayMatchesSpanningCut)
{
  const Hypergraph hg = generateRandomHypergraph(typicalParams(71));
  FMParams params;
  params.seed = 7;
  params.num_parts = 2;
  const FMResult result = partitionFM(hg, params);
  EXPECT_DOUBLE_EQ(result.cut_cost, computeCut(hg, result.partition));
  EXPECT_DOUBLE_EQ(result.cut_cost,
                   computeConnectivityCost(hg, result.partition, 2));
}

TEST(FMPartitionerTest, SinglePartIsTrivial)
{
  const Hypergraph hg = generateRandomHypergraph(typicalParams(81));
  FMParams params;
  params.num_parts = 1;
  const FMResult result = partitionFM(hg, params);
  EXPECT_TRUE(result.balanced);
  EXPECT_DOUBLE_EQ(result.cut_cost, 0.0);
  EXPECT_TRUE(std::all_of(result.partition.begin(),
                          result.partition.end(),
                          [](const int p) { return p == 0; }));
}

// Balance follows the vertex "area" plane, not the vertex count. Vertex 0
// has area 5, the seven others area 1: W = 12, W/2 = 6, tolerance 0.10
// gives bounds [5.4, 6.6], so the ONLY feasible bisections put vertex 0
// with exactly one unit companion (5 + 1 = 6 vs 6). Any count-balanced
// 4/4 split has area 8/4 and is infeasible — an engine that ignored the
// area plane would happily return one and still claim balanced.
TEST(FMPartitionerTest, AreaWeightedBalance)
{
  // Edges: {0,1}, {0,2}, and the 15-edge clique on {2..7}. With
  // companion 1 the cut is just {0,2} (cost 1); any other companion cuts
  // five clique edges as well.
  std::vector<std::vector<int>> edges = {{0, 1}, {0, 2}};
  for (int i = 2; i < 8; ++i) {
    for (int j = i + 1; j < 8; ++j) {
      edges.push_back({i, j});
    }
  }

  FMParams params;
  params.balance_tolerance = 0.10;

  {
    // Provided area-balanced optimum: FM must keep it feasible and
    // cannot leave the cut-1 solution.
    Hypergraph hg;
    hg.buildFromTopology(8, edges);
    std::vector<double>& area = hg.vertexDoublePlane("area");
    area.assign(8, 1.0);
    area[0] = 5.0;

    FMParams provided = params;
    provided.initial = FMParams::InitialPartition::kProvided;
    const std::vector<int> initial = {0, 0, 1, 1, 1, 1, 1, 1};
    const FMResult result = partitionFM(hg, provided, &initial);

    EXPECT_TRUE(result.balanced);
    expectInvariants(hg, result, 2, provided.balance_tolerance);
    EXPECT_DOUBLE_EQ(result.cut_cost, 1.0);
    EXPECT_EQ(result.partition[1], result.partition[0]);
  }
  {
    // Random initial: wherever FM lands, a balanced claim implies the
    // area math held — vertex 0's part must contain exactly 2 vertices.
    Hypergraph hg;
    hg.buildFromTopology(8, edges);
    std::vector<double>& area = hg.vertexDoublePlane("area");
    area.assign(8, 1.0);
    area[0] = 5.0;

    const FMResult result = partitionFM(hg, params);
    EXPECT_TRUE(result.balanced);
    expectInvariants(hg, result, 2, params.balance_tolerance);
    const int side0 = result.partition[0];
    const int in_side0 = static_cast<int>(std::count(
        result.partition.begin(), result.partition.end(), side0));
    EXPECT_EQ(in_side0, 2);
  }
}

// When no feasible partition exists at all, the engine must say so:
// balanced == false, honestly reported cost, normal termination. Vertex 0
// alone outweighs the upper bound (area 10 vs max part 1.1 * 13/2 =
// 7.15), so every bisection is infeasible.
TEST(FMPartitionerTest, InfeasibleToleranceReportsUnbalanced)
{
  Hypergraph hg;
  hg.buildFromTopology(4, {{0, 1}, {1, 2}, {2, 3}});
  std::vector<double>& area = hg.vertexDoublePlane("area");
  area.assign(4, 1.0);
  area[0] = 10.0;

  FMParams params;
  params.balance_tolerance = 0.10;
  const FMResult result = partitionFM(hg, params);

  EXPECT_FALSE(result.balanced);
  expectInvariants(hg, result, 2, params.balance_tolerance);
  EXPECT_LE(result.passes_run, params.max_passes);

  // Same verdict from an unbalanced provided initial: recovery mode has
  // nowhere feasible to walk to and must not misreport success.
  FMParams provided = params;
  provided.initial = FMParams::InitialPartition::kProvided;
  const std::vector<int> initial = {0, 1, 1, 1};
  const FMResult from_provided = partitionFM(hg, provided, &initial);
  EXPECT_FALSE(from_provided.balanced);
  expectInvariants(hg, from_provided, 2, provided.balance_tolerance);
}

// Quality floor on fixed-seed random hypergraphs. The golden values are
// the costs the current engine produces (it is deterministic, see
// FMDeterminism); an algorithm change that matches or improves quality
// still passes, while a change that silently degrades the result —
// e.g. a refinement stage that stops refining — fails. Update a golden
// only for a change that intentionally trades quality away.
TEST(FMPartitionerTest, GoldenCostRegression)
{
  struct GoldenCase
  {
    unsigned graph_seed;
    int k;
    double golden;
  };
  const std::vector<GoldenCase> cases = {
      {7, 2, 163.0},
      {21, 2, 155.0},
      {7, 4, 326.0},
      {21, 4, 301.0},
  };

  for (const GoldenCase& c : cases) {
    const Hypergraph hg = generateRandomHypergraph(typicalParams(c.graph_seed));
    FMParams params;
    params.num_parts = c.k;
    const FMResult result = partitionFM(hg, params);

    EXPECT_TRUE(result.balanced)
        << "graph seed " << c.graph_seed << " k " << c.k;
    expectInvariants(hg, result, c.k, params.balance_tolerance);
    EXPECT_LE(result.cut_cost, c.golden + 1e-9)
        << "quality regression: graph seed " << c.graph_seed << " k " << c.k
        << " golden " << c.golden;
  }
}

// Real data: Nangate45 + the gcd DEF, loaded once for the suite exactly
// as hypergraph_test does.
class FMOdbTest : public ::testing::Test
{
 protected:
  static void SetUpTestSuite()
  {
    logger_ = new utl::Logger();
    db_ = odb::dbDatabase::create();

    const std::string data_dir = EDA_LAB_DATA_DIR;
    odb::lefin lef_reader(db_, logger_, false);
    odb::dbLib* tech_lib = lef_reader.createTechAndLib(
        "tech",
        "nangate45_tech",
        (data_dir + "/nangate45/Nangate45_tech.lef").c_str());
    ASSERT_NE(tech_lib, nullptr);
    odb::dbLib* cell_lib = lef_reader.createLib(
        tech_lib->getTech(),
        "nangate45_stdcell",
        (data_dir + "/nangate45/Nangate45_stdcell.lef").c_str());
    ASSERT_NE(cell_lib, nullptr);

    odb::dbChip* chip = odb::dbChip::create(db_, tech_lib->getTech(), "gcd");
    ASSERT_NE(chip, nullptr);
    odb::defin def_reader(db_, logger_);
    std::vector<odb::dbLib*> search_libs = {tech_lib, cell_lib};
    def_reader.readChip(
        search_libs, (data_dir + "/gcd_nangate45.def").c_str(), chip, false);
    block_ = chip->getBlock();
    ASSERT_NE(block_, nullptr);
  }

  static void TearDownTestSuite()
  {
    odb::dbDatabase::destroy(db_);
    db_ = nullptr;
    block_ = nullptr;
    delete logger_;
    logger_ = nullptr;
  }

  static utl::Logger* logger_;
  static odb::dbDatabase* db_;
  static odb::dbBlock* block_;
};

utl::Logger* FMOdbTest::logger_ = nullptr;
odb::dbDatabase* FMOdbTest::db_ = nullptr;
odb::dbBlock* FMOdbTest::block_ = nullptr;

TEST_F(FMOdbTest, FMOnOdbDesign)
{
  Hypergraph hg;
  hg.buildFromBlock(block_);
  ASSERT_EQ(hg.numVertices(), 734);

  FMParams params;
  params.seed = 9;

  // Balance + reported-cut consistency on real topology.
  const FMResult result = partitionFM(hg, params);
  EXPECT_TRUE(result.balanced);
  expectBalanced(result.partition, params.balance_tolerance);
  EXPECT_DOUBLE_EQ(result.cut_cost, computeCut(hg, result.partition));

  // Determinism on real topology.
  const FMResult again = partitionFM(hg, params);
  EXPECT_EQ(result.partition, again.partition);
  EXPECT_EQ(result.cut_cost, again.cut_cost);
  EXPECT_EQ(result.passes_run, again.passes_run);

  // Cut improvement over a balanced topology-blind initial. On a real
  // netlist FM must do strictly better than index-alternation.
  std::vector<int> initial(hg.numVertices());
  for (int v = 0; v < hg.numVertices(); ++v) {
    initial[v] = v % 2;
  }
  const double initial_cut = computeCut(hg, initial);
  FMParams provided = params;
  provided.initial = FMParams::InitialPartition::kProvided;
  const FMResult improved = partitionFM(hg, provided, &initial);
  EXPECT_TRUE(improved.balanced);
  EXPECT_LT(improved.cut_cost, initial_cut);
}

TEST_F(FMOdbTest, KWayOnOdbDesign)
{
  Hypergraph hg;
  hg.buildFromBlock(block_);
  ASSERT_EQ(hg.numVertices(), 734);

  const int k = 4;
  FMParams params;
  params.num_parts = k;
  params.seed = 9;

  // Balance + reported-cost consistency on real topology.
  const FMResult result = partitionFM(hg, params);
  EXPECT_TRUE(result.balanced);
  expectBalanced(result.partition, params.balance_tolerance, k);
  EXPECT_DOUBLE_EQ(result.cut_cost,
                   computeConnectivityCost(hg, result.partition, k));

  // Determinism on real topology.
  const FMResult again = partitionFM(hg, params);
  EXPECT_EQ(result.partition, again.partition);
  EXPECT_EQ(result.cut_cost, again.cut_cost);
  EXPECT_EQ(result.passes_run, again.passes_run);

  // Cost improvement over a balanced topology-blind initial.
  std::vector<int> initial(hg.numVertices());
  for (int v = 0; v < hg.numVertices(); ++v) {
    initial[v] = v % k;
  }
  const double initial_cost = computeConnectivityCost(hg, initial, k);
  FMParams provided = params;
  provided.initial = FMParams::InitialPartition::kProvided;
  const FMResult improved = partitionFM(hg, provided, &initial);
  EXPECT_TRUE(improved.balanced);
  EXPECT_LT(improved.cut_cost, initial_cost);
}

}  // namespace
}  // namespace eda
