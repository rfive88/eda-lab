# Spike Brief SM1 — structural_metrics Engine: Core Library + CLI

## Goal

Build `structural_metrics` — a new engine under `src/engines/` that drives the `hg_metrics`
functions end-to-end. It has two layers following the `netlistgen` / `netlistgen_cli_core`
pattern:

- **`structural_metrics_core`** (library): loads a design into OpenDB, builds the hypergraph,
  runs C1–C5 congestion analysis in sequence, and returns a structured result. Clean API that
  takes an already-built `Hypergraph` — no ODB loading inside the library — so any other
  engine or future OpenROAD integration can call it in-process without spawning a subprocess.

- **`structural_metrics_cli`** (executable): thin `main()` over the core library. Accepts
  LEF+DEF or a native `.odb` file, handles all ODB loading, builds the hypergraph, calls the
  core, and prints a two-section human-readable report to stdout.

Timing analysis (T0–T4) is explicitly out of scope for SM1. The report prints a placeholder
`[ Timing ]` section noting it is not yet available. A follow-up brief (SM2) will fill it in
once the timing track is complete.

## Context

- **Prerequisite: C5 green** (which implies C1–C4 are also green). Do not implement SM1 until
  `hg_metrics_congestion_scoring_test` is passing.
- The repo has one monolithic root `CMakeLists.txt` — no per-directory CMakeLists files.
- ODB loading: LEF via `odb::lefin`, DEF via `odb::defin` (same as `hello_odb.cpp`). Native
  `.odb` via `db->read(std::istream&)` / write via `db->write(std::ostream&)` — both throw
  `ZIOError`; wrap in a try/catch at the call site (layer-2 error handling, same pattern as
  `loadDesign` in `hello_odb.cpp`).
- CLI `--help` / usage convention: use `eda::CliSpec` / `eda::CliOption` from
  `src/support/cli.h` so `--help` and missing-argument errors share one description source.
- Logging: all messages via `utl::Logger`. Tool group `"structural_metrics"`. Message IDs:
  core library 350–374, CLI 375–399. Add both ranges to `CLAUDE.md`'s message-ID table.
- Verbosity levels follow the repo convention (see `src/support/logging.h`):
  - 0: phase markers + final score summary + warnings/errors
  - 1: per-metric summaries (means, p99s, cluster counts)
  - 2: per-cluster member counts
  - 3: per-vertex trace (capped at `eda::kTraceCap`)
- Error handling: three-layer convention — prechecks (layer 1), try/catch at ODB boundary
  (layer 2), top-level catch in `main()` (layer 3). Return `eda::Status`; never crash on bad
  input.
- Do not modify anything under `src/hypergraph/`, `src/hg_metrics/`, or `src/engines/netlistgen/`.

## Files to Create

```
src/engines/structural_metrics/
├── README.md
├── FLOW.md
├── structural_metrics.h        ← core library public API
├── structural_metrics.cpp      ← core library implementation
└── structural_metrics_cli.cpp  ← thin CLI main()

test/structural_metrics_test.cpp
```

## Files to Modify

```
CMakeLists.txt   (root — three additions described below)
CLAUDE.md        (add message-ID ranges 350–374, 375–399)
```

### Root CMakeLists.txt additions

Follow the exact pattern of the `netlistgen_cli_core` / `netlistgen_cli` targets:

