// Tests for programmatic netlist construction (netlistgen) feeding the
// hypergraph model. No LEF/DEF data is needed — that is the point.

#include <vector>

#include "gtest/gtest.h"
#include "hypergraph/hypergraph.h"
#include "engines/netlistgen/netlistgen.h"
#include "odb/db.h"

namespace eda {
namespace {

// Hand-built 3-inst / 2-net netlist where the full CSR contents are known:
//
//   u0 (INV)   o0 -> n0 -> u1.i0, u2.i0
//   u1 (NAND2) o0 -> n1 -> u2.i1
//   u2 (NAND2)
TEST(NetlistBuilderTest, ExactTopology)
{
  NetlistBuilder nb("tiny");
  odb::dbMaster* inv = nb.makeMaster("INV", 1, 1);
  odb::dbMaster* nand2 = nb.makeMaster("NAND2", 2, 1);
  ASSERT_NE(inv, nullptr);
  ASSERT_NE(nand2, nullptr);

  odb::dbInst* u0 = nb.makeInst(inv, "u0");
  odb::dbInst* u1 = nb.makeInst(nand2, "u1");
  odb::dbInst* u2 = nb.makeInst(nand2, "u2");
  odb::dbNet* n0 = nb.makeNet("n0");
  odb::dbNet* n1 = nb.makeNet("n1");

  EXPECT_TRUE(NetlistBuilder::connect(u0, "o0", n0));
  EXPECT_TRUE(NetlistBuilder::connect(u1, "i0", n0));
  EXPECT_TRUE(NetlistBuilder::connect(u2, "i0", n0));
  EXPECT_TRUE(NetlistBuilder::connect(u1, "o0", n1));
  EXPECT_TRUE(NetlistBuilder::connect(u2, "i1", n1));
  EXPECT_FALSE(NetlistBuilder::connect(u0, "i7", n1));  // no such pin

  Hypergraph hg;
  hg.buildFromBlock(nb.block());

  EXPECT_EQ(hg.numVertices(), 3);
  EXPECT_EQ(hg.numHyperedges(), 2);
  EXPECT_EQ(hg.numPins(), 5);

  // Vertex/hyperedge indices follow dbSet iteration order = creation order.
  const int v0 = hg.vertexIndex(odb::dbId<odb::dbInst>(u0->getId()));
  const int v1 = hg.vertexIndex(odb::dbId<odb::dbInst>(u1->getId()));
  const int v2 = hg.vertexIndex(odb::dbId<odb::dbInst>(u2->getId()));
  const int e0 = hg.hyperedgeIndex(odb::dbId<odb::dbNet>(n0->getId()));
  const int e1 = hg.hyperedgeIndex(odb::dbId<odb::dbNet>(n1->getId()));

  auto edge_verts = [&](int e) {
    std::vector<int> v(hg.pinList().begin() + hg.hyperedgeOffsets()[e],
                       hg.pinList().begin() + hg.hyperedgeOffsets()[e + 1]);
    std::sort(v.begin(), v.end());
    return v;
  };
  EXPECT_EQ(edge_verts(e0), (std::vector<int>{v0, v1, v2}));
  EXPECT_EQ(edge_verts(e1), (std::vector<int>{v1, v2}));

  auto vert_edges = [&](int v) {
    return std::vector<int>(
        hg.vertexPinList().begin() + hg.vertexOffsets()[v],
        hg.vertexPinList().begin() + hg.vertexOffsets()[v + 1]);
  };
  EXPECT_EQ(vert_edges(v0), (std::vector<int>{e0}));
  EXPECT_EQ(vert_edges(v1), (std::vector<int>{e0, e1}));
  EXPECT_EQ(vert_edges(v2), (std::vector<int>{e0, e1}));
}

SyntheticNetlistSpec typicalSpec()
{
  SyntheticNetlistSpec spec;
  spec.masters = {{"INV", 1, 1, 1.0},
                  {"NAND2", 2, 1, 2.0},
                  {"AOI22", 4, 1, 1.0},
                  {"DFF", 2, 2, 0.5}};
  spec.num_insts = 2000;
  spec.min_fanout = 2;
  spec.max_fanout = 6;
  spec.seed = 42;
  return spec;
}

TEST(SyntheticNetlistTest, RespectsSpec)
{
  NetlistBuilder nb;
  const SyntheticNetlistSpec spec = typicalSpec();
  const int nets_made = generateSynthetic(nb, spec);

  odb::dbBlock* block = nb.block();
  EXPECT_EQ(static_cast<int>(block->getInsts().size()), spec.num_insts);
  EXPECT_EQ(static_cast<int>(block->getNets().size()), nets_made);
  EXPECT_GT(nets_made, 0);

  Hypergraph hg;
  hg.buildFromBlock(block);
  EXPECT_EQ(hg.numVertices(), spec.num_insts);
  EXPECT_EQ(hg.numHyperedges(), nets_made);

  // Every net has one driver plus [min_fanout, max_fanout] sink pins (fanout
  // excludes the driver), so its total pin count lies in
  // [min_fanout+1, max_fanout+1]. The last nets may be short of sinks if the
  // pool drained, but always keep the driver plus >= 1 sink, so >= 2 holds
  // uniformly; check the upper bound hard.
  for (int e = 0; e < hg.numHyperedges(); ++e) {
    const int pins = hg.hyperedgeOffsets()[e + 1] - hg.hyperedgeOffsets()[e];
    EXPECT_GE(pins, 2) << "net " << e;
    EXPECT_LE(pins, spec.max_fanout + 1) << "net " << e;
  }

  // No pin is reused: total hypergraph pins == connected iterms.
  int connected_iterms = 0;
  for (odb::dbNet* net : block->getNets()) {
    connected_iterms += net->getITerms().size();
  }
  EXPECT_EQ(hg.numPins(), connected_iterms);
}

TEST(SyntheticNetlistTest, NetCountLimit)
{
  NetlistBuilder nb;
  SyntheticNetlistSpec spec = typicalSpec();
  spec.num_insts = 200;
  spec.num_nets = 25;
  EXPECT_EQ(generateSynthetic(nb, spec), 25);
  EXPECT_EQ(nb.block()->getNets().size(), 25u);
}

TEST(SyntheticNetlistTest, DeterministicForSeed)
{
  const SyntheticNetlistSpec spec = typicalSpec();

  auto build = [&spec](uint32_t seed) {
    SyntheticNetlistSpec s = spec;
    s.seed = seed;
    s.num_insts = 500;
    NetlistBuilder nb;
    generateSynthetic(nb, s);
    Hypergraph hg;
    hg.buildFromBlock(nb.block());
    return std::make_pair(hg.hyperedgeOffsets(), hg.pinList());
  };

  EXPECT_EQ(build(7), build(7));
  EXPECT_NE(build(7), build(8));
}

}  // namespace
}  // namespace eda
