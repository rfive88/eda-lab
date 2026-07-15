# structural_metrics engine (Spike SM1)

Drives the `hg_metrics` congestion group (C1–C5) end to end over an
`eda::Hypergraph` and renders a human-readable report. It is the analysis
front-end that turns the individual `hg_metrics` kernels into a single
"structural congestion" pass with one summary result and one report.

Timing analysis (T0–T4) is **out of scope for SM1**: the report prints a
placeholder `[ Timing ]` section. SM2 will add `run_timing_analysis` /
`print_timing_report`.

See `FLOW.md` for algorithmic flow diagrams.

## Layers

Two layers, following the `netlistgen` / `netlistgen_cli_core` split:

- **`structural_metrics_core`** (library, `structural_metrics.{h,cpp}`) — takes
  an **already-built** `eda::Hypergraph` and runs C1→C2→C3→C4→C5 in sequence,
  returning a `CongestionAnalysisResult`. It does **no ODB loading**, so any
  in-process caller (a future OpenROAD integration, another engine) can invoke
  it without spawning a subprocess. `run_congestion_analysis` writes every
  `hgm.*` per-vertex attribute plane as a side effect (via the `hg_metrics`
  functions it calls). `print_congestion_report` renders the result via
  `utl::Logger::report()`.

- **`structural_metrics_cli`** (executable, `structural_metrics_cli.cpp`) — a
  thin `main()` that handles all ODB loading (LEF+DEF or native `.odb`), builds
  the `Hypergraph`, calls the core, and prints the report.

## What it computes

`run_congestion_analysis` calls, in order:

| Step | `hg_metrics` calls | Planes written | Result fields |
|------|--------------------|----------------|---------------|
| C1 | `vertex_degree_histogram/_stats`, `hyperedge_size_histogram/_stats`, `high_fanout_nets` | — | `degree_*`, `fanout_*`, `high_fanout_count/_threshold` |
| C2 | `k_core_numbers` | `hgm.k_core` | `degeneracy` |
| C3 | `neighborhood_density`, `one_hop_neighborhood_size`, `net_intersection_score` | `hgm.neighborhood_density`, `hgm.neighborhood_size_1hop`, `hgm.net_intersection_score` | `neighborhood_density_stats`, `net_intersection_stats` |
| C4 | `tangle_score` (k_hop_radius=2) | `hgm.tangle_score` | `tangle_stats`, `tangle_hot_spot_count` (tangle > 0.8) |
| C5 | `score_congestion` | `hgm.congestion_score` | `congestion_report` (histogram + clusters) |

`net_intersection_score` writes an **int** plane; because `hg_metrics` exposes
no const int-plane reader and its `DistributionStats` percentile helper is a
private static over `int`, the core computes the intersection/density/tangle
`DistributionStats` with its own file-scope `stats_from_double_plane` helper
(same nearest-rank percentile convention as `hgm::vertex_degree_stats`). This
is a deliberate small duplication: SM1 must not modify `src/hg_metrics/`.

### Score distribution (important)

`DistributionStats::max` is an `int` (shared type), so a double plane's max is
truncated in the report; the p90/p99 fields keep full precision.

The C5 score is a quintile bin (1–5) of a composite percentile rank blended
from the C2/C3/C4 planes. In practice the **full 1–5 spread is only produced on
inputs where all three metrics vary together**. On the synthetic netlists used
by the tests, C4 tangle saturates to ~1.0 for every vertex (locally dense: the
2-hop induced subgraph has more terminals than cells → Rent exponent clamps to
1.0), pinning its rank at 0.5 and capping the composite below band 5. On the
real `gcd` design two metrics saturate the other way (neighborhood density is
near-uniform, tangle collapses to 0 because the 2-hop neighborhood covers the
whole small block), leaving only bands 2–3. This is a property of the
(unmodifiable) `hg_metrics` kernels, not of this engine — the engine reports
faithfully whatever spread they produce.

## Running the CLI

