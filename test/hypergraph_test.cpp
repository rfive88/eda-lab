// Tests for the eda-lab hypergraph netlist model (Phase 0, Item 3).

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "hypergraph/hypergraph.h"
#include "odb/db.h"
#include "odb/defin.h"
#include "odb/lefin.h"
#include "spdlog/sinks/ostream_sink.h"
#include "utl/Logger.h"

namespace eda {
namespace {

// Loads Nangate45 LEF + the gcd DEF once for the whole suite; parsing the
// LEF per-test would dominate the run time.
class HypergraphTest : public ::testing::Test
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

    // readChip at the pinned OpenROAD SHA requires a pre-created chip.
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

utl::Logger* HypergraphTest::logger_ = nullptr;
odb::dbDatabase* HypergraphTest::db_ = nullptr;
odb::dbBlock* HypergraphTest::block_ = nullptr;

TEST_F(HypergraphTest, TestBuildEmpty)
{
  odb::dbDatabase* db = odb::dbDatabase::create();
  odb::dbTech* tech = odb::dbTech::create(db, "empty_tech");
  ASSERT_NE(tech, nullptr);
  odb::dbChip* chip = odb::dbChip::create(db, tech, "empty_chip");
  ASSERT_NE(chip, nullptr);
  odb::dbBlock* block = odb::dbBlock::create(chip, "top");
  ASSERT_NE(block, nullptr);

  Hypergraph graph;
  graph.buildFromBlock(block);

  EXPECT_EQ(graph.numVertices(), 0);
  EXPECT_EQ(graph.numHyperedges(), 0);
  EXPECT_EQ(graph.numPins(), 0);
  EXPECT_TRUE(graph.pinList().empty());
  EXPECT_TRUE(graph.vertexPinList().empty());
  ASSERT_EQ(graph.hyperedgeOffsets().size(), 1u);
  EXPECT_EQ(graph.hyperedgeOffsets()[0], 0);
  ASSERT_EQ(graph.vertexOffsets().size(), 1u);
  EXPECT_EQ(graph.vertexOffsets()[0], 0);

  EXPECT_FALSE(graph.vertexId(0).isValid());
  EXPECT_FALSE(graph.hyperedgeId(0).isValid());
  EXPECT_EQ(graph.vertexIndex(odb::dbId<odb::dbInst>(1)),
            Hypergraph::kInvalidIndex);

  odb::dbDatabase::destroy(db);
}

TEST_F(HypergraphTest, TestVertexIndexing)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  ASSERT_EQ(graph.numVertices(),
            static_cast<int>(block_->getInsts().size()));

  // index -> id -> index round trip; ids must resolve to live dbInsts.
  for (int v = 0; v < graph.numVertices(); ++v) {
    const odb::dbId<odb::dbInst> id = graph.vertexId(v);
    ASSERT_TRUE(id.isValid());
    EXPECT_EQ(graph.vertexIndex(id), v);
    EXPECT_NE(odb::dbInst::getInst(block_, id), nullptr);
  }

  // id -> index -> id round trip over every instance in the block.
  for (odb::dbInst* inst : block_->getInsts()) {
    const odb::dbId<odb::dbInst> id(inst->getId());
    const int v = graph.vertexIndex(id);
    ASSERT_NE(v, Hypergraph::kInvalidIndex);
    EXPECT_EQ(graph.vertexId(v), id);
  }

  // Out-of-range and unknown lookups fail cleanly.
  EXPECT_FALSE(graph.vertexId(-1).isValid());
  EXPECT_FALSE(graph.vertexId(graph.numVertices()).isValid());
  EXPECT_EQ(graph.vertexIndex(odb::dbId<odb::dbInst>()),
            Hypergraph::kInvalidIndex);
}

TEST_F(HypergraphTest, TestHyperedgeIndexing)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  ASSERT_EQ(graph.numHyperedges(),
            static_cast<int>(block_->getNets().size()));

