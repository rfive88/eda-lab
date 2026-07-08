# src/netlistgen/

Programmatic netlist construction through OpenDB API calls only — no
LEF/DEF — so tests and benchmarks can create `dbBlock`s of any size with
exactly known or statistically controlled topology, then feed them to
`Hypergraph::buildFromBlock()`. Two layers:

- **`NetlistBuilder`** owns a fresh `dbDatabase` (tech, lib, chip, top
  block) and wraps master/inst/net creation and pin connection, including
  OpenDB's master-freeze protocol (`dbMTerm::create` all pins, then
  `setFrozen()`, before any `dbInst::create`). Masters are
  connectivity-only, with pins named `i0..iN-1` / `o0..oM-1`.
- **`generateSynthetic(builder, spec)`** populates the block from a
  `SyntheticNetlistSpec`: weighted cell mix, instance count, optional net
  count, and a fanout range (pins per net, driver included). Seeded
  `std::mt19937` makes a given (spec, seed) reproducible, and every iterm
  is used at most once, so the result is always a valid netlist.

Scale reference: ~500k insts / ~1.4M pins generate in about 2 s. Tests are
in `test/netlistgen_test.cpp` (no data files needed).
