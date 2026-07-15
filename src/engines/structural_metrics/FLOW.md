# structural_metrics — algorithmic flow

The `structural_metrics` engine drives the `hg_metrics` congestion group (C1–C5)
over an `eda::Hypergraph` and renders a report. Two source files: the core
library (`structural_metrics.cpp`) and the thin CLI (`structural_metrics_cli.cpp`).
Diagrams below name the real functions/types so they can be cross-checked
against the source.

## `structural_metrics.cpp` — core library

`run_congestion_analysis` calls the `hg_metrics` kernels in a fixed C1→C5 order,
each writing its `hgm.*` plane, and collects the summary into a
`CongestionAnalysisResult`. `stats_from_double_plane` is the file-scope helper
that computes a `hgm::DistributionStats` over a written double plane (needed
because C3/C4 planes are read back for their distributions, and the int
net-intersection plane is lifted to a temporary `sm.net_intersection_d` double
plane, summarised, then removed). `print_congestion_report` renders the result.

```mermaid
graph TD
  A["run_congestion_analysis(hg, result_out, logger, hf_threshold, weights)"] --> B["result_out = {}"]
  B --> C1["C1: vertex_degree_*, hyperedge_size_*, high_fanout_nets(hf_threshold)"]
  C1 --> C2["C2: k_core_numbers(hg) -> degeneracy, writes hgm.k_core"]
  C2 --> C3["C3: neighborhood_density / one_hop_neighborhood_size /<br/>net_intersection_score, writes hgm.* planes"]
  C3 --> C3s["stats_from_double_plane(hgm.neighborhood_density)<br/>lift hgm.net_intersection_score -> sm.net_intersection_d,<br/>stats, removeVertexPlane"]
  C3s --> C4["C4: tangle_score(hg, k_hop=2), writes hgm.tangle_score"]
  C4 --> C4s["tangle_stats = stats_from_double_plane(hgm.tangle_score)<br/>tangle_hot_spot_count = count_above(0.8)"]
  C4s --> C5["C5: score_congestion(hg, report, weights, {}, logger)<br/>writes hgm.congestion_score"]
  C5 --> Ok{"status.ok()?"}
  Ok -->|no| Err["return status (early)"]
  Ok -->|yes| Dim["num_vertices / num_hyperedges from hg"]
  Dim --> Ret["return okStatus()"]
```

```mermaid
graph TD
  P["print_congestion_report(result, logger)"] --> N{"logger == nullptr?"}
  N -->|yes| X["return (no-op)"]
  N -->|no| D["[ Design ]: instances, nets"]
  D --> S["[ Congestion Score ]:<br/>bands 5..1 with fixed-width %<br/>(pct relative to num_vertices_scored)"]
  S --> CL["Top clusters: iterate report.clusters,<br/>print those with peak_score >= 4, else 'None'"]
  CL --> M["[ Supporting Metrics ]:<br/>degree, fanout, high-fanout, degeneracy,<br/>neighborhood density, net intersection, tangle,<br/>tangle hot-spots (all floats 2 decimals)"]
  M --> T["[ Timing ]: placeholder (SM2)"]
```

### `stats_from_double_plane` control flow

```mermaid
graph TD
  A["stats_from_double_plane(hg, plane_name)"] --> H{"hg.hasVertexPlane(name)?"}
  H -->|no| Z["return zeroed DistributionStats"]
  H -->|yes| V["values = copy of vertexDoublePlane(name)"]
  V --> E{"values empty?"}
  E -->|yes| Z
  E -->|no| C["mean = sum/n; max = (int)max_value"]
  C --> P["p90 = nth_element at floor(0.90*(n-1))<br/>p99 = nth_element at floor(0.99*(n-1))"]
  P --> R["return stats"]
```

## `structural_metrics_cli.cpp` — CLI

`main()` is a thin layer-3 `try/catch` over `runStructuralMetricsCli`, which
parses args (via `eda::CliSpec`), validates the input mode, prechecks files
(layer 1), loads through a boundary `try/catch` (layer 2), builds the
hypergraph, calls the core, and prints the report.

