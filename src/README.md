# src/

C++17 source tree. Everything links against OpenDB (`odb`) and the OpenROAD
logger (`utl`) from the vendored submodule; nothing here builds the OpenROAD
application itself. Each subdirectory is one library or tool and carries its
own README with the details.

| Subdirectory | Purpose |
| --- | --- |
| `dbio/` | `hello_odb`: LEF/DEF round-trip smoke test against OpenDB |
| `hypergraph/` | Rebuildable hypergraph view of a `dbBlock` netlist, with attribute planes |
| `netlistgen/` | Programmatic netlist construction (no LEF/DEF) for tests and benchmarks |
| `engines/` | Partitioning/clustering algorithm engines (currently empty) |
| `support/` | Link-time shims for embedding odb/utl without the OpenROAD application |
