// eda-lab: flat K-way Fiduccia–Mattheyses partitioner (Stages 1–2).
//
// Minimizes the connectivity-1 objective — for each hyperedge e with
// lambda(e) = number of distinct parts holding at least one pin of e,
//
//   cut_cost = sum over e of weight(e) * (lambda(e) - 1)
//
// subject to a vertex-weight balance constraint on every part. For
// num_parts == 2, lambda is 1 or 2 and this is exactly the Stage 1
// weighted cut (sum of weights of hyperedges with pins in both parts);
// the K = 2 path reproduces Stage 1 results. This is the flat stage of a
// planned multilevel K-way multi-objective partitioner; the API
// deliberately returns a plain result struct so later stages (and
// CLI/Python/GUI wrappers) can layer on top without changes here.
//
// Plane contract (all optional, read-only):
//   - hyperedge double plane "weight": cut cost per hyperedge (default 1.0)
//   - vertex double plane "area": balance weight per vertex (default 1.0)
//
// Pure library core: the hypergraph is const, results come back by value,
// nothing prints (a utl::Logger, if provided, gets debug-level trace only),
// nothing throws.

#pragma once

#include <vector>

namespace utl {
class Logger;
}  // namespace utl

namespace eda {

class Hypergraph;

struct FMParams
{
  // Number of parts K. Values below 1 are clamped to 1 (K = 1 is the
  // trivial single-part solution: everything in part 0, cost 0).
  int num_parts = 2;

  // Maximum fractional deviation of any part's vertex weight from a
  // perfect W/K split: every part weight must stay within
  // [(1 - t) * W/K, (1 + t) * W/K] where W is the total vertex weight.
  // Must comfortably exceed the largest single vertex weight's share of
  // W/K, or no single move is ever feasible and FM cannot leave the
  // initial partition.
  double balance_tolerance = 0.10;

  // FM outer passes; stops early when a pass fails to improve the cut.
  int max_passes = 10;

  // Seeds the initial random partition and all tie-breaking order.
  unsigned seed = 1;

  enum class InitialPartition
  {
    kRandom,    // seeded balanced random split (see fm_partitioner.cpp)
    kProvided,  // take the initial_partition argument of partitionFM()
  };
  InitialPartition initial = InitialPartition::kRandom;

  // Debug-level trace only (tool UKN, group "fm"); never required.
  utl::Logger* logger = nullptr;
};

struct FMResult
{
  std::vector<int> partition;  // local vertex index -> part in [0, K)
  double cut_cost = 0.0;       // final connectivity-1 cost (see header)
  int passes_run = 0;
  bool balanced = false;  // final solution met the balance constraint
};

// Deterministic: the same hypergraph and params produce the identical
// FMResult on every run (all ordering derives from the seeded RNG and
// stable vertex order; IEEE-754 arithmetic assumed across platforms).
//
// initial_partition is consulted only when params.initial == kProvided; it
// must have numVertices() entries, each in [0, num_parts). An unusable
// provided partition (null / wrong size / other values) falls back to the
// seeded random initial with a warning — no exceptions. A provided
// partition may violate the balance constraint (including leaving parts
// empty); FM then accepts only infeasibility-reducing moves until the
// constraint is met (see fm_partitioner.cpp).
FMResult partitionFM(const Hypergraph& hg,
                     const FMParams& params,
                     const std::vector<int>* initial_partition = nullptr);

}  // namespace eda
