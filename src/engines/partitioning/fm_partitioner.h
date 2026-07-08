// eda-lab: flat 2-way Fiduccia–Mattheyses partitioner (Stage 1).
//
// Minimizes the weighted cut — the sum of weights of hyperedges with pins
// in both parts — subject to a vertex-weight balance constraint. This is
// the flat, 2-way stage of a planned multilevel K-way multi-objective
// partitioner; the API deliberately returns a plain result struct so later
// stages (and CLI/Python/GUI wrappers) can layer on top without changes
// here.
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
  // Maximum fractional deviation of either part's vertex weight from a
  // perfect 50/50 split: part weight must stay within
  // [(1 - t) * W/2, (1 + t) * W/2] where W is the total vertex weight.
  // Must comfortably exceed the largest single vertex weight's share of
  // W/2, or no single move is ever feasible and FM cannot leave the
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
  std::vector<int> partition;  // local vertex index -> 0 or 1
  double cut_cost = 0.0;       // final weighted cut
  int passes_run = 0;
  bool balanced = false;  // final solution met the balance constraint
};

// Deterministic: the same hypergraph and params produce the identical
// FMResult on every run (all ordering derives from the seeded RNG and
// stable vertex order; IEEE-754 arithmetic assumed across platforms).
//
// initial_partition is consulted only when params.initial == kProvided; it
// must have numVertices() entries, each 0 or 1. An unusable provided
// partition (null / wrong size / other values) falls back to the seeded
// random initial with a warning — no exceptions. A provided partition may
// violate the balance constraint; FM then accepts only imbalance-reducing
// moves until the constraint is met (see fm_partitioner.cpp).
FMResult partitionFM(const Hypergraph& hg,
                     const FMParams& params,
                     const std::vector<int>* initial_partition = nullptr);

}  // namespace eda
