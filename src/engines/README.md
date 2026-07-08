# src/engines/

Algorithm engines that operate on the `eda::Hypergraph` model — one
subdirectory per engine (partitioner, clusterer, ...). Engines communicate
with each other and with callers through hypergraph attribute planes rather
than ad-hoc arrays.

The directory is currently empty; the first planned engine is an FM
(Fiduccia–Mattheyses) partitioner.

## Convention: every engine ships its own README.md

This is a requirement for every future engine subdirectory, not a
suggestion. Each `src/engines/<engine>/README.md` must document:

1. **What the engine does** — the algorithm and its objective.
2. **Input contract** — which hypergraph attribute planes it reads and
   which it writes (names, element kind, element type).
3. **Control parameters** — every option, with its type and default.
4. **How to run it** — how to invoke the engine and run its tests.

A session that creates an engine subdirectory creates this README in the
same session (see "Documentation Conventions" in the repo's CLAUDE.md).
