# Spike C1 — Post-Implementation Audit

Run this after C1 is committed. Every check below must pass before proceeding to C2/C3.

## 1. Directory structure

```bash
ls src/hg_metrics/
# Expected: congestion_metrics.h  congestion_metrics.cpp
#           timing_metrics.h      timing_metrics.cpp
#           README.md             FLOW.md
```

Fail if any of the six files is missing. `CMakeLists.txt` must NOT be present — the repo uses
the monolithic root `CMakeLists.txt` only.

## 2. Root CMakeLists wiring

```bash
grep -n "congestion_metrics\|timing_metrics\|hg_metrics" CMakeLists.txt
```

Verify:
- `add_library(hg_metrics ...)` lists both `congestion_metrics.cpp` and `timing_metrics.cpp`.
- `add_executable(hg_metrics_congestion_test test/hg_metrics_congestion_test.cpp)` is present.
- `add_test(NAME hg_metrics_congestion_test ...)` is present.

## 3. Test file location

```bash
ls test/hg_metrics_congestion_test.cpp
```

Must exist. No test file may live under `tests/` or `src/`.

## 4. Build clean

```bash
cmake -B build
cmake --build build 2>&1 | grep -i "warning\|error"
```

- Zero errors.
- Zero warnings (including in `timing_metrics.h/.cpp` stubs).

## 5. All C1 tests green

```bash
ctest --test-dir build --output-on-failure -R hg_metrics_congestion_test
```

All five C1 test cases must pass:
- Empty hypergraph (no crash, empty returns)
- Single vertex no hyperedges (degree histogram = {0:1})
- Known small hypergraph (4 vertices, 3 hyperedges: histogram exact, max=4)
- High-fanout threshold (≥3 pins returned; threshold=99 returns empty)
- Star hypergraph (central vertex degree = num_hyperedges)

## 6. Full test suite still green

```bash
ctest --test-dir build --output-on-failure
```

No regressions in any pre-existing test.

## 7. API shape

```bash
grep -n "DistributionStats\|vertex_degree_histogram\|vertex_degree_stats\
\|hyperedge_size_histogram\|hyperedge_size_stats\|high_fanout_nets\|namespace hgm" \
  src/hg_metrics/congestion_metrics.h
```

Verify all six symbols are declared inside `namespace hgm`.
`HyperedgeId` in `high_fanout_nets` return type should be `int` (the dense local index) — the
hypergraph uses raw `int` indices, not a type alias. If the implementation used a different
spelling, confirm it resolves to `int`.

## 8. Attribute planes

```bash
grep -n "hgm\." src/hg_metrics/congestion_metrics.cpp
```

C1 functions must write **no** attribute planes (all are read-only). Verify zero matches for
`vertexIntPlane\|vertexDoublePlane\|vertexBoolPlane\|hyperedgeIntPlane` in `congestion_metrics.cpp`.

## 9. Logging convention

```bash
grep -n "printf\|cout\|cerr\|fprintf" src/hg_metrics/congestion_metrics.cpp
```

Zero matches. All messages must go through `utl::Logger` / `debugPrint`.

## 10. README and FLOW.md completeness

- `src/hg_metrics/README.md`: must describe the module purpose, the three public function groups
  (degree, fanout, high-fanout), the `"hgm."` plane naming convention, and how to build/test.
- `src/hg_metrics/FLOW.md`: must contain at least one Mermaid diagram per function group and
  one module-level diagram. Validate Mermaid syntax with a preview if available.

## Pass Criteria

All 10 checks green → C1 is complete, C2 and C3 may begin in parallel.
Any failure → fix before proceeding.
