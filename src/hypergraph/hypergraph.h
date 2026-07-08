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
//
// Attribute planes are the standard side-array mechanism for engines:
// a named flat std::vector, parallel to the CSR arrays, holding one
// value per vertex or per hyperedge. Three element types are supported
// (double, int, bool) — a fixed menu rather than a template so callers
// and debuggers always know what a plane holds. Planes are addressed by
// the LOCAL index, never by dbId: they are snapshots of per-element
// data for the current build, exactly like the CSR arrays they run
// parallel to. That also dictates their lifetime — a rebuild reassigns
// indices, so buildFromBlock() and clear() destroy every plane. This is
// enforced in code; never cache a plane reference across a rebuild.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "odb/dbId.h"

namespace odb {
class dbBlock;
class dbInst;
class dbNet;
}  // namespace odb

namespace utl {
class Logger;
}  // namespace utl

namespace eda {

class Hypergraph
{
 public:
  static constexpr int kInvalidIndex = -1;

  // The logger is only used to diagnose attribute-plane type conflicts;
  // a null logger silences the diagnostic but never changes behavior.
  explicit Hypergraph(utl::Logger* logger = nullptr) : logger_(logger) {}

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

  // Attribute planes (see the header comment for the model). Accessing a
  // name that doesn't exist creates the plane on demand: numVertices() /
  // numHyperedges() elements, zero-initialized (0.0 / 0 / false). Index
  // the returned vector with local indices only; never resize it. The
  // reference stays valid until the plane is removed or the graph is
  // rebuilt or cleared — all of which destroy the plane.
  //
  // Type conflicts: a plane name is bound to the element type it was
  // first created with. Accessing it through a different-typed accessor
  // is a caller bug; because this API never throws (and Logger::error()
  // at the pinned SHA does), it is diagnosed as a utl::Logger warning on
  // every mismatched access. The call still returns safe storage — a
  // separate zero-initialized vector of the requested type held in the
  // same slot — so the original plane's data and any outstanding
  // references to it are never disturbed. Vertex and hyperedge planes
  // are independent namespaces; the same name may exist in both.
  std::vector<double>& vertexDoublePlane(const std::string& name);
  std::vector<int>& vertexIntPlane(const std::string& name);
  std::vector<bool>& vertexBoolPlane(const std::string& name);

  std::vector<double>& hyperedgeDoublePlane(const std::string& name);
  std::vector<int>& hyperedgeIntPlane(const std::string& name);
  std::vector<bool>& hyperedgeBoolPlane(const std::string& name);

  bool hasVertexPlane(const std::string& name) const;
  bool hasHyperedgePlane(const std::string& name) const;
  void removeVertexPlane(const std::string& name);
  void removeHyperedgePlane(const std::string& name);
  void clearAllPlanes();

 private:
  // One slot per plane name. `type` records the type of first creation
  // and only exists to detect mismatched accesses; the per-type vectors
  // are optional so a slot pays for exactly the storage it uses (and so
  // "created when the graph was empty" is distinguishable from "absent").
  enum class PlaneType
  {
    kDouble,
    kInt,
    kBool
  };
  struct Plane
  {
    PlaneType type;
    std::optional<std::vector<double>> doubles;
    std::optional<std::vector<int>> ints;
    std::optional<std::vector<bool>> bools;
  };
  using PlaneMap = std::unordered_map<std::string, Plane>;

  // Shared find-or-insert for the six accessors: pins the slot's type on
  // first creation and emits the mismatch diagnostic afterwards. `kind`
  // is "vertex" or "hyperedge", for the message only.
  Plane& getOrCreatePlane(PlaneMap& planes,
                          const std::string& name,
                          PlaneType requested,
                          const char* kind);
  static const char* planeTypeName(PlaneType type);

  int num_vertices_ = 0;
  int num_hyperedges_ = 0;

  utl::Logger* logger_ = nullptr;

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

  // Attribute planes; destroyed by clear()/buildFromBlock(), the only
  // operations that can change the element counts the planes parallel.
  PlaneMap vertex_planes_;
  PlaneMap hyperedge_planes_;
};

}  // namespace eda
