// See hypergraph.h for the CSR layout and dbId-vs-index semantics.

#include "hypergraph/hypergraph.h"

#include <algorithm>

#include "odb/db.h"
#include "support/logging.h"
#include "utl/Logger.h"

namespace eda {

namespace {
// Debug group for buildFromBlock/buildFromTopology verbosity. These are
// library entry points with no CLI flag of their own; their phase markers are
// debug-gated (group "hypergraph"), so an in-memory caller sees nothing at
// verbosity 0 and a CLI that raises verbosity surfaces the build trace. See
// support/logging.h for the level scheme.
constexpr const char* kGroup = "hypergraph";
}  // namespace

const char* Hypergraph::planeTypeName(const PlaneType type)
{
  switch (type) {
    case PlaneType::kDouble:
      return "double";
    case PlaneType::kInt:
      return "int";
    case PlaneType::kBool:
      return "bool";
  }
  return "?";
}

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

  // Planes parallel the element counts this function just reset, so they
  // die here — the one choke point both clear() and buildFromBlock() (via
  // clear()) pass through, which is what enforces the invalidation rule.
  clearAllPlanes();
}

void Hypergraph::buildFromBlock(odb::dbBlock* block)
{
  clear();

  if (logger_ != nullptr) {
    debugPrint(logger_, utl::UKN, kGroup, kVerbosityDetail,
               "buildFromBlock: instance scan");
  }

  // Pass 1: number the vertices before any pin is recorded, so pass 2 can
  // translate an iterm's owning dbInst id to a vertex index immediately.
  for (odb::dbInst* inst : block->getInsts()) {
    const odb::dbId<odb::dbInst> id(inst->getId());
    vertex_index_[id] = num_vertices_;
    vertex_ids_.push_back(id);
    ++num_vertices_;
  }

  if (logger_ != nullptr) {
    debugPrint(logger_, utl::UKN, kGroup, kVerbosityDetail,
               "buildFromBlock: {} vertices numbered; net scan + "
               "hyperedge-major CSR",
               num_vertices_);
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
    if (logger_ != nullptr && num_hyperedges_ < kTraceCap) {
      const int degree = static_cast<int>(pin_list_.size())
                         - hyperedge_offsets_[num_hyperedges_];
      debugPrint(logger_, utl::UKN, kGroup, kVerbosityTrace,
                 "  net[{}] '{}' -> {} pins{}", num_hyperedges_, net->getName(),
                 degree,
                 num_hyperedges_ + 1 == kTraceCap ? " (per-net trace capped)"
                                                  : "");
    }
    ++num_hyperedges_;
  }

  if (logger_ != nullptr) {
    debugPrint(logger_, utl::UKN, kGroup, kVerbosityDetail,
               "buildFromBlock: {} hyperedges, {} pins; transposing to "
               "vertex-major CSR",
               num_hyperedges_, static_cast<int>(pin_list_.size()));
  }
  buildVertexMajor();
  if (logger_ != nullptr) {
    debugPrint(logger_, utl::UKN, kGroup, kVerbosityDetail,
               "buildFromBlock: done ({} vertices, {} hyperedges, {} pins)",
               num_vertices_, num_hyperedges_,
               static_cast<int>(pin_list_.size()));
  }
}

void Hypergraph::buildFromTopology(
    const int num_vertices,
    const std::vector<std::vector<int>>& hyperedges)
{
  clear();
  num_vertices_ = std::max(num_vertices, 0);

  // No dbBlock here, so vertex_ids_/hyperedge_ids_ and the reverse maps
  // stay empty on purpose — that emptiness is what makes every dbId lookup
  // fail soft in procedural mode (see vertexId()/hyperedgeId()).
  if (logger_ != nullptr) {
    debugPrint(logger_, utl::UKN, kGroup, kVerbosityDetail,
               "buildFromTopology: {} vertices, {} hyperedges from explicit "
               "topology",
               num_vertices_, static_cast<int>(hyperedges.size()));
  }

  hyperedge_offsets_.push_back(0);
  for (const std::vector<int>& edge : hyperedges) {
    for (const int v : edge) {
      if (v < 0 || v >= num_vertices_) {
        // Fail soft per the no-exceptions contract: drop the pin, keep
        // the edge, diagnose if a logger is attached.
        if (logger_ != nullptr) {
          logger_->warn(utl::UKN,
                        101,
                        "buildFromTopology: hyperedge {} lists vertex {} "
                        "outside [0, {}); pin skipped",
                        num_hyperedges_,
                        v,
                        num_vertices_);
        }
        continue;
      }
      pin_list_.push_back(v);
    }
    hyperedge_offsets_.push_back(static_cast<int>(pin_list_.size()));
    ++num_hyperedges_;
  }

  buildVertexMajor();
  if (logger_ != nullptr) {
    debugPrint(logger_, utl::UKN, kGroup, kVerbosityDetail,
               "buildFromTopology: done ({} vertices, {} hyperedges, {} pins)",
               num_vertices_, num_hyperedges_,
               static_cast<int>(pin_list_.size()));
  }
}