```mermaid
graph TD
  M["main()"] --> TRY["try: runStructuralMetricsCli(argc, argv)"]
  TRY -->|std::exception / ...| CATCH["stderr 'Fatal error'; return 1"]

  subgraph run["runStructuralMetricsCli"]
    H{"wantsHelp?"} -->|yes| HP["printHelp; return 0"]
    H -->|no| PARSE["parse --lef* / --def / --odb /<br/>--hf-threshold / -verbosity"]
    PARSE --> MODE{"odb_mode &&<br/>lefdef_mode?"}
    MODE -->|both| UE1["printUsageError; return 1"]
    MODE -->|neither| UE2["printUsageError; return 1"]
    MODE -->|lef+def| REQ{"--lef and --def<br/>both present?"}
    REQ -->|no| UE3["printUsageError; return 1"]
    REQ -->|yes| PRE
    MODE -->|odb| PRE["layer 1: precheck every input path exists"]
    PRE -->|missing| WF["logger.warn; return 1"]
    PRE -->|ok| LOAD{"odb_mode?"}
    LOAD -->|yes| LO["loadOdb (layer-2 catch)"]
    LOAD -->|no| LL["loadLefDef (layer-2 catch)"]
    LO --> ST{"status.ok()?"}
    LL --> ST
    ST -->|no| WF2["logger.warn(status.message); return 1"]
    ST -->|yes| BUILD["Hypergraph(&logger).buildFromBlock(block)"]
    BUILD --> RUN["sm::run_congestion_analysis(hg, result, &logger, hf)"]
    RUN --> ST2{"ok?"}
    ST2 -->|no| WF3["logger.warn; return 1"]
    ST2 -->|yes| PR["sm::print_congestion_report(result, &logger); return 0"]
  end
```

### Loaders (layer-2 boundary catch)

```mermaid
graph TD
  subgraph lefdef["loadLefDef"]
    A1["try"] --> A2["lefin.createTechAndLib(lef[0])"]
    A2 -->|nullptr| A3["return LefLoadFailed"]
    A2 --> A4["for lef[1..]: createLib(tech, ...)"]
    A4 -->|nullptr| A3
    A4 --> A5["dbChip::create; defin.readChip(search_libs, def, chip)"]
    A5 --> A6["block = db->getChip()->getBlock()"]
    A6 -->|nullptr| A7["return DefLoadFailed"]
    A6 --> A8["*out_block = block; return okStatus"]
    A1 -.->|"lefin/defin call error() -> throws"| AC["catch: return ParseError"]
  end

  subgraph odb["loadOdb"]
    B1["ifstream(odb_path, binary)"] -->|"!is_open"| B2["return FileNotFound"]
    B1 --> B3["try: db->read(stream)"]
    B3 -.->|"ZIOError / throw"| B4["catch: return ParseError"]
    B3 --> B5["block = db->getChip()->getBlock()"]
    B5 -->|nullptr| B6["return DefLoadFailed"]
    B5 --> B7["*out_block = block; return okStatus"]
  end
```

## End-to-end (CLI → core → report)

```mermaid
sequenceDiagram
  participant U as user
  participant CLI as structural_metrics_cli
  participant ODB as odb (lefin/defin/read)
  participant HG as eda::Hypergraph
  participant CORE as structural_metrics_core
  participant HGM as hg_metrics (C1-C5)
  participant LOG as utl::Logger

  U->>CLI: --lef/--def or --odb [--hf-threshold]
  CLI->>ODB: load (boundary try/catch)
  ODB-->>CLI: dbBlock*
  CLI->>HG: buildFromBlock(block)
  CLI->>CORE: run_congestion_analysis(hg, result, logger, hf)
  CORE->>HGM: C1..C5 (writes hgm.* planes)
  HGM-->>CORE: stats / degeneracy / CongestionReport
  CORE-->>CLI: eda::Status + CongestionAnalysisResult
  CLI->>CORE: print_congestion_report(result, logger)
  CORE->>LOG: report() lines (Design / Score / Supporting / Timing)
  LOG-->>U: stdout report
```
