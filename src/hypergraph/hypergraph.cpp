// See hypergraph.h for the CSR layout and dbId-vs-index semantics.

#include "hypergraph/hypergraph.h"

#include "odb/db.h"

namespace eda {

void Hypergraph::clear()
{
  num_vertices_ = 0;
  num_hyperedges_ = 0;
  vertex_ids_.clear();
  hyperedge_ids_.clear();
  vertex_index_.clear();
  hyperedge_index_.clear();
  hyperedge_offsets_.clear();
  pin_list_.clear();
  vertex_offsets_.clear();
  vertex_pin_list_.clear();
}

void Hypergraph::buildFromBlock(odb::dbBlock* block)
{
  clear();

  // Pass 1: number the vertices before any pin is recorded, so pass 2 can
  // translate an iterm's owning dbInst id to a vertex index immediately.
  for (odb::dbInst* inst : block->getInsts()) {
    const odb::dbId<odb::dbInst> id(inst->getId());
    vertex_index_[id] = num_vertices_;
    vertex_ids_.push_back(id);
    ++num_vertices_;
  }

  // Pass 2: stream the hyperedge-major CSR directly — nets arrive in
  // iteration order, so each net's pins land contiguously and the offset
  // is just the running pin count. One pin per dbITerm; an instance with
  // several pins on the same net appears once per pin so the pin slice
  // stays in one-to-one correspondence with dbNet::getITerms().
  hyperedge_offsets_.push_back(0);
  for (odb::dbNet* net : block->getNets()) {
    const odb::dbId<odb::dbNet> id(net->getId());
    hyperedge_index_[id] = num_hyperedges_;
    hyperedge_ids_.push_back(id);
    for (odb::dbITerm* iterm : net->getITerms()) {
      pin_list_.push_back(
          vertexIndex(odb::dbId<odb::dbInst>(iterm->getInst()->getId())));
    }
    hyperedge_offsets_.push_back(static_cast<int>(pin_list_.size()));
    ++num_hyperedges_;
  }

  // Pass 3: transpose into the vertex-major CSR with a counting sort
  // (degree count, prefix sum, scatter) — O(pins) with no per-vertex
  // temporary lists to allocate.
  vertex_offsets_.assign(num_vertices_ + 1, 0);
  for (const int vertex : pin_list_) {
    ++vertex_offsets_[vertex + 1];
  }
  for (int v = 0; v < num_vertices_; ++v) {
    vertex_offsets_[v + 1] += vertex_offsets_[v];
  }

  // cursor[v] tracks the next free slot in vertex v's slice; iterating
  // edges in order keeps each slice sorted by hyperedge index.
  vertex_pin_list_.assign(pin_list_.size(), 0);
  std::vector<int> cursor(vertex_offsets_.begin(), vertex_offsets_.end() - 1);
  for (int e = 0; e < num_hyperedges_; ++e) {
    for (int p = hyperedge_offsets_[e]; p < hyperedge_offsets_[e + 1]; ++p) {
      vertex_pin_list_[cursor[pin_list_[p]]++] = e;
    }
  }
}

odb::dbId<odb::dbInst> Hypergraph::vertexId(const int idx) const
{
  if (idx < 0 || idx >= num_vertices_) {
    return odb::dbId<odb::dbInst>();
  }
  return vertex_ids_[idx];
}

int Hypergraph::vertexIndex(const odb::dbId<odb::dbInst> id) const
{
  const auto it = vertex_index_.find(id);
  return it == vertex_index_.end() ? kInvalidIndex : it->second;
}

odb::dbId<odb::dbNet> Hypergraph::hyperedgeId(const int idx) const
{
  if (idx < 0 || idx >= num_hyperedges_) {
    return odb::dbId<odb::dbNet>();
  }
  return hyperedge_ids_[idx];
}

int Hypergraph::hyperedgeIndex(const odb::dbId<odb::dbNet> id) const
{
  const auto it = hyperedge_index_.find(id);
  return it == hyperedge_index_.end() ? kInvalidIndex : it->second;
}

}  // namespace eda