// Pass 3: transpose into the vertex-major CSR with a counting sort
// (degree count, prefix sum, scatter) — O(pins) with no per-vertex
// temporary lists to allocate.
void Hypergraph::buildVertexMajor()
{
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
  // Bounds-check against the id array, not num_vertices_: after
  // buildFromTopology() the array is empty while num_vertices_ is not,
  // and every in-range index must still return the invalid id.
  if (idx < 0 || idx >= static_cast<int>(vertex_ids_.size())) {
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
  // Same soft-fail rule as vertexId() for the procedural-build mode.
  if (idx < 0 || idx >= static_cast<int>(hyperedge_ids_.size())) {
    return odb::dbId<odb::dbNet>();
  }
  return hyperedge_ids_[idx];
}

int Hypergraph::hyperedgeIndex(const odb::dbId<odb::dbNet> id) const
{
  const auto it = hyperedge_index_.find(id);
  return it == hyperedge_index_.end() ? kInvalidIndex : it->second;
}

Hypergraph::Plane& Hypergraph::getOrCreatePlane(PlaneMap& planes,
                                                const std::string& name,
                                                const PlaneType requested,
                                                const char* kind)
{
  const auto [it, inserted] = planes.try_emplace(name, Plane{requested});
  if (!inserted && it->second.type != requested) {
    // Diagnosed on every mismatched access so the bug stays visible.
    // warn, not error: Logger::error() at the pinned SHA throws, and
    // this API guarantees it never will (see hypergraph.h).
    if (logger_ != nullptr) {
      logger_->warn(utl::UKN,
                    100,
                    "{} attribute plane '{}' was created as {} but accessed "
                    "as {}; returning separate {} storage",
                    kind,
                    name,
                    planeTypeName(it->second.type),
                    planeTypeName(requested),
                    planeTypeName(requested));
    }
  }
  return it->second;
}

std::vector<double>& Hypergraph::vertexDoublePlane(const std::string& name)
{
  Plane& plane
      = getOrCreatePlane(vertex_planes_, name, PlaneType::kDouble, "vertex");
  if (!plane.doubles) {
    plane.doubles.emplace(num_vertices_, 0.0);
  }
  return *plane.doubles;
}

std::vector<int>& Hypergraph::vertexIntPlane(const std::string& name)
{
  Plane& plane
      = getOrCreatePlane(vertex_planes_, name, PlaneType::kInt, "vertex");
  if (!plane.ints) {
    plane.ints.emplace(num_vertices_, 0);
  }
  return *plane.ints;
}

std::vector<bool>& Hypergraph::vertexBoolPlane(const std::string& name)
{
  Plane& plane
      = getOrCreatePlane(vertex_planes_, name, PlaneType::kBool, "vertex");
  if (!plane.bools) {
    plane.bools.emplace(num_vertices_, false);
  }
  return *plane.bools;
}

std::vector<double>& Hypergraph::hyperedgeDoublePlane(const std::string& name)
{
  Plane& plane = getOrCreatePlane(
      hyperedge_planes_, name, PlaneType::kDouble, "hyperedge");
  if (!plane.doubles) {
    plane.doubles.emplace(num_hyperedges_, 0.0);
  }
  return *plane.doubles;
}

std::vector<int>& Hypergraph::hyperedgeIntPlane(const std::string& name)
{
  Plane& plane
      = getOrCreatePlane(hyperedge_planes_, name, PlaneType::kInt, "hyperedge");
  if (!plane.ints) {
    plane.ints.emplace(num_hyperedges_, 0);
  }
  return *plane.ints;
}

std::vector<bool>& Hypergraph::hyperedgeBoolPlane(const std::string& name)
{
  Plane& plane = getOrCreatePlane(
      hyperedge_planes_, name, PlaneType::kBool, "hyperedge");
  if (!plane.bools) {
    plane.bools.emplace(num_hyperedges_, false);
  }
  return *plane.bools;
}

// The two const finders are probes, not accesses: they never create,
// never warn, and return nullptr unless the name is bound to double.
// (A slot whose type is kDouble always has `doubles` engaged — first
// creation emplaces it in the same accessor call that pins the type.)
const std::vector<double>* Hypergraph::findVertexDoublePlane(
    const std::string& name) const
{
  const auto it = vertex_planes_.find(name);
  if (it == vertex_planes_.end() || it->second.type != PlaneType::kDouble) {
    return nullptr;
  }
  return &*it->second.doubles;
}

const std::vector<double>* Hypergraph::findHyperedgeDoublePlane(
    const std::string& name) const
{
  const auto it = hyperedge_planes_.find(name);
  if (it == hyperedge_planes_.end() || it->second.type != PlaneType::kDouble) {
    return nullptr;
  }
  return &*it->second.doubles;
}

bool Hypergraph::hasVertexPlane(const std::string& name) const
{
  return vertex_planes_.count(name) > 0;
}

bool Hypergraph::hasHyperedgePlane(const std::string& name) const
{
  return hyperedge_planes_.count(name) > 0;
}

void Hypergraph::removeVertexPlane(const std::string& name)
{
  vertex_planes_.erase(name);
}

void Hypergraph::removeHyperedgePlane(const std::string& name)
{
  hyperedge_planes_.erase(name);
}

void Hypergraph::clearAllPlanes()
{
  vertex_planes_.clear();
  hyperedge_planes_.clear();
}

}  // namespace eda
