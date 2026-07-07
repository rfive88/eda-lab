// eda-lab: hypergraph netlist model (Phase 0, Item 3).
//
// Vertices are dbInsts and hyperedges are dbNets. Objects are identified
// two ways, with different lifetimes:
//
//   - dbId<T>: the stable OpenDB id. Survives database save/load and is
//     never invalidated by a rebuild. Use it to refer to an instance or
//     net across rebuilds or to get back to the OpenDB object.
//   - index: a dense [0, n) position assigned in dbSet iteration order at
//     build time. Only meaningful for the current build; used to address
//     the flat arrays below and any per-vertex/per-edge side arrays an
//     engine allocates.
//
// Topology is stored CSR-style (compressed sparse row): an offsets array
// of size n+1 brackets each element's slice of a single flat data array,
// so element i's data is data[offsets[i] .. offsets[i+1]). Two mirrored
// CSRs are kept — hyperedge-major (edge -> member vertices) and
// vertex-major (vertex -> incident edges) — trading memory for cache-
// friendly traversal in either direction, the access pattern most
// partitioning/clustering kernels need.
//
// The view is rebuilt on demand from a dbBlock; there is no incremental
// sync with the database (observers can come later if profiling demands).

#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "odb/dbId.h"

namespace odb {
class dbBlock;
class dbInst;
class dbNet;
}  // namespace odb

namespace eda {

class Hypergraph
{
 public:
  static constexpr int kInvalidIndex = -1;

  Hypergraph() = default;

  // Discard any previous contents and rebuild from the block's current
  // netlist. Vertex/hyperedge indices are assigned in dbSet iteration
  // order and are only meaningful until the next rebuild.
  void buildFromBlock(odb::dbBlock* block);
  void clear();

  int numVertices() const { return num_vertices_; }
  int numHyperedges() const { return num_hyperedges_; }
  int numPins() const { return static_cast<int>(pin_list_.size()); }

  // Vertex index <-> dbInst id. vertexId translates a build-local index
  // to the stable OpenDB id (resolve with dbInst::getInst); vertexIndex
  // goes the other way. Out-of-range or unknown lookups return an invalid
  // dbId (0) / kInvalidIndex rather than failing — no exceptions.
  odb::dbId<odb::dbInst> vertexId(int idx) const;
  int vertexIndex(odb::dbId<odb::dbInst> id) const;

  // Hyperedge index <-> dbNet id, same semantics as the vertex pair.
  odb::dbId<odb::dbNet> hyperedgeId(int idx) const;
  int hyperedgeIndex(odb::dbId<odb::dbNet> id) const;

  // CSR topology, hyperedge-major: the vertices on hyperedge e are
  // pinList()[hyperedgeOffsets()[e] .. hyperedgeOffsets()[e + 1]).
  // One entry per dbITerm, so a vertex appears once per connected pin.
  const std::vector<int>& hyperedgeOffsets() const
  {
    return hyperedge_offsets_;
  }
  const std::vector<int>& pinList() const { return pin_list_; }

  // CSR topology, vertex-major: the hyperedges on vertex v are
  // vertexPinList()[vertexOffsets()[v] .. vertexOffsets()[v + 1]).
  const std::vector<int>& vertexOffsets() const { return vertex_offsets_; }
  const std::vector<int>& vertexPinList() const { return vertex_pin_list_; }

 private:
  int num_vertices_ = 0;
  int num_hyperedges_ = 0;

  // index -> stable OpenDB id
  std::vector<odb::dbId<odb::dbInst>> vertex_ids_;
  std::vector<odb::dbId<odb::dbNet>> hyperedge_ids_;

  // stable OpenDB id -> index
  std::unordered_map<odb::dbId<odb::dbInst>, int> vertex_index_;
  std::unordered_map<odb::dbId<odb::dbNet>, int> hyperedge_index_;

  // CSR arrays (see accessors above)
  std::vector<int> hyperedge_offsets_;  // size numHyperedges() + 1
  std::vector<int> pin_list_;           // size numPins()
  std::vector<int> vertex_offsets_;     // size numVertices() + 1
  std::vector<int> vertex_pin_list_;    // size numPins()
};

}  // namespace eda