  for (int e = 0; e < graph.numHyperedges(); ++e) {
    const odb::dbId<odb::dbNet> id = graph.hyperedgeId(e);
    ASSERT_TRUE(id.isValid());
    EXPECT_EQ(graph.hyperedgeIndex(id), e);
    EXPECT_NE(odb::dbNet::getNet(block_, id), nullptr);
  }

  for (odb::dbNet* net : block_->getNets()) {
    const odb::dbId<odb::dbNet> id(net->getId());
    const int e = graph.hyperedgeIndex(id);
    ASSERT_NE(e, Hypergraph::kInvalidIndex);
    EXPECT_EQ(graph.hyperedgeId(e), id);
  }

  EXPECT_FALSE(graph.hyperedgeId(-1).isValid());
  EXPECT_FALSE(graph.hyperedgeId(graph.numHyperedges()).isValid());
  EXPECT_EQ(graph.hyperedgeIndex(odb::dbId<odb::dbNet>()),
            Hypergraph::kInvalidIndex);
}

TEST_F(HypergraphTest, TestConnectivityConsistency)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  const std::vector<int>& edge_offsets = graph.hyperedgeOffsets();
  const std::vector<int>& pins = graph.pinList();
  ASSERT_EQ(edge_offsets.size(),
            static_cast<size_t>(graph.numHyperedges()) + 1);
  ASSERT_EQ(edge_offsets.back(), graph.numPins());

  // Every hyperedge's pin slice must match the net's dbITerms, one pin
  // per iterm, as an unordered multiset of vertex indices.
  int total_iterms = 0;
  for (odb::dbNet* net : block_->getNets()) {
    const int e = graph.hyperedgeIndex(odb::dbId<odb::dbNet>(net->getId()));
    ASSERT_NE(e, Hypergraph::kInvalidIndex);

    std::vector<int> expected;
    for (odb::dbITerm* iterm : net->getITerms()) {
      expected.push_back(
          graph.vertexIndex(odb::dbId<odb::dbInst>(iterm->getInst()->getId())));
    }
    total_iterms += static_cast<int>(expected.size());

    std::vector<int> actual(pins.begin() + edge_offsets[e],
                            pins.begin() + edge_offsets[e + 1]);
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, expected) << "pin mismatch on net " << net->getName();
  }
  EXPECT_EQ(graph.numPins(), total_iterms);

  // The vertex-major CSR must be the exact transpose of the edge-major
  // one: same (vertex, hyperedge) incidence multiset on both sides.
  const std::vector<int>& vertex_offsets = graph.vertexOffsets();
  const std::vector<int>& vertex_pins = graph.vertexPinList();
  ASSERT_EQ(vertex_offsets.size(),
            static_cast<size_t>(graph.numVertices()) + 1);
  ASSERT_EQ(vertex_offsets.back(), graph.numPins());
  ASSERT_EQ(vertex_pins.size(), pins.size());

  std::vector<std::pair<int, int>> from_edges;
  for (int e = 0; e < graph.numHyperedges(); ++e) {
    for (int p = edge_offsets[e]; p < edge_offsets[e + 1]; ++p) {
      from_edges.emplace_back(pins[p], e);
    }
  }
  std::vector<std::pair<int, int>> from_vertices;
  for (int v = 0; v < graph.numVertices(); ++v) {
    for (int p = vertex_offsets[v]; p < vertex_offsets[v + 1]; ++p) {
      from_vertices.emplace_back(v, vertex_pins[p]);
    }
  }
  std::sort(from_edges.begin(), from_edges.end());
  std::sort(from_vertices.begin(), from_vertices.end());
  EXPECT_EQ(from_edges, from_vertices);
}

