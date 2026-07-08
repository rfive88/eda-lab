# src/hypergraph/

`eda::Hypergraph` — a rebuild-on-demand hypergraph view of an
`odb::dbBlock` netlist. Vertices are `dbInst`s, hyperedges are `dbNet`s,
and one pin is recorded per `dbITerm` (an instance with several pins on a
net appears once per pin). There is no incremental sync with the database:
after the netlist changes, call `buildFromBlock()` again.

## Design

**Two identifier spaces.**

- `dbId<T>` — the stable OpenDB id. Survives rebuilds and database
  save/load; use it to refer to an instance/net across rebuilds or to get
  back to the OpenDB object.
- local index — a dense `[0, n)` position assigned in dbSet iteration order
  at build time. Only valid until the next rebuild; used to address the
  flat arrays below.

`vertexId`/`vertexIndex` and `hyperedgeId`/`hyperedgeIndex` translate
between the two. Bad lookups return an invalid `dbId` (0) or
`Hypergraph::kInvalidIndex` — nothing here throws.

**Dual CSR topology.** Two mirrored compressed-sparse-row arrays:
hyperedge-major (`hyperedgeOffsets()`/`pinList()`: edge → member vertices)
and vertex-major (`vertexOffsets()`/`vertexPinList()`: vertex → incident
edges). Memory is doubled to get cache-friendly traversal in both
directions, the access pattern partitioning/clustering kernels need.

## Attribute planes

Named side arrays for engines — one value per vertex or per hyperedge,
parallel to the CSR arrays. The element type menu is fixed (`double`,
`int`, `bool`; no templates), so a plane's type is always knowable:

- `vertexDoublePlane(name)` / `vertexIntPlane(name)` /
  `vertexBoolPlane(name)` → `std::vector<T>&` sized `numVertices()`.
- `hyperedgeDoublePlane` / `hyperedgeIntPlane` / `hyperedgeBoolPlane` →
  sized `numHyperedges()`.
- `hasVertexPlane` / `hasHyperedgePlane`, `removeVertexPlane` /
  `removeHyperedgePlane`, `clearAllPlanes()`.

Semantics:

- **On-demand creation.** Accessing a name that doesn't exist creates the
  plane, zero-initialized (`0.0` / `0` / `false`).
- **Local-index addressing.** Planes are indexed by local index, never by
  `dbId`.
- **Rebuild invalidation.** Planes are valid only for the topology snapshot
  they were created against. `buildFromBlock()` and `clear()` destroy every
  plane — enforced in code, not by convention. Never cache a plane
  reference across a rebuild.
- **Type binding.** A name is bound to the type it was first created with.
  A different-typed access is a caller bug: it logs a `utl::Logger` warning
  (UKN-0100) and returns separate valid storage of the requested type,
  leaving the original plane untouched. Pass a logger via
  `Hypergraph(utl::Logger*)` to see the diagnostic; the API itself never
  throws.
- **Independent namespaces.** A vertex plane and a hyperedge plane may
  share a name without colliding.

## Use model

Building from a block:

```cpp
eda::Hypergraph graph(&logger);   // logger optional, diagnostics only
graph.buildFromBlock(block);
```

Iterating topology (vertices on a hyperedge, then edges on a vertex):

```cpp
const auto& eoff = graph.hyperedgeOffsets();
const auto& pins = graph.pinList();
for (int e = 0; e < graph.numHyperedges(); ++e) {
  for (int p = eoff[e]; p < eoff[e + 1]; ++p) {
    int v = pins[p];  // vertex on hyperedge e
  }
}

const auto& voff = graph.vertexOffsets();
const auto& vpins = graph.vertexPinList();
for (int p = voff[v]; p < voff[v + 1]; ++p) {
  int e = vpins[p];  // hyperedge on vertex v
}
```

Creating and using a weight plane:

```cpp
std::vector<double>& weight = graph.vertexDoublePlane("weight");
weight[graph.vertexIndex(inst_id)] = 2.0;   // local index, not dbId
// ... engine reads graph.vertexDoublePlane("weight") later ...
graph.buildFromBlock(block);                // plane is now gone
```

## Invariants a consumer must respect

- Local indices, CSR array contents, and every attribute plane die on
  `buildFromBlock()`/`clear()`. Re-translate stable `dbId`s afterwards.
- Index planes with local indices only; never resize a plane vector.
- Don't mutate the topology arrays (they are exposed `const` for a reason).
- One CSR entry per `dbITerm`: a hyperedge's pin slice corresponds 1:1 with
  `dbNet::getITerms()`, so vertices can repeat within a slice.

Tests live in `test/hypergraph_test.cpp`.
