// Tests for the Stage 1 partitioning engine: the procedural random
// hypergraph generator and the flat 2-way FM partitioner. Only the final
// FMOnOdbDesign test touches LEF/DEF data; everything else runs on
// dbBlock-free hypergraphs (that being the point of buildFromTopology).

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

// Unit-weight balance check: both sides within (1 +- tol) * n/2 vertices.
void expectBalanced(const std::vector<int>& part, const double tol)
{
  const double half = part.size() / 2.0;
  const int side0 = static_cast<int>(std::count(part.begin(), part.end(), 0));
  const int side1 = static_cast<int>(part.size()) - side0;
  EXPECT_GE(side0, (1.0 - tol) * half - 1e-9);
  EXPECT_LE(side0, (1.0 + tol) * half + 1e-9);
  EXPECT_GE(side1, (1.0 - tol) * half - 1e-9);
  EXPECT_LE(side1, (1.0 + tol) * half + 1e-9);
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

}  // namespace
}  // namespace eda
