// eda-lab: procedural random hypergraph generator for engine testing.
//
// Produces a dbBlock-free Hypergraph (via Hypergraph::buildFromTopology)
// with a controlled size and degree distribution, fully determined by the
// seed — the input side of partitioning/clustering engine tests, where
// LEF/DEF data would be overkill and netlistgen's driver/sink pin
// bookkeeping is more structure than needed.

#pragma once

#include "hypergraph/hypergraph.h"

namespace eda {

struct RandomHypergraphParams
{
  int num_vertices = 0;
  int num_hyperedges = 0;
  // Pins per hyperedge, inclusive bounds. Effective bounds are clamped to
  // [2, num_vertices]: an edge needs >= 2 distinct pins to ever be
  // cuttable, and pins within an edge are distinct so the degree cannot
  // exceed the vertex count.
  int min_degree = 2;
  int max_degree = 2;
  unsigned seed = 1;
};

// Same params -> bit-identical topology on every run and every platform:
// all randomness comes from raw std::mt19937 draws (the engine's output
// sequence is fixed by the C++ standard) mapped with modulo, never from
// std::uniform_int_distribution, whose algorithm is implementation-defined.
// Vertices within a hyperedge are distinct (no duplicate pins).
//
// Degenerate params fail soft, matching the Hypergraph API style: with
// num_vertices < 2 or num_hyperedges <= 0 the result has num_vertices
// vertices (clamped at >= 0) and no hyperedges.
Hypergraph generateRandomHypergraph(const RandomHypergraphParams& params);

}  // namespace eda
