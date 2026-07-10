# src/support/

Link-time support for embedding only `odb` + `utl` from OpenROAD, plus the
repo-wide logging convention header.

`logging.h` is the single source of truth for the `utl::Logger` +
runtime-verbosity convention (see the "Logging & runtime verbosity" section
of `CLAUDE.md`). It is header-only: confirmed `utl::Logger` API notes for the
pinned SHA, the verbosity-level constants (`kVerbosityDefault`..`kVerbosityTrace`,
`kTraceCap`), and `applyVerbosity(logger, group, level)` — a one-line wrapper
over `Logger::setDebugLevel` that maps a `-verbosity <level>` flag onto the
logger's built-in debug tier. Every CLI and every engine includes it via
`"support/logging.h"`.

`cli.h` is the single source of truth for the CLI `--help`/usage convention
(see "CLI `--help` / usage" in `CLAUDE.md`). Also header-only: a `CliSpec` of
`CliOption`s (each carrying one one-line `description`) plus
`printHelp`/`printUsageError`/`wantsHelp`, so every CLI renders both its
`--help` output and its missing-argument error from the same registered option
list. `verbosityOption()` supplies the shared `-verbosity` wording. Used by
`hello_odb` and `netlistgen_cli`; `test/cli_help_test.cpp` enforces the
single-source-of-truth guarantee.

`ord_shim.cpp` provides inert definitions of `ord::getLogger()` and
`ord::OpenRoad::openRoad()` (both return `nullptr`). Some `utl.a` members
(the swig/Tcl wrappers, LoggerCommon) reference these
OpenROAD-application globals and get dragged into a link whenever the
linker resolves any stray symbol from those objects. Nothing in eda-lab
calls these paths — the shim exists purely so such links succeed.

Link `ord_shim` into any new target that hits
`undefined reference to ord::getLogger()` /
`ord::OpenRoad::openRoad()` (the `netlistgen` target already carries it).
