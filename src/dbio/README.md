# src/dbio/

`hello_odb.cpp` — a standalone smoke test that exercises the OpenDB I/O path
end to end:

1. Loads a tech LEF, then a cell LEF, via `odb::lefin`.
2. Reads a DEF into a pre-created `dbChip` via `odb::defin::readChip`.
3. Logs the instance and net counts of the resulting block.
4. Writes the database back out as `out.odb` and the block as `out.def`
   (both into the current working directory).

Every step above is an `info`-level `utl::Logger` phase marker (no
`std::cout`), per the repo logging convention.

## Usage

Run from the `run/` directory so `out.odb`/`out.def` land there, not in the
repo root (standing convention in `CLAUDE.md`):

```bash
cd run
../build/hello_odb <tech_lef> <cell_lef> <def> [-verbosity <level>]
# e.g.
../build/hello_odb ../data/nangate45/Nangate45_tech.lef \
                   ../data/nangate45/Nangate45_stdcell.lef \
                   ../data/gcd_nangate45.def
```

Argument order is tech LEF, cell LEF, DEF — exiting with usage help when the
three positional paths are not all present. The optional `-verbosity <level>`
flag (`--verbosity=<level>` too) raises debug verbosity; unset = the `info`
phase markers above. Level 1 adds the block's DBU/micron as a debug line (see
the repo logging convention in `CLAUDE.md`).

## ODB API notes at the pinned SHA (`a3d4865`)

- `defin::readChip` requires the `dbChip` to be created up front with
  `dbChip::create(db, tech, name)`; it does not create one itself.
- `lefin::createTechAndLib(tech_name, lib_name, lef_path)` loads the tech
  LEF; `lefin::createLib(tech, lib_name, lef_path)` loads cell LEFs against
  an existing tech.

The instance/net counts printed here are the reference values the
hypergraph tests compare against.
