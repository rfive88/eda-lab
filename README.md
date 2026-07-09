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
