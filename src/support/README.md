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

`ord_shim.cpp` provides inert definitions of `ord::getLogger()` and
`ord::OpenRoad::openRoad()` (both return `nullptr`). Some `utl.a` members
(the swig/Tcl wrappers, LoggerCommon) reference these
OpenROAD-application globals and get dragged into a link whenever the
linker resolves any stray symbol from those objects. Nothing in eda-lab
calls these paths — the shim exists purely so such links succeed.

Link `ord_shim` into any new target that hits
`undefined reference to ord::getLogger()` /
`ord::OpenRoad::openRoad()` (the `netlistgen` target already carries it).
