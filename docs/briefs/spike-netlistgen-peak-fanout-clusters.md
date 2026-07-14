# Spike Brief — Netlistgen Peak Fanout Sub-Clusters

## Goal

Extend the netlistgen engine (Stages A–D) to support generation of synthetic congestion
hot-spots: sub-clusters of cells that are preferentially wired to each other with higher-than-average
fanout. The resulting netlists have localized dense subgraphs with known structural properties,
providing ground truth for validating the hg_metrics congestion and timing detectors.

## Source-read requirement

**Before writing a single line of implementation**, read the following in full:

1. All source files under `src/engines/netlistgen/` — understand the current Stage A–D pipeline,
   especially how instances are generated (Stage A), how nets and fanout are generated (Stage B),
   and how the JSON config is parsed.
2. `src/engines/netlistgen/README.md` and `src/engines/netlistgen/FLOW.md` — understand the documented flow.
3. `src/common/` — understand shared utilities that may be relevant.

Identify:
- The exact function(s) responsible for net generation and fanout sampling in Stage B.
- Where JSON config fields are declared, parsed, and validated.
- Where instance/cell data structures are defined.

Only after completing this read should any implementation begin.

## Context

- All of Stages A–D are complete, tested, and pushed. Do not break any existing behaviour.
  All new parameters are optional — if absent, the netlist is generated identically to today.
- Stage D's combinational loop avoidance (DAG topological sort + cycle breaking) runs unchanged
  as the backstop. Cluster-aware net generation may introduce cycles; Stage D will handle them.
- The purpose of this feature is to generate netlists with tunable congestion hot-spots for
  validating `src/hg_metrics/` metrics (not yet implemented — this brief is upstream of that work).

## New JSON config parameters

All four fields are optional. If none are present, behaviour is unchanged.

```json
{
  "avg_fanout": 4.0,           // existing
  "peak_avg_fanout": 12.0,     // new: target average fanout for intra-cluster nets
  "peak_cluster_pct": 0.15,    // new: fraction of total cells assigned to peak clusters (0.0–1.0)
  "num_peak_clusters": 2       // new: number of peak clusters to create (default 1 if omitted but peak_avg_fanout present)
}
```

Validation rules:
- `peak_avg_fanout` must be > `avg_fanout` if both are specified. Error and return non-OK Status if not.
- `peak_cluster_pct` must be in (0.0, 1.0). Error if out of range.
- `num_peak_clusters` must be >= 1.
- If `peak_avg_fanout` is present but `peak_cluster_pct` is absent, default `peak_cluster_pct` to 0.10.
- If `peak_avg_fanout` is absent, ignore the other two fields even if present.

## Algorithm

### Step 1 — Cluster assignment (new, runs after Stage A instance generation)

```
if peak_avg_fanout not specified: skip entirely

cluster_size = floor(peak_cluster_pct * total_instances / num_peak_clusters)
shuffle instance list with seeded RNG (use same RNG as rest of generation for reproducibility)
for k in 0 .. num_peak_clusters-1:
    assign instances[k * cluster_size .. (k+1) * cluster_size - 1] to cluster k
remaining instances: background (cluster = -1)
```

Store cluster membership in a `std::vector<int> cluster_id(num_instances, -1)` local to the
generation function. Do not add cluster membership to the ODB/hypergraph data model — it is
a generation-time bookkeeping structure only.

### Step 2 — Cluster-aware net generation (modifies Stage B fanout sampling and sink selection)

For each net being generated, after selecting the driver cell:

```
if driver is in cluster c (cluster_id[driver] >= 0):
    fanout = sample from fanout distribution centred on peak_avg_fanout
    for each sink slot:
        if uniform_random() < 0.8:   // p_intra = 0.8, hardcoded
            pick random cell from cluster c  (exclude driver, exclude already-picked sinks)
        else:
            pick random cell from background (exclude driver, exclude already-picked sinks)
else:
    fanout = sample from fanout distribution centred on avg_fanout   // unchanged
    sink selection = unchanged
```

`p_intra = 0.8` is an implementation constant — do not expose it in the JSON config.

Use the same fanout distribution shape as the existing Stage B implementation (whatever
distribution is currently used — Poisson, log-normal, etc.) applied to the new centre value.

**Edge case**: if cluster c has fewer cells than the requested fanout, fall back to background
cells for the remaining sink slots rather than allowing duplicate sinks.

### Step 3 — Stage D runs unchanged

No changes to Stage D. Cycle breaking is the existing backstop.

## Files to modify

Based on your source read, identify the minimal set of files. Expected changes are in:
- JSON config struct and parser (new fields + validation)
- Stage B net generation function (cluster assignment call + conditional fanout/sink logic)
- Possibly Stage A if cluster assignment is more naturally placed there

Do not modify Stage C (writers), Stage D (DAG), or any test infrastructure outside the
netlistgen test file.

## Test requirements

Add test cases to the existing netlistgen test suite:

1. **No peak params** — existing tests must all pass unchanged. Verify no regression.

2. **Basic cluster generation** — `peak_avg_fanout=12, peak_cluster_pct=0.15, num_peak_clusters=1`:
   - Netlist generates without error.
   - Total instance count matches config.
   - Stage D runs and produces a valid DAG (no crashes).
   - Average fanout of nets whose drivers are in the cluster is measurably higher than global
     average fanout (allow 20% tolerance given small test sizes).

3. **Multiple clusters** — `num_peak_clusters=3`:
   - Three distinct cluster groups are assigned.
   - Verify cluster sizes are approximately `peak_cluster_pct * N / 3` each (within ±1 cell).

4. **Validation errors**:
   - `peak_avg_fanout < avg_fanout` → non-OK Status, no netlist generated.
   - `peak_cluster_pct = 1.5` → non-OK Status.
   - `num_peak_clusters = 0` → non-OK Status.

5. **Default `num_peak_clusters`** — omit `num_peak_clusters` but specify `peak_avg_fanout`
   and `peak_cluster_pct` → defaults to 1, generates successfully.

All tests must pass under `ctest` before committing.

## Deliverables checklist

- [ ] Source read completed before any implementation
- [ ] JSON config struct updated with 3 new optional fields + validation
- [ ] Cluster assignment logic implemented (runs after Stage A, before Stage B net generation)
- [ ] Stage B net generation modified with cluster-aware fanout sampling and sink selection
- [ ] Stages C and D untouched
- [ ] All existing netlistgen tests green (no regression)
- [ ] New test cases green
- [ ] `src/engines/netlistgen/FLOW.md` updated to show cluster assignment step in the pipeline diagram
- [ ] `src/engines/netlistgen/README.md` updated to document the new JSON fields
- [ ] Committed with message `netlistgen: peak fanout sub-cluster generation`

## Hard gate

Do not proceed past this brief until all tests are green and the commit is clean.
This brief completes before Stage E (dbBTerm / structural Verilog) begins.
