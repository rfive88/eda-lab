// See random_hypergraph.h for the determinism contract.

#include "engines/partitioning/random_hypergraph.h"

#include <algorithm>
#include <random>
#include <vector>

namespace eda {

namespace {

// Uniform-ish draw in [lo, hi] from raw mt19937 output. Modulo mapping has
// bias O(range / 2^32) — irrelevant for test-input generation, and unlike
// std::uniform_int_distribution it is bit-identical across platforms.
int drawInRange(std::mt19937& rng, const int lo, const int hi)
{
  return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1));
}

}  // namespace

Hypergraph generateRandomHypergraph(const RandomHypergraphParams& params)
{
  const int n = std::max(params.num_vertices, 0);

  std::vector<std::vector<int>> edges;
  if (n >= 2 && params.num_hyperedges > 0) {
    // Clamp the degree bounds into [2, n] (see the header for why), then
    // reconcile an inverted range in favor of the lower bound.
    const int lo = std::min(std::max(params.min_degree, 2), n);
    const int hi = std::max(std::min(params.max_degree, n), lo);

    std::mt19937 rng(params.seed);
    edges.reserve(params.num_hyperedges);

    // Rejection-sample distinct vertices per edge. `stamp[v] == e` marks v
    // as already in edge e, giving O(1) duplicate checks without clearing
    // a set between edges. Termination: degree <= n guarantees enough
    // distinct vertices exist, and pin order is the draw order, so the
    // result depends only on the rng stream.
    std::vector<int> stamp(n, -1);
    for (int e = 0; e < params.num_hyperedges; ++e) {
      const int degree = drawInRange(rng, lo, hi);
      std::vector<int> edge;
      edge.reserve(degree);
      while (static_cast<int>(edge.size()) < degree) {
        const int v = drawInRange(rng, 0, n - 1);
        if (stamp[v] != e) {
          stamp[v] = e;
          edge.push_back(v);
        }
      }
      edges.push_back(std::move(edge));
    }
  }

  Hypergraph hg;
  hg.buildFromTopology(n, edges);
  return hg;
}

}  // namespace eda