TEST_F(HypergraphTest, TestRoundTripComparison)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  // hello_odb reports block->getInsts().size() / block->getNets().size()
  // for the same LEF/DEF; the hypergraph must agree.
  EXPECT_EQ(graph.numVertices(),
            static_cast<int>(block_->getInsts().size()));
  EXPECT_EQ(graph.numHyperedges(),
            static_cast<int>(block_->getNets().size()));

  // gcd_nangate45.def declares COMPONENTS 734.
  EXPECT_EQ(graph.numVertices(), 734);

  // Rebuild is idempotent on an unchanged block.
  Hypergraph rebuilt;
  rebuilt.buildFromBlock(block_);
  EXPECT_EQ(rebuilt.numVertices(), graph.numVertices());
  EXPECT_EQ(rebuilt.numHyperedges(), graph.numHyperedges());
  EXPECT_EQ(rebuilt.pinList(), graph.pinList());
  EXPECT_EQ(rebuilt.hyperedgeOffsets(), graph.hyperedgeOffsets());
  EXPECT_EQ(rebuilt.vertexOffsets(), graph.vertexOffsets());
  EXPECT_EQ(rebuilt.vertexPinList(), graph.vertexPinList());
}

TEST_F(HypergraphTest, PlaneCreateOnDemand)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);
  const size_t nv = static_cast<size_t>(graph.numVertices());
  const size_t ne = static_cast<size_t>(graph.numHyperedges());

  EXPECT_FALSE(graph.hasVertexPlane("weight"));
  EXPECT_FALSE(graph.hasHyperedgePlane("weight"));

  // First access creates the plane at the right size, zero-initialized.
  std::vector<double>& vd = graph.vertexDoublePlane("weight");
  EXPECT_TRUE(graph.hasVertexPlane("weight"));
  ASSERT_EQ(vd.size(), nv);
  EXPECT_TRUE(std::all_of(
      vd.begin(), vd.end(), [](const double x) { return x == 0.0; }));

  std::vector<int>& vi = graph.vertexIntPlane("partition");
  ASSERT_EQ(vi.size(), nv);
  EXPECT_TRUE(
      std::all_of(vi.begin(), vi.end(), [](const int x) { return x == 0; }));

  std::vector<bool>& vb = graph.vertexBoolPlane("fixed");
  ASSERT_EQ(vb.size(), nv);
  EXPECT_TRUE(std::none_of(vb.begin(), vb.end(), [](const bool x) { return x; }));

  std::vector<double>& ed = graph.hyperedgeDoublePlane("weight");
  EXPECT_TRUE(graph.hasHyperedgePlane("weight"));
  ASSERT_EQ(ed.size(), ne);
  std::vector<int>& ei = graph.hyperedgeIntPlane("cut");
  ASSERT_EQ(ei.size(), ne);
  std::vector<bool>& eb = graph.hyperedgeBoolPlane("critical");
  ASSERT_EQ(eb.size(), ne);
}

TEST_F(HypergraphTest, PlanePersistence)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  std::vector<int>& first = graph.vertexIntPlane("partition");
  for (int v = 0; v < graph.numVertices(); ++v) {
    first[v] = v % 3;
  }

  // A second access must hand back the same storage, values intact.
  std::vector<int>& second = graph.vertexIntPlane("partition");
  EXPECT_EQ(&first, &second);
  for (int v = 0; v < graph.numVertices(); ++v) {
    EXPECT_EQ(second[v], v % 3);
  }
}

TEST_F(HypergraphTest, PlaneTypeConflict)
{
  // Capture the diagnostic through a private in-memory sink rather than
  // asserting on the process's stdout.
  utl::Logger logger;
  auto stream = std::make_shared<std::ostringstream>();
  logger.addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream));

  Hypergraph graph(&logger);
  graph.buildFromBlock(block_);

  std::vector<double>& doubles = graph.vertexDoublePlane("weight");
  doubles[0] = 2.5;
  EXPECT_TRUE(stream->str().empty());

  // Documented conflict behavior: warning logged, the call returns
  // separate valid int storage, and the original double plane (and any
  // reference to it) is untouched.
  std::vector<int>& ints = graph.vertexIntPlane("weight");
  EXPECT_NE(stream->str().find("created as double"), std::string::npos);
  ASSERT_EQ(ints.size(), static_cast<size_t>(graph.numVertices()));
  EXPECT_EQ(ints[0], 0);
  EXPECT_EQ(doubles[0], 2.5);
  ints[0] = 7;
  EXPECT_EQ(doubles[0], 2.5);

  // The name still resolves to the same double plane for matched access.
  EXPECT_EQ(&graph.vertexDoublePlane("weight"), &doubles);
}