```cmake
# Core library: drives hg_metrics C1-C5 analysis; no ODB loading inside.
add_library(structural_metrics_core
  src/engines/structural_metrics/structural_metrics.cpp)
target_include_directories(structural_metrics_core PUBLIC src)
target_link_libraries(structural_metrics_core
                      PUBLIC hg_metrics hypergraph odb utl)

# CLI: thin main() — handles ODB loading and arg parsing.
add_executable(structural_metrics_cli
  src/engines/structural_metrics/structural_metrics_cli.cpp)
target_link_libraries(structural_metrics_cli
                      PRIVATE structural_metrics_core)

# Tests: in-process via netlistgen + library; subprocess CLI smoke tests.
add_executable(structural_metrics_test test/structural_metrics_test.cpp)
target_compile_definitions(structural_metrics_test
  PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
          STRUCTURAL_METRICS_CLI_BIN="$<TARGET_FILE:structural_metrics_cli>")
target_link_libraries(structural_metrics_test
  PRIVATE structural_metrics_core netlistgen hg_metrics hypergraph odb utl
          GTest::gtest GTest::gtest_main)
add_dependencies(structural_metrics_test structural_metrics_cli)

add_test(NAME structural_metrics_test COMMAND structural_metrics_test)
```

## Data Structures

```cpp
// src/engines/structural_metrics/structural_metrics.h
#pragma once
#include "hypergraph/hypergraph.h"
#include "hg_metrics/congestion_metrics.h"    // DistributionStats
#include "hg_metrics/congestion_scoring.h"    // CongestionWeights, CongestionReport
#include "support/status.h"
#include <map>
#include <vector>

namespace utl { class Logger; }

namespace sm {

// Full result of a congestion analysis run.
// All per-vertex attribute planes are written to the hypergraph as a side effect
// of run_congestion_analysis(); this struct carries the summary data.
struct CongestionAnalysisResult {
    // Design dimensions
    int num_vertices;
    int num_hyperedges;

    // C1: Degree + fanout distributions (supporting data)
    std::map<int,int>       degree_histogram;
    hgm::DistributionStats  degree_stats;
    std::map<int,int>       fanout_histogram;
    hgm::DistributionStats  fanout_stats;
    int                     high_fanout_count;      // nets >= hf_threshold
    int                     high_fanout_threshold;  // threshold used

    // C2: K-core (supporting data)
    int                     degeneracy;             // max k-core number

    // C3: Neighborhood density + net intersection (supporting data)
    hgm::DistributionStats  neighborhood_density_stats;
    hgm::DistributionStats  net_intersection_stats;

    // C4: Tangle score (supporting data)
    hgm::DistributionStats  tangle_stats;
    int                     tangle_hot_spot_count;  // vertices with tangle > 0.8

    // C5: Congestion score (main output)
    hgm::CongestionReport   congestion_report;
};

} // namespace sm
```

## Library API

```cpp
// src/engines/structural_metrics/structural_metrics.h (continued)
namespace sm {

// Run full congestion analysis (C1 → C2 → C3 → C4 → C5) on an already-built
// hypergraph. Writes all hgm.* attribute planes as a side effect.
//
// Parameters:
//   hg                  — hypergraph built from a dbBlock (or from topology for tests)
//   logger              — shared logger; may be nullptr (silences all diagnostics)
//   high_fanout_threshold — passed to hgm::high_fanout_nets (C1). Default 20.
//   weights             — passed to hgm::score_congestion (C5). Default equal weights.
//
// Returns a fully populated CongestionAnalysisResult on success.
// Returns eda::Status error (result undefined) if any hg_metrics function fails.
eda::Status run_congestion_analysis(
    eda::Hypergraph&               hg,
    CongestionAnalysisResult&      result_out,
    utl::Logger*                   logger,
    int                            high_fanout_threshold = 20,
    const hgm::CongestionWeights&  weights               = {}
);

// Print the congestion report to stdout via logger->report().
// Section order:
//   1. [ Design ] — instance and net counts
//   2. [ Congestion Score ] — score histogram + top clusters (score >= 4)
//   3. [ Supporting Metrics ] — C1-C4 distribution summaries
//   4. [ Timing ] — placeholder ("not yet available")
void print_congestion_report(
    const CongestionAnalysisResult& result,
    utl::Logger*                    logger
);

} // namespace sm
```

## Implementation Notes

### `run_congestion_analysis`

Call the hg_metrics functions in this exact order, propagating errors via early return:

