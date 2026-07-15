# Spike Brief T3 — HyperANF Neighbourhood Function

## Goal

Implement the HyperANF algorithm (Boldi, Rosa, Vigna 2011) to approximate the neighbourhood
function N(t) of the timing DAG and compute per-vertex ball sizes at configurable hop depths.
Ball size B(v, t) — the approximate count of vertices reachable from v within t hops — is a
direct proxy for timing cone size and is an input to the T4 timing risk composite score.

## Context

- Prerequisite: T1 green. `TimingDAG` with `adj` and `topo_order` is available.
- The neighbourhood function N(t) counts pairs (u, v) such that v is reachable from u in < t hops.
- HyperLogLog counters: each counter is an array of `m` registers (each storing a 5-bit value,
  sufficient for graphs up to 2^32 nodes). Union of two counters = component-wise max.
  The estimated set size from a counter is `alpha_m * m^2 * (sum of 2^{-M[j]})^{-1}`.
- This is implemented as two static helpers inside `timing_metrics.cpp` — not exposed in the
  header. No separate HyperLogLog file; no templates.
- The BFS expansion is independent of `congestion_metrics.cpp`'s propagation helper.
  Implement separately as specified here.
- Do not modify files outside `src/hg_metrics/` and `tests/hg_metrics/`.

## Files to Modify

```
src/hg_metrics/timing_metrics.h
src/hg_metrics/timing_metrics.cpp
test/hg_metrics_timing_test.cpp
```

## API Additions

```cpp
// timing_metrics.h — add inside namespace hgm

// Approximates the neighbourhood function N(t) for t = 0, 1, ..., until convergence.
// Returns a vector where result[t] = approximate number of (source, dest) reachable pairs
// with path length < t. Uses HyperLogLog counters with m registers per vertex.
// m=64 gives ~13% relative error; m=256 gives ~6.6%. Default m=64.
std::vector<double> neighbourhood_function(const TimingDAG& dag, int num_vertices, int m = 64);

// Effective diameter: smallest t such that N(t)/N(max) >= alpha. Default alpha=0.9.
double effective_diameter(const TimingDAG& dag, int num_vertices, int m = 64, double alpha = 0.9);

// For each vertex v, approximates |B(v, t)| = number of vertices reachable from v in <= t hops.
// Writes result to "hgm.ball_size_{t}" (double) attribute plane on vertices.
// Runs HyperANF internally; m controls precision.
void vertex_ball_sizes(Hypergraph& hg, const TimingDAG& dag, int t, int m = 64);
```

## Static Helpers (inside timing_metrics.cpp, not in header)

```cpp
// HyperLogLog counter: array of m uint8_t registers.
// Each register stores a value in [0, 31] (5 bits, sufficient for 2^32 nodes).
using HLLCounter = std::vector<uint8_t>;

// Add a single element (hashed) to a counter.
static void hll_add(HLLCounter& c, uint64_t hash_val);

// Union: c[i] = max(c[i], other[i]) for all i. In-place.
static void hll_union(HLLCounter& c, const HLLCounter& other);

// Estimate cardinality from counter.
static double hll_size(const HLLCounter& c);
```

### HyperLogLog implementation

```
hll_add(c, x):
    use a fast integer hash (e.g., splitmix64) on x
    b = number of bits in log2(m)  [e.g., b=6 for m=64]
    j = top b bits of hash → register index [0, m-1]
    w = remaining bits of hash
    rho = position of leftmost 1-bit in w (count leading zeros + 1)
    c[j] = max(c[j], rho)

hll_size(c):
    Z = 1.0 / sum(2^{-c[j]} for j in 0..m-1)
    alpha_m = correction constant (0.7213 / (1 + 1.079/m) for m >= 128; use table for smaller m)
    E = alpha_m * m * m * Z
    return E
    // Apply small/large range corrections if needed (see HyperLogLog paper Section 4)
```

Alpha_m constants: m=16→0.673, m=32→0.697, m=64→0.709, m=128→0.715, m=256→0.718.

### HyperANF main loop

```
neighbourhood_function(dag, num_vertices, m):
    counters = vector of num_vertices HLLCounter, each of size m, initialised to 0

    // Seed: each vertex adds itself
    for v in 0..num_vertices-1:
        hll_add(counters[v], hash(v))

    result = []
    prev_sum = 0.0

    loop:
        // Sum current counters
        current_sum = sum(hll_size(counters[v]) for all v)
        result.push_back(current_sum)

        // Check convergence: stop if sum did not change
        if abs(current_sum - prev_sum) < 0.5:
            break
        prev_sum = current_sum

        // One expansion step: process in REVERSE topo_order
        // (so that when we update u, all successors of u have already been updated)
        new_counters = copy of counters
        for u in reverse(dag.topo_order):
            for v in dag.adj[u]:
                hll_union(new_counters[u], counters[v])
        counters = new_counters

    return result
```

Note: process in reverse topological order so that each vertex accumulates from its successors
(reachability propagates forward along directed edges — we want B(u, t) to include everything
u can reach). Seeding each vertex with itself gives B(v, 0) ≈ 1 for all v.

For `vertex_ball_sizes`: run HyperANF for exactly `t` iterations (not to convergence), then read
`hll_size(counters[v])` for each vertex and write to the attribute plane.

## Test Requirements

Add to `test/hg_metrics_timing_test.cpp`:

1. **Single vertex, no edges**: N(0) ≈ 1 (one self-pair), neighbourhood_function returns [1.0].
   effective_diameter = 0.

2. **Linear chain A→B→C→D (4 vertices)**:
   - B(A,0)=1, B(A,1)≈2, B(A,2)≈3, B(A,3)≈4.
   - N(1)≈4 (each vertex reaches itself), N(2)≈4+3=7, etc.
   - Allow 20% relative error on estimates (m=64, small graph).

3. **`effective_diameter`** on a linear chain of 10 vertices:
   - Exact diameter is 9. Effective diameter (α=0.9) should be in [7, 10].

4. **`vertex_ball_sizes` with t=1**:
   - On a star (1 source → 5 sinks): source ball size ≈ 6, each sink ≈ 1.
   - Verify `"hgm.ball_size_1"` plane exists and source value > sink values.

5. **`vertex_ball_sizes` with t=2**:
   - On a two-hop chain (A→B→C): B(A,2) ≈ 3 (reaches A, B, C).
   - Allow 20% error.

6. **Convergence**: verify `neighbourhood_function` terminates (does not loop indefinitely)
   on any acyclic graph.

All tests must pass under `ctest` before proceeding to T4.

## Deliverables Checklist

- [ ] `neighbourhood_function`, `effective_diameter`, `vertex_ball_sizes` implemented
- [ ] Three static HLL helpers (`hll_add`, `hll_union`, `hll_size`) in `.cpp`, not in header
- [ ] Alpha_m correction constants hardcoded for m = 16, 32, 64, 128, 256
- [ ] Convergence loop terminates correctly (change < 0.5 threshold)
- [ ] `"hgm.ball_size_{t}"` double plane written (e.g., `"hgm.ball_size_2"` for t=2)
- [ ] All timing gtest cases green
- [ ] `FLOW.md` updated with HyperANF expansion diagram
- [ ] Committed with message `hg_metrics: T3 HyperANF neighbourhood function`

## Hard Gate

All tests green. T2 and T3 must both be complete before T4 begins.
