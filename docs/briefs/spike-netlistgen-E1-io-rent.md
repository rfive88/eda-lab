# Spike Brief — Netlistgen Stage E1: Primary I/O Generation with Rent's Rule

## Goal

Add Stage E1 to the netlistgen pipeline: generate primary input (PI) and primary output (PO)
ports whose count is governed by Rent's rule (T = k · G^p). Port count scales correctly with
design size, boundary buffer/FF cells are inserted per a configurable pin-type distribution,
and the final metrics report includes the actual Rent parameters for the full design and for
each sub-cluster.

Stage E2 (structural Verilog output) is deferred and will follow a separate brief.

## Source-read requirement

**Before writing a single line of implementation**, read the following in full:

1. All source files under `src/engines/netlistgen/` — understand the complete Stage A–D pipeline,
   especially how instances, nets, and cluster assignments are represented, and how the
   final metrics report is printed.
2. `src/engines/netlistgen/README.md` and `src/engines/netlistgen/FLOW.md` — understand the documented flow.
3. `src/common/` — shared utilities (logging, Status, RNG helpers).
4. The ODB API (search for `dbBTerm`, `dbNet`, `dbBlock`, `dbITerm` in the OpenROAD headers
   or in any existing usage in `src/`) — understand how boundary terminals (PI/PO ports) are
   created and associated with nets.

Identify:
- Where the final metrics report is generated (the function that prints average fanout,
  instance count, etc.). E1 appends to this report.
- Where the cluster membership vector (`cluster_id`) is held. Sub-cluster Rent computation
  iterates over it.
- The exact type used for instance IDs across Stages A–D.

Only after completing this read should any implementation begin.

## Context

- Stages A–D are complete, tested, and pushed. Do not break any existing behaviour.
- All new parameters are optional. If `rent_k` and `rent_p` are both absent, Stage E1 is a
  no-op: the pipeline terminates after Stage D exactly as today.
- Stage E1 operates on the hypergraph and ODB block produced by Stage D. It adds new vertices
  (ports, boundary cells) and new hyperedges connecting them to internal nets.
- The existing `sequential_ratio` controls internal registers only. Boundary FFs created in
  this stage are tracked separately and must not alter the internal register population or
  the sequential_ratio accounting.

## New JSON config parameters

All fields are optional unless noted. E1 activates only when both `rent_k` and `rent_p` are
present.

```json
{
  "rent_k": 2.5,
  "rent_p": 0.60,
  "io_input_ratio": 0.60,
  "io_pin_type_distribution": {
    "combinational": 1.0,
    "buffered":       0.0,
    "registered":     0.0
  }
}
```

### Field descriptions

| Field | Type | Default | Notes |
|---|---|---|---|
| `rent_k` | float | — | Rent coefficient k (required to activate E1) |
| `rent_p` | float | — | Rent exponent p (required to activate E1) |
| `io_input_ratio` | float | 0.60 | Fraction of T assigned as PIs; remainder are POs |
| `io_pin_type_distribution` | object | `{combinational:1.0, buffered:0.0, registered:0.0}` | Fractional split of pin types applied independently to PI and PO populations |

### Validation rules

- If exactly one of `rent_k` / `rent_p` is present → error, non-OK Status, no generation.
- `rent_k` must be > 0.
- `rent_p` must be in (0.0, 1.0] — Rent exponents outside this range are physically
  unrealistic; warn and clamp if in (1.0, 1.2] (degenerate but tolerable), error if > 1.2.
- `io_input_ratio` must be in (0.0, 1.0).
- `io_pin_type_distribution` fractions must sum to 1.0 ± 0.01. If they sum outside this
  range, error. If within tolerance, normalise silently before use.
- Individual fractions must each be in [0.0, 1.0].

## Algorithm

### Step 1 — Compute target terminal count

```
G = instance count from Stage A  // internal logic only; boundary cells not yet created
T = round(rent_k * pow(G, rent_p))
T_in  = round(T * io_input_ratio)   // PI count
T_out = T - T_in                     // PO count
```

G does **not** include boundary buffer/FF cells that will be added in Step 3. Those cells are
created after T is fixed, so the Rent target is anchored to internal logic only.

### Step 2 — Identify boundary candidates via random sampling

Real designs have no strict topological rule for which internal nets become boundary-visible.
Use random sampling:

```
// For PIs: randomly select T_in nets from the full net list to be PI-driven.
// The selected net will receive a new PI port vertex as an additional driver.
shuffle net_list with seeded RNG
pi_nets  = net_list[0 .. T_in - 1]
po_nets  = net_list[T_in .. T_in + T_out - 1]
```

