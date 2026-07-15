# Spike Brief T0 — ODB Annotation + TimingDAG Construction

## Goal

Implement two prerequisites that all timing metric functions (T1–T4) depend on:

1. `annotate_timing_attributes` — reads ODB to populate `"driver_vertex"` (int) on hyperedges
   and `"is_register"` (bool) on vertices in the hypergraph.
2. `build_timing_dag` — constructs a directed acyclic graph from the annotated hypergraph,
   performing topological sort with graceful cycle breaking. Returns a `TimingDAG` struct
   consumed by all subsequent timing functions.

## Context

- Prerequisites: C1 green. The `src/hg_metrics/` directory and build wiring exist.
  `timing_metrics.h/.cpp` are currently stubs — this brief replaces them with real content.
- The hypergraph attribute plane mechanism supports named `int` and `bool` planes on both
  vertices and hyperedges. These must already exist or be created by `annotate_timing_attributes`.
- `"driver_vertex"` on a hyperedge: stores the `VertexId` of the vertex that drives that net
  (i.e., the instance pin with OUTPUT direction in ODB). Value `-1` if no driver found.
- `"is_register"` on a vertex: `true` if the corresponding ODB instance has a clock pin
  (i.e., `dbITerm` with `CLOCK` signal type) or its master cell type indicates sequential
  behaviour.
- Cycle breaking: DFS three-color marking (WHITE=unvisited, GRAY=in-progress, BLACK=done).
  A back edge (edge to a GRAY vertex) indicates a cycle. Break it by excluding that hyperedge
  from the DAG and writing `true` to the `"hgm.cycle_edge"` (bool) plane on that hyperedge.
- The expanded DAG uses one directed edge per (driver_vertex → sink_vertex) pair per hyperedge.
  A hyperedge with 1 driver and k sinks expands to k directed edges.
- Do not modify files outside `src/hg_metrics/` and `tests/hg_metrics/`.
- ODB headers are already available via the project's existing third_party/openroad dependency.

## Files to Modify

```
src/hg_metrics/timing_metrics.h    ← replace stub with full content
src/hg_metrics/timing_metrics.cpp  ← replace stub with full content
test/hg_metrics_timing_test.cpp    ← create new file
CMakeLists.txt (root)              ← add hg_metrics_timing_test executable + add_test entry

The root CMakeLists.txt is the only build file. Add the following two blocks
(following the pattern of existing test targets):

  add_executable(hg_metrics_timing_test test/hg_metrics_timing_test.cpp)
  target_link_libraries(hg_metrics_timing_test PRIVATE hg_metrics hypergraph
                                                        odb utl GTest::gtest
                                                        GTest::gtest_main)

  add_test(NAME hg_metrics_timing_test COMMAND hg_metrics_timing_test)
```

## API

```cpp
// src/hg_metrics/timing_metrics.h
#pragma once
#include "hypergraph/hypergraph.h"
#include "congestion_metrics.h"   // for DistributionStats
#include <vector>
#include <string>

// Forward declaration — avoid pulling all ODB headers into this header
namespace odb { class dbBlock; }

namespace hgm {

// --- Prerequisites ---

// Reads ODB block to annotate the hypergraph with:
//   "driver_vertex" (int) on each hyperedge: VertexId of the driving pin, or -1 if absent.
//   "is_register"   (bool) on each vertex:   true if the instance has a clock pin.
// The mapping from ODB net/instance to hyperedge/vertex must be consistent with how the
// hypergraph was originally built from ODB (same ordering assumptions as the hypergraph builder).
// Returns Status::OK on success. Logs warnings for nets with no driver via utl::Logger.
Status annotate_timing_attributes(Hypergraph& hg, odb::dbBlock* block);

// Directed acyclic graph derived from the hypergraph + driver annotations.
struct TimingDAG {
    std::vector<VertexId>              topo_order;   // all vertices in topological order
    std::vector<std::vector<VertexId>> adj;          // adj[u] = list of vertices u drives
    std::vector<std::vector<VertexId>> radj;         // radj[v] = list of vertices that drive v
    int                                broken_edges; // number of cycle edges removed
};

// Builds the TimingDAG from the annotated hypergraph.
// Requires "driver_vertex" plane to be populated (call annotate_timing_attributes first).
// Cycle detection via DFS three-color marking; back edges written to "hgm.cycle_edge" (bool)
// on hyperedges and excluded from adj/radj.
// Source vertices (in-degree 0 in the DAG) appear first in topo_order.
TimingDAG build_timing_dag(Hypergraph& hg);

} // namespace hgm
```

