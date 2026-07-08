# src/support/

Link-time support for embedding only `odb` + `utl` from OpenROAD.

`ord_shim.cpp` provides inert definitions of `ord::getLogger()` and
`ord::OpenRoad::openRoad()` (both return `nullptr`). Some `utl.a` members
(the swig/Tcl wrappers, LoggerCommon) reference these
OpenROAD-application globals and get dragged into a link whenever the
linker resolves any stray symbol from those objects. Nothing in eda-lab
calls these paths — the shim exists purely so such links succeed.

Link `ord_shim` into any new target that hits
`undefined reference to ord::getLogger()` /
`ord::OpenRoad::openRoad()` (the `netlistgen` target already carries it).
