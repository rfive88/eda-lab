# data/

Test data for the GTest suites and `hello_odb`. `EDA_LAB_DATA_DIR`
(defined by CMake for the tests) points here.

- `nangate45/` — the Nangate45 open cell library LEF set. The tests use
  `Nangate45_tech.lef` + `Nangate45_stdcell.lef`; the other LEF variants
  come along from the source tree.
- `gcd_nangate45.def` — a placed GCD design on Nangate45: 734 components,
  497 nets. The standard small benchmark for the hypergraph tests.

## Provenance

Copied from the OpenROAD submodule's test tree
(`third_party/openroad/test/`) at the pinned SHA, so the data stays
available even if the submodule layout changes.

## Guidance

Keep DEFs here small. The test suites parse LEF/DEF at fixture setup, and
the whole test loop should stay in the seconds range — put large synthetic
netlists through `src/engines/netlistgen/` instead, which needs no data
files.