```
1. C1: degree_histogram    ← hgm::vertex_degree_histogram(hg)
        degree_stats        ← hgm::vertex_degree_stats(hg)
        fanout_histogram    ← hgm::hyperedge_size_histogram(hg)
        fanout_stats        ← hgm::hyperedge_size_stats(hg)
        high_fanout_nets    ← hgm::high_fanout_nets(hg, threshold)
        high_fanout_count   ← size of the returned vector

2. C2: degeneracy          ← hgm::k_core_numbers(hg)
                              (also writes "hgm.k_core" plane)

3. C3: hgm::neighborhood_density(hg)        → writes "hgm.neighborhood_density"
        hgm::one_hop_neighborhood_size(hg)   → writes "hgm.neighborhood_size_1hop"
        hgm::net_intersection_score(hg)      → writes "hgm.net_intersection_score"
        neighborhood_density_stats ← compute DistributionStats over the plane
        net_intersection_stats     ← compute DistributionStats over the plane

4. C4: hgm::tangle_score(hg)               → writes "hgm.tangle_score"
        tangle_stats         ← compute DistributionStats over the plane
        tangle_hot_spot_count ← count vertices with tangle > 0.8

5. C5: hgm::score_congestion(hg, report_out.congestion_report, weights, {})
        → writes "hgm.congestion_score" plane
        → populates congestion_report (histogram + clusters)

6. Populate result_out.num_vertices / num_hyperedges from hg.
```

For steps 3 and 4, `DistributionStats` must be computed from the written plane values — add a
file-scope static helper `stats_from_double_plane(hg, plane_name)` that reads the plane and
calls the same percentile logic as `hgm::vertex_degree_stats` (or factor that logic out of
`congestion_metrics.cpp` into a shared static if both files need it — do NOT duplicate it).

### `print_congestion_report`

Use `logger->report(...)` for all output (no direct `printf`/`cout`). Format:

```
=== Structural Metrics Report ===

[ Design ]
  Instances  : <num_vertices>
  Nets       : <num_hyperedges>

[ Congestion Score ]
  Score 5 (critical) : <n>  (<pct>%)
  Score 4            : <n>  (<pct>%)
  Score 3            : <n>  (<pct>%)
  Score 2            : <n>  (<pct>%)
  Score 1 (clean)    : <n>  (<pct>%)

  Top congestion clusters (score >= 4):
    Cluster <id> : <size> instances, peak=<peak_score>
    ...
  (print only clusters with peak_score >= 4; if none, print "  None")

[ Supporting Metrics ]
  Vertex degree        : mean=<x>  p90=<x>  p99=<x>  max=<x>
  Net fanout           : mean=<x>  p90=<x>  p99=<x>  max=<x>
  High-fanout nets     : <count> (threshold >= <threshold>)
  K-core degeneracy    : <degeneracy>
  Neighborhood density : mean=<x>  p90=<x>  p99=<x>  max=<x>
  Net intersection     : mean=<x>  p90=<x>  p99=<x>  max=<x>
  Tangle score         : mean=<x>  p90=<x>  p99=<x>  max=<x>
  Tangle hot-spots     : <count> instances with score > 0.8

[ Timing ]
  (not yet available — implement T0-T4 to enable)
```

Use fixed-width alignment for the score histogram percentages. Print all floating-point stats
to 2 decimal places.

## CLI Design

### Arguments

```
structural_metrics_cli --lef <path> --def <path> [--hf-threshold <n>] [-verbosity <level>]
structural_metrics_cli --odb <path>               [--hf-threshold <n>] [-verbosity <level>]
```

Input modes are mutually exclusive:
- `--lef` + `--def`: load via `odb::lefin` then `odb::defin` (same pattern as `hello_odb.cpp`).
- `--odb`: load via `std::ifstream` + `db->read(stream)`.

`--lef` and `--def` are both required when either is supplied. Supplying `--odb` with `--lef`
or `--def` is an error. `--hf-threshold` is optional (default 20). `-verbosity` follows the
repo convention via `eda::applyVerbosity()`.

