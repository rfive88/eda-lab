# Spike Brief C1 — Directory Scaffold + Degree/Fanout Distributions

## Goal

Create the `src/hg_metrics/` module from scratch, wire it into the build system, and implement
the first congestion metric group: vertex degree distributions, hyperedge size (fanout)
distributions, and high-fanout net identification. This brief establishes all directory
structure, CMake plumbing, README, and FLOW.md that subsequent briefs (C2–C4, T0–T4) depend on.

## Context

- Repo conventions: `utl::Logger`-based verbosity, `Status`-based error propagation, all test/run
  output to `run/`, every directory carries a `README.md` and `FLOW.md`.
- Hypergraph is in `src/hypergraph/`. The `Hypergraph` class exposes CSR-style flat arrays and a
  named attribute plane layer (`double`/`int`/`bool` planes on vertices and hyperedges).
- Attribute plane naming convention for this module: prefix `"hgm."` on all plane names written
  by hg_metrics functions.
- `src/hg_metrics/timing_metrics.h/.cpp` should be created as stubs (header guard + TODO) so the
  build is complete from day one.
- Do not read or modify anything under `src/hypergraph/` or `src/engines/netlistgen/`.

## Files to Create

```
src/hg_metrics/
├── CMakeLists.txt
├── README.md
├── FLOW.md
├── congestion_metrics.h
├── congestion_metrics.cpp
├── timing_metrics.h          ← stub only
└── timing_metrics.cpp        ← stub only

tests/hg_metrics/
├── CMakeLists.txt
└── congestion_metrics_test.cpp
```

## Files to Modify

- `src/CMakeLists.txt` — add `add_subdirectory(hg_metrics)`
- `tests/CMakeLists.txt` — add `add_subdirectory(hg_metrics)`

## API

```cpp
// src/hg_metrics/congestion_metrics.h
#pragma once
#include "hypergraph/hypergraph.h"
#include <map>
#include <vector>

namespace hgm {

struct DistributionStats {
    double mean;
    double p90;   // 90th percentile
    double p99;   // 99th percentile
    int    max;
};

// --- Vertex degree ---

// Returns histogram: degree -> vertex count
std::map<int,int>    vertex_degree_histogram(const Hypergraph& hg);
DistributionStats    vertex_degree_stats(const Hypergraph& hg);

// --- Hyperedge size (fanout) ---

// Returns histogram: pin count -> hyperedge count
std::map<int,int>    hyperedge_size_histogram(const Hypergraph& hg);
DistributionStats    hyperedge_size_stats(const Hypergraph& hg);

// --- High-fanout nets ---

// Returns IDs of hyperedges whose pin count exceeds threshold (inclusive)
std::vector<HyperedgeId> high_fanout_nets(const Hypergraph& hg, int threshold);

} // namespace hgm
```

`DistributionStats` is shared across congestion and (later) timing headers — define it once in
`congestion_metrics.h` and include from `timing_metrics.h`.

## Implementation Notes

- Vertex degree: for each vertex, count incident hyperedges from the CSR adjacency. No mutation
  of the hypergraph.
- Hyperedge size: for each hyperedge, read its pin count from the CSR structure.
- `DistributionStats::p90` / `p99`: sort the values, index at 90th/99th percentile position.
  Use `std::nth_element` for efficiency.
- `high_fanout_nets`: linear scan over hyperedges comparing pin count to threshold. Threshold is
  caller-supplied; no default — force the caller to be explicit.
- No attribute planes are written in this brief. All functions are `const Hypergraph&` read-only.
- Use `utl::Logger` at verbosity level 3 to log histogram summary (mean, p99, max) when computed.

## Test Requirements

Cover all of the following in `congestion_metrics_test.cpp`:

1. **Empty hypergraph** — all functions return empty maps / zero stats / empty vectors without crash.
2. **Single vertex, no hyperedges** — degree histogram is `{0: 1}`, size histogram empty.
3. **Known small hypergraph** (construct by hand: 4 vertices, 3 hyperedges of sizes 2, 3, 4):
   - Verify histogram counts exactly.
   - Verify `max` in stats equals 4.
   - Verify `p90` and `p99` are within the known distribution.
4. **High-fanout threshold**:
   - threshold = 3 → returns only hyperedges with ≥ 3 pins.
   - threshold = 99 → returns empty vector on the small test graph.
5. **Degree distribution**: a star hypergraph (one vertex in all hyperedges) has that vertex with
   degree = num_hyperedges; verify histogram reflects this.

All tests must pass under `ctest` before proceeding to C2.

## Deliverables Checklist

- [ ] `src/hg_metrics/` directory with all files above
- [ ] `tests/hg_metrics/` directory with CMakeLists and test file
- [ ] Both added to parent CMakeLists files
- [ ] `timing_metrics.h/.cpp` stubs compile cleanly (no warnings)
- [ ] All C1 gtest cases green
- [ ] `src/hg_metrics/README.md` written (module purpose, public API summary, build instructions)
- [ ] `src/hg_metrics/FLOW.md` written (per-function flow diagrams + module-level diagram)
- [ ] Committed in a single commit with message `hg_metrics: C1 scaffold + degree/fanout distributions`

## Hard Gate

Do not proceed to C2, C3, C4, or any timing brief until all tests in `tests/hg_metrics/` are
green under `ctest` and the commit is clean.
