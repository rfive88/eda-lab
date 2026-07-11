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

**Graceful failure (see "Error handling" in `CLAUDE.md`).** Bad input never
crashes. The work lives in a testable `runHelloOdb(argc, argv)`; `main()`
is a thin `try/catch` backstop over it. `loadDesign` prechecks that each of
the three input files exists before touching `lefin`/`defin` (a missing
file becomes an `eda::Status` `FileNotFound`, not a thrown OpenROAD error),
and wraps the reader calls in a boundary `try/catch` so a present-but-
malformed LEF/DEF — which makes OpenROAD's readers throw — is reported as a
`Status` and a clean nonzero exit rather than an abort. It also null-checks
every `lefin` return before dereferencing, and (because `defin::readChip`
destroys the passed chip on failure) detects DEF-read failure via
`db->getChip()` rather than the now-dangling local pointer. Output stream
opens and the DEF-writer return are checked too.

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

## Command-Line Options

Per the repo CLI `--help`/usage convention (`CLAUDE.md`, `src/support/cli.h`).
`--help`/`-h` prints this list and exits 0; a missing positional prints the
same block to stderr and exits nonzero.

| Option | Required | Description |
|--------|----------|-------------|
| `<tech_lef>` | yes | Technology LEF file, loaded first (`lefin::createTechAndLib`). |
| `<cell_lef>` | yes | Cell library LEF file, loaded against that tech (`lefin::createLib`). |
| `<def>` | yes | DEF file read into a pre-created chip/block (`defin::readChip`). |
| `-verbosity <level>` | no | Debug verbosity (`--verbosity=<level>` too); unset/0 = the `info` phase markers above. Level 1 adds the block's DBU/micron as a debug line. See the logging convention in `CLAUDE.md`. |

Argument order is tech LEF, cell LEF, DEF.

## ODB API notes at the pinned SHA (`a3d4865`)

- `defin::readChip` requires the `dbChip` to be created up front with
  `dbChip::create(db, tech, name)`; it does not create one itself.
- `lefin::createTechAndLib(tech_name, lib_name, lef_path)` loads the tech
  LEF; `lefin::createLib(tech, lib_name, lef_path)` loads cell LEFs against
  an existing tech.

The instance/net counts printed here are the reference values the
hypergraph tests compare against.