Use `eda::CliSpec` to register all options so `--help` and missing-argument errors render from
the same description source.

### `main()` structure

Follow the three-layer error handling pattern:

```
main():
    try:
        return runStructuralMetricsCli(argc, argv)
    catch (const std::exception& e): stderr + return 1
    catch (...): stderr + return 1

runStructuralMetricsCli(argc, argv):
    parse args with CliSpec; if wantsHelp → printHelp + return 0
    validate: lef+def XOR odb; precheck file existence (layer 1)
    db = dbDatabase::create()
    block = nullptr

    if odb mode:
        try: ifstream + db->read(stream); block = first chip's top block
        catch (ZIOError): log error, return 1
    else:
        try: lefin + defin (same as loadDesign in hello_odb.cpp)
        catch (runtime_error): log error, return 1

    hg = Hypergraph(&logger)
    hg.buildFromBlock(block)

    CongestionAnalysisResult result
    status = sm::run_congestion_analysis(hg, result, &logger, hf_threshold)
    if !status.ok(): log error, return 1

    sm::print_congestion_report(result, &logger)
    return 0
```

### Loading `.odb` files

```cpp
std::ifstream odb_stream(odb_path, std::ios::binary);
if (!odb_stream.is_open())
    return eda::makeError(eda::ErrorCode::kFileNotFound, "cannot open: " + odb_path);
try {
    db->read(odb_stream);
} catch (const std::exception& e) {
    return eda::makeError(eda::ErrorCode::kParseError,
                          std::string("ODB read failed: ") + e.what());
}
// Extract block: db->getChips() → first chip → getBlock()
```

## Test Requirements

Create `test/structural_metrics_test.cpp`.

### In-process tests (no subprocess, no data files)

Build a synthetic netlist in-process using `netlistgen::generateSynthetic`, build a hypergraph
from it with `hg.buildFromTopology`, then call `sm::run_congestion_analysis` directly.

1. **Basic sanity** — 100-vertex synthetic netlist (statistical mix, `sequential_ratio=0.3`):
   - `result.num_vertices` == 100
   - `result.congestion_report.score_histogram` sums to 100
   - All five score bins (1–5) appear (allow missing bins only if N < 5)
   - `result.degeneracy` >= 0
   - `result.tangle_stats.max` <= 1.0 + 1e-6
   - `result.congestion_report.clusters` non-empty

2. **Empty hypergraph** — call on a freshly constructed `Hypergraph` with no topology:
   - `run_congestion_analysis` returns `Status::ok()` without crashing
   - `result.num_vertices == 0`, all histograms empty

3. **Custom high-fanout threshold** — 50-vertex netlist with `max_fanout=30`:
   - Call with `high_fanout_threshold=5`
   - `result.high_fanout_count >= 0` (may be 0 if no net exceeds 5; just verify no crash)
   - `result.high_fanout_threshold == 5`

4. **Custom weights** — pass `CongestionWeights{0.6, 0.2, 0.2}`:
   - Verify `run_congestion_analysis` returns ok (weight validation is C5's job, but the
     plumbing must pass weights through correctly)

5. **`print_congestion_report` smoke** — call on result from test 1; verify no crash and
   no assertions fired. (stdout content is not asserted — visual inspection is sufficient
   for this brief.)

### CLI subprocess tests (require data files + built binary)

Use `EDA_LAB_DATA_DIR` and `STRUCTURAL_METRICS_CLI_BIN` compile definitions.
Spawn the binary via `fork`/`execv` (same pattern as `error_handling_test.cpp`) so a crash
is detectable as a signal death rather than being masked by a shell.

6. **LEF+DEF happy path**:
   ```
   structural_metrics_cli
       --lef <EDA_LAB_DATA_DIR>/NangateOpenCellLibrary.mod.lef
       --def <EDA_LAB_DATA_DIR>/gcd_nangate45.def
   ```
   Verify exit code 0.