TEST_F(HypergraphTest, PlaneRemoval)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  graph.vertexBoolPlane("fixed")[0] = true;
  EXPECT_TRUE(graph.hasVertexPlane("fixed"));

  graph.removeVertexPlane("fixed");
  EXPECT_FALSE(graph.hasVertexPlane("fixed"));

  // Removing an unknown name is a harmless no-op.
  graph.removeVertexPlane("no_such_plane");
  graph.removeHyperedgePlane("no_such_plane");

  // Re-access after removal starts from defaults again.
  EXPECT_FALSE(graph.vertexBoolPlane("fixed")[0]);

  graph.hyperedgeIntPlane("cut");
  EXPECT_TRUE(graph.hasHyperedgePlane("cut"));
  graph.removeHyperedgePlane("cut");
  EXPECT_FALSE(graph.hasHyperedgePlane("cut"));
}

TEST_F(HypergraphTest, PlaneRebuildInvalidation)
{
  Hypergraph graph;
  graph.buildFromBlock(block_);

  graph.vertexDoublePlane("weight")[0] = 3.0;
  graph.hyperedgeIntPlane("cut")[0] = 1;

  // Rebuilding reassigns local indices, so every plane must be gone.
  graph.buildFromBlock(block_);
  EXPECT_FALSE(graph.hasVertexPlane("weight"));
  EXPECT_FALSE(graph.hasHyperedgePlane("cut"));
  EXPECT_EQ(graph.vertexDoublePlane("weight")[0], 0.0);

  // clearAllPlanes() wipes planes without touching the topology.
  graph.vertexDoublePlane("weight")[0] = 3.0;
  graph.clearAllPlanes();
  EXPECT_FALSE(graph.hasVertexPlane("weight"));
  EXPECT_EQ(graph.numVertices(), 734);

  // clear() goes through the same choke point.
  graph.vertexDoublePlane("weight");
  graph.clear();
  EXPECT_FALSE(graph.hasVertexPlane("weight"));
}

TEST_F(HypergraphTest, PlaneIndependence)
{
  utl::Logger logger;
  auto stream = std::make_shared<std::ostringstream>();
  logger.addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream));

  Hypergraph graph(&logger);
  graph.buildFromBlock(block_);
  ASSERT_NE(graph.numVertices(), graph.numHyperedges());

  // Same name, different element kind: separate planes, no conflict.
  std::vector<int>& vx = graph.vertexIntPlane("x");
  std::vector<int>& ex = graph.hyperedgeIntPlane("x");
  EXPECT_TRUE(stream->str().empty());
  EXPECT_EQ(vx.size(), static_cast<size_t>(graph.numVertices()));
  EXPECT_EQ(ex.size(), static_cast<size_t>(graph.numHyperedges()));

  vx[0] = 42;
  EXPECT_EQ(ex[0], 0);

  // Even the type binding is per-kind: a double hyperedge plane "x" after
  // an int vertex plane "x" is not a conflict.
  graph.removeHyperedgePlane("x");
  graph.hyperedgeDoublePlane("x");
  EXPECT_TRUE(stream->str().empty());

  // Removal on one side leaves the other side alone.
  graph.removeVertexPlane("x");
  EXPECT_FALSE(graph.hasVertexPlane("x"));
  EXPECT_TRUE(graph.hasHyperedgePlane("x"));
}