## Implementation Notes

### `annotate_timing_attributes`

```
for each dbNet in block:
    find the corresponding HyperedgeId (by net name or index — use same convention as hypergraph builder)
    find the driver dbITerm (iterm->getIoType() == dbIoType::OUTPUT)
    map driver dbITerm to VertexId (by instance name or index)
    write VertexId to "driver_vertex" plane on hyperedge (write -1 if no driver found)

for each dbInst in block:
    find the corresponding VertexId
    is_reg = any dbITerm on this instance has getSigType() == dbSigType::CLOCK
    write is_reg to "is_register" plane on vertex
```

Log a `utl::Logger` warning (verbosity 1) for each net with no driver. Log a summary count at
verbosity 2.

### `build_timing_dag`

```
1. Expand hyperedges to directed edges:
   for each hyperedge e:
       driver = "driver_vertex"[e]
       if driver == -1 or "hgm.cycle_edge"[e] == true: skip
       for each vertex v in e (v != driver):
           raw_adj[driver].push_back(v)
           raw_radj[v].push_back(driver)

2. DFS cycle detection + topological sort:
   color = vector<WHITE>(num_vertices)
   topo_order = []
   broken = 0

   function dfs(u):
       color[u] = GRAY
       for each v in raw_adj[u]:
           if color[v] == GRAY:    // back edge → cycle
               mark the hyperedge connecting u→v as "hgm.cycle_edge" = true
               remove v from raw_adj[u] (mark skip, don't mutate during iteration)
               broken++
           elif color[v] == WHITE:
               dfs(v)
       color[u] = BLACK
       topo_order.push_front(u)   // prepend for correct order

   for each vertex u with color WHITE: dfs(u)

3. Rebuild adj/radj from raw_adj after removing cycle edges.
4. Populate TimingDAG struct.
```

Use an explicit stack instead of recursion to avoid stack overflow on deep netlists.

## Test Requirements

Create `test/hg_metrics_timing_test.cpp`:

Note: `annotate_timing_attributes` requires a live ODB block — test it with a minimal ODB
fixture if available, or write an integration-style test that loads the GCD DEF. If neither is
practical in the test environment, skip this function's unit test and mark it for manual
verification. Focus unit tests on `build_timing_dag`.

For `build_timing_dag`, build the `TimingDAG` by manually pre-populating the `"driver_vertex"`
attribute plane on a hand-constructed hypergraph (bypassing `annotate_timing_attributes`):

1. **Linear chain** (A→B→C→D): verify topo_order starts with A, ends with D; broken_edges=0;
   adj[A]={B}, adj[B]={C}, adj[C]={D}.

2. **Diamond (reconvergent)** (A→B, A→C, B→D, C→D): verify topo_order has A first, D last;
   broken_edges=0; radj[D]={B,C} (order may vary).

3. **Single cycle** (A→B→C→A): verify broken_edges=1; the remaining DAG is a valid
   topological order (2 vertices after breaking one edge); verify `"hgm.cycle_edge"` is true
   on exactly one hyperedge.

4. **Disconnected components**: two independent chains. Verify topo_order contains all vertices;
   broken_edges=0.

5. **Vertex with no driver** (driver_vertex = -1): hyperedge is skipped; affected vertices still
   appear in topo_order (as sources if no other incoming edges).

All tests must pass under `ctest` before proceeding to T1.

## Deliverables Checklist

- [ ] `timing_metrics.h` fully implemented (no longer a stub)
- [ ] `timing_metrics.cpp` implements `annotate_timing_attributes` and `build_timing_dag`
- [ ] `TimingDAG` struct includes `topo_order`, `adj`, `radj`, `broken_edges`
- [ ] `"hgm.cycle_edge"` bool plane written on broken hyperedges
- [ ] Explicit DFS stack (no recursion)
- [ ] `test/hg_metrics_timing_test.cpp` created and wired into root `CMakeLists.txt`
- [ ] All timing gtest cases green
- [ ] `FLOW.md` updated with DAG construction + cycle-breaking diagram
- [ ] Committed with message `hg_metrics: T0 ODB annotation + TimingDAG`

## Hard Gate

Do not proceed to T1 until all tests in `tests/hg_metrics/` are green and `build_timing_dag`
handles cycles without crashing on pathological inputs.