```
structural_metrics_cli --lef <tech.lef> [--lef <cells.lef> ...] --def <def> \
                       [--hf-threshold <n>] [-verbosity <level>]
structural_metrics_cli --odb <file.odb> [--hf-threshold <n>] [-verbosity <level>]
```

Run from the `run/` directory if you redirect output to a file (repo
convention 4). Example against the bundled Nangate45 + gcd design:

```bash
cd run
../build/structural_metrics_cli \
    --lef ../data/nangate45/Nangate45_tech.lef \
    --lef ../data/nangate45/Nangate45_stdcell.lef \
    --def ../data/gcd_nangate45.def
```

### Command-line options

- `--lef <path>` — **repeatable.** The first `--lef` supplies the technology
  (loaded via `odb::lefin::createTechAndLib`); each additional `--lef` adds a
  cell library against that tech (`createLib`). Required together with `--def`.
  (This generalises the brief's single-`--lef` sketch to N libraries, matching
  `hello_odb.cpp`'s tech-LEF + cell-LEF pattern and this repo's split
  Nangate45 `Nangate45_tech.lef` / `Nangate45_stdcell.lef` files. A single
  combined LEF still works as one `--lef`.)
- `--def <path>` — DEF design file. Requires `--lef`. Loaded via `odb::defin`.
- `--odb <path>` — native `.odb` design, loaded via `dbDatabase::read`.
  Mutually exclusive with `--lef`/`--def`.
- `--hf-threshold <n>` — high-fanout net pin-count threshold passed to C1
  (`high_fanout_nets`, inclusive). Default **20**.
- `-verbosity <level>` (`--verbosity=<level>`) — repo-wide debug verbosity
  (see `src/support/logging.h`). Debug group `structural_metrics`.

Input-mode rules: supply `--lef` + `--def`, **or** `--odb`, never both. A
missing required argument prints a usage/error block (shared with `--help` via
`eda::CliSpec`) and exits nonzero.

### Verbosity levels

- **0** (default): the final report (always printed via `logger->report()`)
  plus CLI phase markers (`info`) and any warnings.
- **1**: per-phase library markers (C1…C5) and the `hg_metrics` per-metric
  debug summaries.
- **2**: `hg_metrics` heartbeats (k-core, cluster counts).
- **3**: per-item traces, capped at `eda::kTraceCap`.

## Report shape

```
=== Structural Metrics Report ===

[ Design ]           — instance / net counts
[ Congestion Score ] — score 1-5 histogram (fixed-width %), top clusters (peak>=4)
[ Supporting Metrics ] — C1-C4 distribution summaries (floats to 2 decimals)
[ Timing ]           — placeholder (SM2)
```

## Logging / message IDs

All output via `utl::Logger`. Tool id `utl::UKN`; message-id ranges (see
`src/support/logging.h`): **core library 350–374**, **CLI 375–399**. Debug
group `structural_metrics`.

## Error handling

Three-layer convention (see CLAUDE.md): file-existence prechecks (layer 1), a
`try/catch` at each OpenROAD reader / `.odb` stream call converting the throw to
an `eda::Status` (layer 2), and a top-level `try/catch` in `main()` (layer 3).
Bad input yields a clean nonzero exit, never a crash.

## Tests

`test/structural_metrics_test.cpp`:

- **In-process** (no data files): build a synthetic netlist with
  `netlistgen::generateSynthetic`, build a `Hypergraph` from its block, call the
  core directly — basic sanity, empty hypergraph, custom hf-threshold, custom
  weights, `print_congestion_report` smoke.
- **CLI subprocess** (needs `EDA_LAB_DATA_DIR` + `STRUCTURAL_METRICS_CLI_BIN`):
  `fork`/`execv` the binary — LEF+DEF happy path, missing `--def`, nonexistent
  file, `--help`, and `--odb` mode (generates an `.odb` in-process first).

Build and run inside the devcontainer:

```bash
cmake --build build --target structural_metrics_test
./build/structural_metrics_test
```