If the net list has fewer nets than T_in + T_out (can happen on very small configs), cap T
at net_list.size() and log a warning:
```
utl::Logger warn: "E1: T ({}) exceeds net count ({}); capping at net count"
```

### Step 3 — Assign pin types and insert boundary cells

For each PI net in `pi_nets`, sample a pin type from `io_pin_type_distribution`:

**Combinational PI** (no extra cell):
```
new_vertex = hg.add_vertex()
hg.set_bool_attr(new_vertex, "hgm.is_pi", true)
add new_vertex as driver of the selected net in the hypergraph
create dbBTerm on the ODB net with direction INPUT
```

**Buffered PI** (insert buffer cell between port and internal net):
```
buf_vertex = hg.add_vertex()          // buffer instance
hg.set_bool_attr(buf_vertex, "hgm.is_boundary_buf", true)
pi_vertex  = hg.add_vertex()          // the port itself
hg.set_bool_attr(pi_vertex, "hgm.is_pi", true)
create a new 1-sink net: driver = pi_vertex, sink = buf_vertex
add buf_vertex as driver of the selected internal net
create dbBTerm on the ODB net for pi_vertex with direction INPUT
```

**Registered PI** (FF at boundary; FF drives internal net):
```
ff_vertex  = hg.add_vertex()          // boundary FF instance
hg.set_bool_attr(ff_vertex, "hgm.is_boundary_reg", true)
pi_vertex  = hg.add_vertex()          // the port itself
hg.set_bool_attr(pi_vertex, "hgm.is_pi", true)
create a new 1-sink net: driver = pi_vertex, sink = ff_vertex (D pin)
add ff_vertex as driver of the selected internal net (Q pin)
create dbBTerm on the ODB net for pi_vertex with direction INPUT
```

Apply the same pattern for PO nets:

**Combinational PO**: new PO vertex added as sink of selected internal net; dbBTerm direction OUTPUT.

**Buffered PO**: buffer cell takes internal net as input; PO port vertex observes buffer output.

**Registered PO**: FF captures internal net (D pin); PO port vertex observes FF Q output.

### Step 4 — Hypergraph attribute planes written by E1

| Plane name | Type | Set on | Meaning |
|---|---|---|---|
| `hgm.is_pi` | bool | PI port vertices | True for primary input ports |
| `hgm.is_po` | bool | PO port vertices | True for primary output ports |
| `hgm.is_boundary_buf` | bool | Buffer boundary cells | True for I/O buffer instances |
| `hgm.is_boundary_reg` | bool | FF boundary cells | True for boundary flip-flops (not counted in sequential_ratio) |

Do **not** set `hgm.is_boundary_reg` on internal registers created in Stage A.

### Step 5 — Sub-cluster Rent computation (if sub-clusters present)

This runs before the metrics report and requires the `cluster_id` vector from the sub-cluster
feature (introduced in the peak-fanout-clusters brief). If `cluster_id` is all -1 (no clusters
configured), skip this step.

For each cluster index c in 0 .. num_peak_clusters-1:
```
G_c = count of instances with cluster_id == c
T_c = count of nets that have at least one endpoint in cluster c
      AND at least one endpoint outside cluster c (driver or sink)
      // These are the "cut nets" of the cluster — its effective terminal count
if G_c < 2 or T_c < 1: skip cluster (log warning, degenerate)
p_c = log(T_c) / log(G_c)
k_c = T_c / pow(G_c, p_c)
```

Store results in a small struct:
```cpp
struct ClusterRentStats {
    int    cluster_idx;
    int    G_c;
    int    T_c;
    double p_c;
    double k_c;
};
std::vector<ClusterRentStats> cluster_rent;
```

### Step 6 — Actual Rent computation for full design

After all PI/PO ports are created:
```
T_actual = T_in + T_out        // may differ from T if capped in Step 2
G_actual = Stage A instance count  // internal instances only, not boundary cells
p_actual = log(T_actual) / log(G_actual)
k_actual = T_actual / pow(G_actual, p_actual)
```

### Step 7 — Metrics report additions

Append to the existing final metrics report (find the function that logs design stats):