// Procedural construction needs no fixture (that is the point): plain TEST
// so the LEF/DEF suite setup is not triggered.
TEST(HypergraphTopologyTest, BuildFromTopology)
{
  Hypergraph graph;
  // 4 vertices, 3 edges; edge 1 repeats vertex 2 (multi-pin membership,
  // mirroring one-entry-per-dbITerm), edge 2 is a 2-pin edge.
  graph.buildFromTopology(4, {{0, 1, 2}, {2, 2, 3}, {0, 3}});

  EXPECT_EQ(graph.numVertices(), 4);
  EXPECT_EQ(graph.numHyperedges(), 3);
  EXPECT_EQ(graph.numPins(), 8);
  EXPECT_EQ(graph.hyperedgeOffsets(), (std::vector<int>{0, 3, 6, 8}));
  EXPECT_EQ(graph.pinList(), (std::vector<int>{0, 1, 2, 2, 2, 3, 0, 3}));

  // Transpose: per-vertex incident-edge slices, sorted, with the repeated
  // pin of vertex 2 on edge 1 appearing twice.
  EXPECT_EQ(graph.vertexOffsets(), (std::vector<int>{0, 2, 3, 6, 8}));
  EXPECT_EQ(graph.vertexPinList(), (std::vector<int>{0, 2, 0, 0, 1, 1, 1, 2}));

  // No dbBlock behind this graph: every dbId lookup fails soft.
  EXPECT_FALSE(graph.vertexId(0).isValid());
  EXPECT_FALSE(graph.hyperedgeId(0).isValid());
  EXPECT_EQ(graph.vertexIndex(odb::dbId<odb::dbInst>(1)),
            Hypergraph::kInvalidIndex);
  EXPECT_EQ(graph.hyperedgeIndex(odb::dbId<odb::dbNet>(1)),
            Hypergraph::kInvalidIndex);

  // Planes size to the procedural counts and die on rebuild as usual.
  EXPECT_EQ(graph.vertexDoublePlane("area").size(), 4u);
  EXPECT_EQ(graph.hyperedgeDoublePlane("weight").size(), 3u);
  graph.buildFromTopology(2, {{0, 1}});
  EXPECT_FALSE(graph.hasVertexPlane("area"));
  EXPECT_EQ(graph.numVertices(), 2);
}

TEST(HypergraphTopologyTest, BuildFromTopologySkipsBadPins)
{
  utl::Logger logger;
  auto stream = std::make_shared<std::ostringstream>();
  logger.addSink(std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream));

  Hypergraph graph(&logger);
  graph.buildFromTopology(3, {{0, 5, 2}, {-1, 1}});

  // Bad pins dropped, edges kept, warning emitted.
  EXPECT_EQ(graph.numHyperedges(), 2);
  EXPECT_EQ(graph.pinList(), (std::vector<int>{0, 2, 1}));
  EXPECT_NE(stream->str().find("pin skipped"), std::string::npos);
}

TEST(HypergraphTopologyTest, ConstDoublePlaneFinders)
{
  Hypergraph graph;
  graph.buildFromTopology(3, {{0, 1}, {1, 2}});
  const Hypergraph& cgraph = graph;

  // Absent: probe returns nullptr and creates nothing.
  EXPECT_EQ(cgraph.findHyperedgeDoublePlane("weight"), nullptr);
  EXPECT_FALSE(graph.hasHyperedgePlane("weight"));

  // Present as double: probe returns the exact same storage.
  std::vector<double>& weight = graph.hyperedgeDoublePlane("weight");
  EXPECT_EQ(cgraph.findHyperedgeDoublePlane("weight"), &weight);

  // Bound to a different type: probe says nullptr, no diagnostic games.
  graph.vertexIntPlane("area");
  EXPECT_EQ(cgraph.findVertexDoublePlane("area"), nullptr);
  EXPECT_EQ(cgraph.findVertexDoublePlane("weight"), nullptr);  // vertex side
}

}  // namespace
}  // namespace eda
