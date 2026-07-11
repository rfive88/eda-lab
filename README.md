# eda-lab

Experimental EDA playground built on OpenROAD's OpenDB (`odb`). C++17,
CMake, GTest. OpenROAD is vendored as a submodule (pinned to `a3d4865`);
only its `odb` and `utl` libraries are linked.

Current pieces:

- a rebuildable hypergraph view of a `dbBlock` netlist, with attribute
  planes for engine data (`src/hypergraph/`),
- a LEF/DEF round-trip smoke test (`src/dbio/`),
- algorithm engines under `src/engines/`: synthetic-netlist construction
  (no LEF/DEF needed, `src/engines/netlistgen/`), a K-way FM partitioner
  (`src/engines/partitioning/`), and a home for future clustering engines.

## Build & test

```bash
git submodule update --init --recursive
cmake -B build
cmake --build build
ctest --test-dir build -R "hypergraph_test|netlistgen_test" --output-on-failure
```

Every directory has a README.md describing its contents; development
conventions live in `CLAUDE.md`.

## Developer notes

All messages go through OpenROAD's `utl::Logger` (never ad hoc
`printf`/`std::cout`). Every CLI takes an optional `-verbosity <level>`
flag (`--verbosity=<level>` also works):

- **unset / 0** — default: one phase marker per major step, a final
  summary, and any warnings/errors.
- **1** — per-phase detail (counts, achieved-vs-requested statistics,
  library-level phase markers).
- **2** — periodic progress heartbeats on long-running operations.
- **3** — per-item tracing (per-net formation, per-move gains), capped
  for large runs.

Example: `netlistgen_cli config.json -verbosity 2`. Logging goes to
stdout; it never mixes into the deterministic data output files
(DEF/`.odb`). See `CLAUDE.md` and `src/support/logging.h` for the full
convention.

Every CLI also follows a consistent `--help`/usage convention: `--help`
(or `-h`) lists each option with a one-line description and exits 0; a
missing required argument prints the same usage block and exits nonzero.
Per-option detail (defaults, ranges, examples) lives in each engine's own
README under "Command-Line Options"; the shared renderer is
`src/support/cli.h`.

Bad input fails gracefully, never with a crash: a missing or malformed
LEF/DEF/config, or a missing output directory, produces a clear message
and a nonzero exit code. Expected failures are propagated as explicit
return values (`src/support/status.h`), OpenROAD's throwing
`utl::Logger::error()` is contained at the reader boundary, and every CLI
`main()` carries a top-level catch-all backstop. See `CLAUDE.md` for the
full convention.