```
[INFO] === Primary I/O Summary ===
[INFO]   Target Rent:  k={rent_k:.3f}  p={rent_p:.3f}  ->  T_target={T}
[INFO]   Actual Rent:  k={k_actual:.3f}  p={p_actual:.3f}  ->  T_actual={T_actual}  (PI={T_in}, PO={T_out})
[INFO]   Pin types:    combinational={n_comb}  buffered={n_buf}  registered={n_reg}
[INFO]   Boundary FFs: {n_boundary_ff}  (tracked separately from internal sequential_ratio={sequential_ratio:.2f})

[INFO] === Sub-cluster Rent Parameters ===    // only if clusters present
[INFO]   Cluster 0:  G={G_c}  T_c={T_c}  k={k_c:.3f}  p={p_c:.3f}
[INFO]   Cluster 1:  ...
...
[INFO]   Background:  G={G_bg}  T_bg={T_bg}  k={k_bg:.3f}  p={p_bg:.3f}
```

For background Rent: G_bg = instances with cluster_id == -1, T_bg = cut nets of the
background (nets with at least one endpoint in background and at least one in any cluster,
or nets entirely within background touching a PI/PO).

## Files to modify

Based on your source read, identify the minimal set. Expected changes:
- JSON config struct and parser (new fields + validation)
- A new `stage_e1.cpp` / `stage_e1.h` (or inline into the main pipeline file if the pattern
  for prior stages is to keep everything in one file — follow existing convention)
- Final metrics report function (append Rent stats)
- `FLOW.md` — add Stage E1 to the pipeline diagram
- `README.md` — document all new JSON fields

## Test requirements

Add test cases to the existing netlistgen test suite:

1. **No E1 params** — all existing tests must pass unchanged. Confirm `is_pi`, `is_po`,
   `is_boundary_reg` planes are absent.

2. **Basic E1** — `rent_k=2.5, rent_p=0.60, instance_count=2000`:
   - T_target ≈ round(2.5 * 2000^0.6) — verify T_actual within ±5% of T_target.
   - PI count ≈ 0.60 * T_actual, PO count ≈ 0.40 * T_actual (within ±1 rounding).
   - All PI/PO vertices have `is_pi` / `is_po` planes set.
   - Boundary cells (buf/reg) have their respective planes set.
   - `is_boundary_reg` is NOT set on any Stage A internal FF.
   - p_actual logged in report is a finite positive number.

3. **Custom io_input_ratio** — `io_input_ratio=0.75`:
   - PI count ≈ 0.75 * T_actual.

4. **Custom pin type distribution** — `{"combinational":1.0, "buffered":0.0, "registered":0.0}`:
   - Zero boundary buf/FF cells created.
   - `is_boundary_buf` and `is_boundary_reg` planes are absent or all false.

5. **With sub-clusters** — combine with `peak_avg_fanout`, `num_peak_clusters=2`:
   - Per-cluster Rent stats logged for cluster 0, cluster 1, and background.
   - p_c for peak clusters is plausibly higher than background p (soft check, warn if not).

6. **Validation errors**:
   - Only `rent_k` present, `rent_p` absent → non-OK Status.
   - `rent_p = 1.5` → non-OK Status.
   - `io_pin_type_distribution` fractions sum to 0.5 → non-OK Status.
   - `io_input_ratio = 1.1` → non-OK Status.

7. **Small design cap** — `instance_count=10, rent_k=5.0, rent_p=0.8`:
   - If T exceeds net count, warning is logged and T is capped. No crash.

All tests must pass under `ctest` before committing.

## Deliverables checklist

- [ ] Source read completed before any implementation
- [ ] JSON config struct updated with new optional fields + validation
- [ ] T = k · G^p computation correct (anchored to internal G only)
- [ ] Random boundary sampling implemented with seeded RNG
- [ ] All three pin types (combinational / buffered / registered) implemented for PI and PO
- [ ] `hgm.is_pi`, `hgm.is_po`, `hgm.is_boundary_buf`, `hgm.is_boundary_reg` planes written correctly
- [ ] Boundary FFs do NOT increment internal sequential count
- [ ] dbBTerm created in ODB for each PI/PO port
- [ ] Actual Rent p and k computed and logged in metrics report
- [ ] Sub-cluster Rent stats computed and logged (when clusters present)
- [ ] Background Rent stats computed and logged (when clusters present)
- [ ] All existing netlistgen tests green (no regression)
- [ ] All new E1 test cases green
- [ ] `FLOW.md` updated with Stage E1
- [ ] `README.md` updated with new JSON fields
- [ ] Committed with message `netlistgen: Stage E1 primary I/O generation with Rent's rule`

## Hard gate

Do not proceed past this brief until all tests are green and the commit is clean.
Stage E2 (structural Verilog output) begins only after this gate is passed.