7. **Missing DEF** — supply `--lef` but omit `--def`:
   Verify exit code nonzero.

8. **Non-existent file** — `--lef /nonexistent.lef --def /nonexistent.def`:
   Verify exit code nonzero (clean error, not a crash/signal death).

9. **`--help`** — `structural_metrics_cli --help`:
   Verify exit code 0.

10. **`--odb` mode** — generate an `.odb` file first using `hello_odb` or by writing one
    in-process (`db->write(stream)`), then pass it to the CLI:
    Verify exit code 0. (If generating the `.odb` in-process is complex to set up, skip
    this test and mark it for a follow-up.)

All tests must pass under `ctest` before SM1 is considered complete.

## Stdout Report — Example Output

The following is the expected shape of the report on the GCD design. Exact numbers will vary;
use this as a reference for format validation during code review:

```
=== Structural Metrics Report ===

[ Design ]
  Instances  : 484
  Nets       : 511

[ Congestion Score ]
  Score 5 (critical) :  97  (20.0%)
  Score 4            :  97  (20.1%)
  Score 3            :  97  (20.0%)
  Score 2            :  97  (20.0%)
  Score 1 (clean)    :  96  (19.8%)

  Top congestion clusters (score >= 4):
    Cluster 0 : 38 instances, peak=5
    Cluster 1 : 21 instances, peak=5
    Cluster 2 :  9 instances, peak=4

[ Supporting Metrics ]
  Vertex degree        : mean=2.09  p90=3  p99=6  max=9
  Net fanout           : mean=2.09  p90=3  p99=6  max=9
  High-fanout nets     : 0 (threshold >= 20)
  K-core degeneracy    : 3
  Neighborhood density : mean=2.30  p90=4.12  p99=7.81  max=12.50
  Net intersection     : mean=0.98  p90=2.00  p99=4.00  max=8
  Tangle score         : mean=0.31  p90=0.67  p99=0.89  max=1.00
  Tangle hot-spots     : 41 instances with score > 0.8

[ Timing ]
  (not yet available — implement T0-T4 to enable)
```

## Deliverables Checklist

- [ ] `src/engines/structural_metrics/` directory with all five files
- [ ] `structural_metrics_core` library: `run_congestion_analysis` + `print_congestion_report`
      implemented; no ODB loading inside the library
- [ ] `structural_metrics_cli` executable: LEF+DEF mode and `.odb` mode both working;
      `--help` and missing-arg error use the same `CliSpec` description source
- [ ] `--hf-threshold` flag wired through to `run_congestion_analysis`
- [ ] Three-layer error handling in CLI: file-existence prechecks + ODB boundary catch +
      top-level `main()` catch
- [ ] Report sections printed in correct order via `logger->report()`
- [ ] Root `CMakeLists.txt` updated with three new targets
- [ ] `CLAUDE.md` updated with message-ID ranges 350–374 (core) and 375–399 (CLI)
- [ ] `src/engines/structural_metrics/README.md` written
- [ ] `src/engines/structural_metrics/FLOW.md` written (CLI flow + library flow + report
      assembly diagram)
- [ ] `src/engines/README.md` updated to mention `structural_metrics`
- [ ] All `test/structural_metrics_test.cpp` cases green under `ctest`
- [ ] All pre-existing tests still green (no regressions)
- [ ] Committed with message `structural_metrics: SM1 core library + CLI driver`

## Hard Gate

All tests green including pre-existing suite. SM2 (timing section) cannot begin until SM1 is
committed and clean.

## Follow-up: SM2

Once T0–T4 are green, SM2 will:
- Add `run_timing_analysis(Hypergraph&, dbBlock*, Logger*) -> TimingAnalysisResult` to
  `structural_metrics_core`
- Add `print_timing_report(...)` replacing the `[ Timing ] — not yet available` placeholder
- Extend `structural_metrics_test.cpp` with timing analysis cases
