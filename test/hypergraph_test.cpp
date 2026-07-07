// Tests for the eda-lab hypergraph netlist model (Phase 0, Item 3).

#include <algorithm>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "hypergraph/hypergraph.h"
#include "odb/db.h"
#include "odb/defin.h"
#include "odb/lefin.h"
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

    odb::defin def_reader(db_, logger_);
    std::vector<odb::dbLib*> search_libs = {tech_lib, cell_lib};
    def_reader.readChip(search_libs,
                        (data_dir + "/gcd_nangate45.def").c_str(),
                        nullptr,
                        false);
    ASSERT_NE(db_->getChip(), nullptr);
    block_ = db_->getChip()->getBlock();
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

}  // namespace
}  // namespace eda
