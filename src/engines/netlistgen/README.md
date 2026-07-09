# src/engines/netlistgen/

Synthetic netlist generation engine. Builds `dbBlock`s through OpenDB API
calls only — **no LEF/DEF input** — so tests and benchmarks can create
netlists of any size with exactly known or statistically controlled
topology, then feed them to `Hypergraph::buildFromBlock()`.

> **Migration status (Stage A of 5).** `netlistgen` was just promoted from a
> Stage 1/2 test utility (`src/netlistgen/`) into an engine under
> `src/engines/`. This stage is pure plumbing: the directory move, a proper
> library target, and a pin-access refactor that reads pin direction from
> `dbMTerm::getIoType()` instead of parsing pin names. **No new generation
> capability was added.** Landing in later stages, in order:
> - **Stage B** — LEF-backed masters (real cells) and a statistical cell
>   mix (weighted + max-entropy modes).
> - **Stage C** — combinational-loop avoidance (provably acyclic net
>   formation among combinational cells). Until then the generator can
>   produce combinational cycles.
> - **Stage D** — DEF / `.odb` writers and a standalone CLI executable.
> - **Stage E** — primary I/O ports and a Verilog writer.
>
> See `FLOW.md` for algorithmic flow diagrams.

## What the engine does

Two layers, both in `netlistgen.h` / `netlistgen.cpp`:

- **`NetlistBuilder`** owns a fresh `dbDatabase` (tech, lib, chip, top
  block) and wraps master/inst/net creation and pin connection. It handles
  OpenDB's master-freeze protocol (`dbMTerm::create` all pins, then
  `setFrozen()`, required before `dbInst::create`). Masters built here are
  connectivity-only (no geometry), with input pins named `i0..iN-1` and
  output pins `o0..oM-1`. Those names are a builder convenience only — the
  generator does **not** depend on them (see below).

- **`generateSynthetic(builder, spec)`** populates the block from a
  `SyntheticNetlistSpec`. Instances are drawn from a weighted cell mix and
  named `u0..u{n-1}`; nets are named `n0..n{k-1}`. Each terminal is sorted
  into a driver pool or a sink pool **by its `dbMTerm` IoType, not by pin
  name**: `OUTPUT` → driver, `INPUT`/`INOUT` → sink, `FEEDTHRU` → ignored.
  No assumption is made about pin ordering or position, so the same code
  path will drive Stage B's LEF-backed masters (arbitrary pin names/counts)
  unchanged. Each net takes one unused driver pin and `fanout−1` unused sink
  pins from shuffled pools, so every iterm lands on at most one net — always
  a valid netlist. Generation stops when the requested net count is reached
  or a pin pool drains. A seeded `std::mt19937` makes a given `(spec, seed)`
  reproducible.

## Input / output contract

Standalone in-memory library: **no hypergraph attribute planes** are read
or written. Input is a `SyntheticNetlistSpec` (C++ struct); output is a
populated `dbBlock` owned by the `NetlistBuilder`, plus the net count as the
return value. The block is consumed downstream by
`Hypergraph::buildFromBlock()`.

## Control parameters (`SyntheticNetlistSpec`)

| Field | Type | Default | Meaning |
|-------|------|---------|---------|
| `masters` | `std::vector<MasterSpec>` | — (must be non-empty) | Weighted cell mix. |
| `num_insts` | `int` | `0` (must be `> 0`) | Number of instances to create. |
| `num_nets` | `int` | `-1` | Net cap; `-1` means "as many as the pin pools allow". |
| `min_fanout` | `int` | `2` | Min pins per net (driver included). |
| `max_fanout` | `int` | `4` | Max pins per net (driver included). |
| `seed` | `uint32_t` | `1` | RNG seed; fixes output for a given spec. |

`MasterSpec` fields: `name` (`std::string`), `num_inputs` (`int`, default
`2`), `num_outputs` (`int`, default `1`), `weight` (`double`, default `1.0`;
relative pick frequency, need not sum to 1). Every combinational master is
modeled as exactly one output pin and `(pin_count − 1)` input pins.

## Determinism

Output for a given `(spec, seed)` is bit-identical across runs and
platforms — the generator draws from a seeded `std::mt19937` and the
pin-classification order is fixed by `dbInst`/`dbITerm` iteration order.

## How to run

Link the library and call it in-memory — this is the intended consumer
pattern for other engines (e.g. a partitioner choosing synthetic input over
real LEF/DEF):

```cmake
target_link_libraries(<your_target> PRIVATE netlistgen odb utl)
```

```cpp
eda::NetlistBuilder nb;
eda::SyntheticNetlistSpec spec;
spec.masters = {{"INV", 1, 1, 1.0}, {"NAND2", 2, 1, 2.0}};
spec.num_insts = 1000;
const int nets = eda::generateSynthetic(nb, spec);
eda::Hypergraph hg;
hg.buildFromBlock(nb.block());
```

### Tests

```bash
cmake -B build
cmake --build build --target netlistgen_test netlistgen_link_smoke
ctest --test-dir build -R "netlistgen_test|netlistgen_link_smoke" --output-on-failure
```

- `test/netlistgen_test.cpp` — behavior: exact CSR contents on a hand-built
  3-inst/2-net case, spec conformance (fanout bounds, counts, pin
  uniqueness) on 2000 insts, net-count limiting, and seed determinism. No
  data files needed.
- `test/netlistgen_link_smoke.cpp` — library-linkage guard: an external
  consumer that links `PRIVATE netlistgen odb utl` and calls the engine,
  proving `netlistgen` is a real linkable library target (fails to link if
  it ever regresses to header-only).

Scale reference: ~500k insts / ~1.4M pins generate in about 2 s.
