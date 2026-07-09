// Flat K-way FM with the connectivity-1 objective. See fm_partitioner.h
// for the API contract.
//
// Implementation notes
// --------------------
// Objective bookkeeping: the engine maintains, per hyperedge e, the pin
// count in every part (cnt_[e*K + p]) and the derived connectivity
// lambda(e) = number of parts with a nonzero count. Moving a vertex from
// p to q touches only the two affected slots of each incident edge, so
// the cut delta is w(e) * (lambda_after - lambda_before) with lambda
// adjusted in O(1) — no recount. For K = 2 this reduces to the Stage 1
// spanning cut, and the K = 2 code path performs the same floating-point
// operations in the same order as Stage 1, reproducing its results.
//
// Gain container: hyperedge weights are doubles, so the classic
// integer-keyed bucket list does not apply directly. The options were
// (a) discretize gains into buckets — O(1) selection but adds a
// quantization knob whose granularity interacts with weight scale, or
// (b) a max-heap keyed by the exact double gain with lazy deletion —
// O(log n) selection, no quantization, much simpler to keep correct.
// This engine uses (b): one heap per source part, one entry per
// (vertex, target part) — a vertex in part p has K-1 candidate moves,
// all keyed in heap p. Every gain change pushes a fresh entry and bumps
// the (vertex, target) stamp; entries whose stamp no longer matches are
// dead and get dropped when popped. Revisit (a) only if selection ever
// shows up in a profile.
//
// Multiplicity: a vertex can hold several pins of one hyperedge (one CSR
// entry per dbITerm). Moving the vertex moves all of them at once, so all
// count/gain math runs on distinct (vertex, multiplicity) incidence pairs,
// precomputed once per run from the CSR arrays.
//
// Gain bookkeeping — the classic FM bug source — avoids the textbook
// "if T(e)==0 / T(e)==1" case rules entirely (they assume unit
// multiplicity and K = 2). Instead it uses one exact formula. With
// n_p(e) = pins of e in part p and vertex u in part s holding mult(u,e)
// pins, the connectivity-1 delta of moving u from s to t is captured by:
//
//   contrib(u, e, t) = w(e) * [n_s(e) == mult(u,e)]   // move empties s
//                    - w(e) * [n_t(e) == 0]           // move occupies t
//   gain(u, t) = sum over u's distinct edges of contrib(u, e, t)
//
// (When both brackets hold, e leaves s and enters t together — lambda
// unchanged — and the terms cancel.) When a vertex moves from p to q,
// only n_p and n_q change, so every other unlocked member's contribution
// is re-derived under the old and new counts and the difference applied.
// Targets outside {p, q} are affected only when the member itself sits
// in p or q (its own-part bracket changed). That difference form is
// self-correcting: it only needs the counts to be right, not a case
// analysis to be exhaustive.

#include "engines/partitioning/fm_partitioner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <random>
#include <vector>

#include "hypergraph/hypergraph.h"
#include "utl/Logger.h"

namespace eda {

namespace {

// Comparison slack for the double-valued cut/balance bookkeeping. "Better"
// always means "better by more than kEps", so FP noise can neither create
// phantom improvements (infinite passes) nor flip a tie inconsistently.
constexpr double kEps = 1e-12;

struct HeapEntry
{
  double gain;
  int vertex;
  int target;      // destination part of this candidate move
  uint64_t stamp;  // matches stamp_[vertex*K + target] iff alive
};

// Max-heap on gain; equal gains break toward the smaller vertex index,
// then the smaller target part, so selection order is a pure function of
// the gain values (determinism).
struct EntryOrder
{
  bool operator()(const HeapEntry& a, const HeapEntry& b) const
  {
    if (a.gain != b.gain) {
      return a.gain < b.gain;
    }
    if (a.vertex != b.vertex) {
      return a.vertex > b.vertex;
    }
    return a.target > b.target;
  }
};

using GainHeap
    = std::priority_queue<HeapEntry, std::vector<HeapEntry>, EntryOrder>;

// Platform-stable draw in [0, n): raw mt19937 output (sequence fixed by
// the standard) with modulo mapping, because uniform_int_distribution's
// algorithm is implementation-defined. Bias is O(n / 2^32) — irrelevant
// for tie-breaking and initial-partition purposes.
int drawBelow(std::mt19937& rng, const int n)
{
  return static_cast<int>(rng() % static_cast<unsigned>(n));
}

class FMEngine
{
 public:
  FMEngine(const Hypergraph& hg, const FMParams& params)
      : hg_(hg),
        params_(params),
        k_(std::max(1, params.num_parts)),
        rng_(params.seed)
  {
  }

  FMResult run(const std::vector<int>* initial);

 private:
  void loadWeights();
  void buildIncidence();
  void setInitialPartition(const std::vector<int>* initial);
  void computeCountsAndCut();
  double vertexGain(int v, int to) const;
  bool isBalanced() const;
  // Total distance of the part-weight vector from the feasible box: sum
  // over parts of how far the part lies outside [min_part_, max_part_].
  // Zero iff balanced (modulo kEps). For K = 2 this makes the same
  // accept/reject and best-prefix decisions as Stage 1's |w0 - W/2|
  // deviation metric: with symmetric bounds both parts violate together
  // and the two metrics differ by a constant plus a factor of two.
  double infeasibility() const;
  double infeasibilityAfter(int from, int to, double moved) const;
  bool moveIsAcceptable(int v, int to) const;
  void applyMove(int v, int to, GainHeap* heaps);
  bool runPass();

  const Hypergraph& hg_;
  const FMParams& params_;
  const int k_;  // number of parts

  int n_ = 0;  // vertices
  int m_ = 0;  // hyperedges

  std::vector<double> vertex_weight_;  // "area" plane or 1.0
  std::vector<double> edge_weight_;    // "weight" plane or 1.0
  double total_weight_ = 0.0;
  double min_part_ = 0.0;  // (1 - tol) * W/K
  double max_part_ = 0.0;  // (1 + tol) * W/K

  // Distinct incidence with multiplicity, both directions, CSR-style:
  // vertex v's distinct edges are (v_edge_[i], v_mult_[i]) for i in
  // [v_edge_off_[v], v_edge_off_[v+1]); e_* mirrors for hyperedges.
  std::vector<int> v_edge_off_, v_edge_, v_mult_;
  std::vector<int> e_vert_off_, e_vert_, e_mult_;

  std::vector<int> part_;             // vertex -> part in [0, K)
  std::vector<double> part_weight_;   // per part
  std::vector<int> cnt_;              // edge e, part p -> cnt_[e*K + p]
  std::vector<int> lambda_;           // per edge: parts with pins
  double cut_ = 0.0;                  // sum of w(e) * (lambda(e) - 1)

  std::vector<double> gain_;      // (vertex, target) -> gain_[v*K + t]
  std::vector<uint64_t> stamp_;   // parallel to gain_
  std::vector<char> locked_;

  std::mt19937 rng_;
};

void FMEngine::loadWeights()
{
  const std::vector<double>* area = hg_.findVertexDoublePlane("area");
  vertex_weight_.assign(n_, 1.0);
  if (area != nullptr) {
    vertex_weight_ = *area;
  }
  total_weight_ = 0.0;
  for (const double w : vertex_weight_) {
    total_weight_ += w;
  }
  min_part_ = (1.0 - params_.balance_tolerance) * total_weight_
              / static_cast<double>(k_);
  max_part_ = (1.0 + params_.balance_tolerance) * total_weight_
              / static_cast<double>(k_);

  const std::vector<double>* weight = hg_.findHyperedgeDoublePlane("weight");
  edge_weight_.assign(m_, 1.0);
  if (weight != nullptr) {
    edge_weight_ = *weight;
  }
}

void FMEngine::buildIncidence()
{
  // Vertex side: each vertex's incident-edge slice is sorted by
  // construction (hypergraph build guarantee), so duplicates are adjacent
  // and a single run-length pass produces (edge, multiplicity) pairs.
  const std::vector<int>& voff = hg_.vertexOffsets();
  const std::vector<int>& vpins = hg_.vertexPinList();
  v_edge_off_.assign(n_ + 1, 0);
  for (int v = 0; v < n_; ++v) {
    for (int p = voff[v]; p < voff[v + 1]; ++p) {
      if (p == voff[v] || vpins[p] != vpins[p - 1]) {
        v_edge_.push_back(vpins[p]);
        v_mult_.push_back(1);
      } else {
        ++v_mult_.back();
      }
    }
    v_edge_off_[v + 1] = static_cast<int>(v_edge_.size());
  }

  // Edge side: pin slices follow dbITerm order, not sorted — sort a copy
  // per edge, then run-length encode. Sorting also fixes a deterministic
  // member order for gain updates.
  const std::vector<int>& eoff = hg_.hyperedgeOffsets();
  const std::vector<int>& pins = hg_.pinList();
  e_vert_off_.assign(m_ + 1, 0);
  std::vector<int> members;
  for (int e = 0; e < m_; ++e) {
    members.assign(pins.begin() + eoff[e], pins.begin() + eoff[e + 1]);
    std::sort(members.begin(), members.end());
    for (size_t i = 0; i < members.size(); ++i) {
      if (i == 0 || members[i] != members[i - 1]) {
        e_vert_.push_back(members[i]);
        e_mult_.push_back(1);
      } else {
        ++e_mult_.back();
      }
    }
    e_vert_off_[e + 1] = static_cast<int>(e_vert_.size());
  }
}

void FMEngine::setInitialPartition(const std::vector<int>* initial)
{
  const bool provided_ok
      = initial != nullptr && static_cast<int>(initial->size()) == n_
        && std::all_of(initial->begin(), initial->end(), [this](const int p) {
             return p >= 0 && p < k_;
           });

  if (params_.initial == FMParams::InitialPartition::kProvided
      && provided_ok) {
    part_ = *initial;
  } else {
    if (params_.initial == FMParams::InitialPartition::kProvided
        && params_.logger != nullptr) {
      params_.logger->warn(utl::UKN,
                           102,
                           "FM: provided initial partition is unusable "
                           "(null, wrong size, or values outside [0, {})); "
                           "falling back to seeded random initial",
                           k_);
    }
    // Balanced random initial: visit vertices in a seeded Fisher–Yates
    // order (own loop, not std::shuffle — see drawBelow) and put each on
    // the currently lightest part (ties to the smallest part index).
    // Randomness lives in the visit order; the greedy placement is what
    // guarantees the result is balanced whenever balance is achievable
    // at all (final spread between parts <= max single vertex weight).
    std::vector<int> order(n_);
    for (int v = 0; v < n_; ++v) {
      order[v] = v;
    }
    for (int i = n_ - 1; i > 0; --i) {
      std::swap(order[i], order[drawBelow(rng_, i + 1)]);
    }
    part_.assign(n_, 0);
    std::vector<double> filled(k_, 0.0);
    for (const int v : order) {
      int side = 0;
      for (int p = 1; p < k_; ++p) {
        if (filled[p] < filled[side]) {
          side = p;
        }
      }
      part_[v] = side;
      filled[side] += vertex_weight_[v];
    }
  }

  part_weight_.assign(k_, 0.0);
  for (int v = 0; v < n_; ++v) {
    part_weight_[part_[v]] += vertex_weight_[v];
  }
}

void FMEngine::computeCountsAndCut()
{
  const std::vector<int>& eoff = hg_.hyperedgeOffsets();
  const std::vector<int>& pins = hg_.pinList();
  cnt_.assign(static_cast<size_t>(m_) * k_, 0);
  lambda_.assign(m_, 0);
  cut_ = 0.0;
  for (int e = 0; e < m_; ++e) {
    int* c = cnt_.data() + static_cast<size_t>(e) * k_;
    for (int p = eoff[e]; p < eoff[e + 1]; ++p) {
      ++c[part_[pins[p]]];
    }
    int lam = 0;
    for (int p = 0; p < k_; ++p) {
      if (c[p] > 0) {
        ++lam;
      }
    }
    lambda_[e] = lam;
    if (lam > 1) {
      cut_ += edge_weight_[e] * (lam - 1);
    }
  }
}

double FMEngine::vertexGain(const int v, const int to) const
{
  // See the file header: exact count-based form, valid for any pin
  // multiplicity, summed in fixed incidence order (determinism).
  const int from = part_[v];
  double g = 0.0;
  for (int i = v_edge_off_[v]; i < v_edge_off_[v + 1]; ++i) {
    const int e = v_edge_[i];
    const int mult = v_mult_[i];
    const int* c = cnt_.data() + static_cast<size_t>(e) * k_;
    if (c[from] == mult) {
      g += edge_weight_[e];
    }
    if (c[to] == 0) {
      g -= edge_weight_[e];
    }
  }
  return g;
}

bool FMEngine::isBalanced() const
{
  for (int p = 0; p < k_; ++p) {
    if (part_weight_[p] < min_part_ - kEps
        || part_weight_[p] > max_part_ + kEps) {
      return false;
    }
  }
  return true;
}

double FMEngine::infeasibility() const
{
  double d = 0.0;
  for (int p = 0; p < k_; ++p) {
    d += std::max(0.0, part_weight_[p] - max_part_)
         + std::max(0.0, min_part_ - part_weight_[p]);
  }
  return d;
}

double FMEngine::infeasibilityAfter(const int from,
                                    const int to,
                                    const double moved) const
{
  double d = 0.0;
  for (int p = 0; p < k_; ++p) {
    double w = part_weight_[p];
    if (p == from) {
      w -= moved;
    }
    if (p == to) {
      w += moved;
    }
    d += std::max(0.0, w - max_part_) + std::max(0.0, min_part_ - w);
  }
  return d;
}

bool FMEngine::moveIsAcceptable(const int v, const int to) const
{
  const int from = part_[v];
  const double new_from = part_weight_[from] - vertex_weight_[v];
  const double new_to = part_weight_[to] + vertex_weight_[v];
  if (new_from >= min_part_ - kEps && new_to <= max_part_ + kEps) {
    return true;  // move keeps (or establishes) balance for these parts
  }
  // Recovery mode for an unbalanced provided initial: a move that
  // strictly shrinks the distance to the feasible region is allowed even
  // though the result is still out of tolerance, so FM can walk back into
  // feasibility instead of freezing. Never fires from a balanced state
  // (there the branch above already accepted any tolerable move, and an
  // intolerable one must not be taken regardless of deviation).
  if (isBalanced()) {
    return false;
  }
  return infeasibilityAfter(from, to, vertex_weight_[v])
         < infeasibility() - kEps;
}

void FMEngine::applyMove(const int v, const int to, GainHeap* heaps)
{
  const int from = part_[v];
  locked_[v] = 1;

  for (int i = v_edge_off_[v]; i < v_edge_off_[v + 1]; ++i) {
    const int e = v_edge_[i];
    const int mult = v_mult_[i];
    int* c = cnt_.data() + static_cast<size_t>(e) * k_;

    // Only the from/to slots change when v's pins change parts.
    const int oc_from = c[from];
    const int oc_to = c[to];
    const int nc_from = oc_from - mult;
    const int nc_to = oc_to + mult;

    // Lambda / cut delta straight from the counts — no case analysis to
    // get wrong. (nc_from can hit zero; oc_to can have been zero.)
    int lam_new = lambda_[e];
    if (nc_from == 0) {
      --lam_new;
    }
    if (oc_to == 0) {
      ++lam_new;
    }
    if (lam_new != lambda_[e]) {
      cut_ += edge_weight_[e] * (lam_new - lambda_[e]);
    }

    // Re-derive every other member's contribution from e under the old
    // and new counts; the difference is its gain adjustment. Locked
    // vertices are skipped — they cannot move again this pass, so their
    // gains are dead until the pass-start rebuild. A member's own-part
    // bracket changes only if it sits in {from, to}; its target bracket
    // changes only for targets in {from, to} — anything else is a no-op
    // and skipped cheaply.
    for (int j = e_vert_off_[e]; j < e_vert_off_[e + 1]; ++j) {
      const int u = e_vert_[j];
      if (u == v || locked_[u]) {
        continue;
      }
      const int u_from = part_[u];
      const int u_mult = e_mult_[j];
      const int oc_own = (u_from == from) ? oc_from
                         : (u_from == to) ? oc_to
                                          : c[u_from];
      const int nc_own = (u_from == from) ? nc_from
                         : (u_from == to) ? nc_to
                                          : oc_own;
      for (int t = 0; t < k_; ++t) {
        if (t == u_from) {
          continue;
        }
        const int oc_t = (t == from) ? oc_from : (t == to) ? oc_to : c[t];
        const int nc_t = (t == from) ? nc_from : (t == to) ? nc_to : oc_t;
        if (oc_own == nc_own && oc_t == nc_t) {
          continue;  // neither bracket's count changed
        }
        const double old_contrib
            = edge_weight_[e]
              * ((oc_own == u_mult ? 1.0 : 0.0) - (oc_t == 0 ? 1.0 : 0.0));
        const double new_contrib
            = edge_weight_[e]
              * ((nc_own == u_mult ? 1.0 : 0.0) - (nc_t == 0 ? 1.0 : 0.0));
        if (old_contrib != new_contrib) {
          const size_t slot = static_cast<size_t>(u) * k_ + t;
          gain_[slot] += new_contrib - old_contrib;
          ++stamp_[slot];  // kills every older heap entry for (u, t)
          heaps[u_from].push(HeapEntry{gain_[slot], u, t, stamp_[slot]});
        }
      }
    }

    c[from] = nc_from;
    c[to] = nc_to;
    lambda_[e] = lam_new;
  }

  part_[v] = to;
  part_weight_[from] -= vertex_weight_[v];
  part_weight_[to] += vertex_weight_[v];
}

bool FMEngine::runPass()
{
  computeCountsAndCut();

  // Fresh gains, stamps and heaps for the pass; one heap per current
  // part so the selection step can enforce balance directionally.
  locked_.assign(n_, 0);
  gain_.resize(static_cast<size_t>(n_) * k_);
  stamp_.resize(static_cast<size_t>(n_) * k_, 0);
  std::vector<GainHeap> heaps(k_);
  for (int v = 0; v < n_; ++v) {
    for (int t = 0; t < k_; ++t) {
      if (t == part_[v]) {
        continue;
      }
      const size_t slot = static_cast<size_t>(v) * k_ + t;
      gain_[slot] = vertexGain(v, t);
      ++stamp_[slot];
      heaps[part_[v]].push(HeapEntry{gain_[slot], v, t, stamp_[slot]});
    }
  }

  const double start_cut = cut_;

  // Best-prefix tracking. Prefix 0 = the pass's starting state; a later
  // prefix only replaces it on strict improvement, so best_prefix > 0
  // is exactly "this pass improved". "Better" is lexicographic:
  // balanced beats unbalanced, then lower cut among balanced states,
  // then smaller infeasibility among unbalanced ones (progress metric
  // for the recovery mode). Earlier prefixes win ties -> fewest moves
  // kept.
  std::vector<int> move_order;
  std::vector<int> move_from;  // source part of each move, for rollback
  size_t best_prefix = 0;
  double best_cut = cut_;
  bool best_balanced = isBalanced();
  double best_infeas = infeasibility();

  // Fresh-but-infeasible entries popped while scanning for a candidate;
  // re-pushed after the winner is chosen so they stay available later in
  // the pass (their stamps are untouched, so they remain alive).
  std::vector<HeapEntry> deferred;
  std::vector<HeapEntry> cand(k_);
  std::vector<char> has_cand(k_);

  for (int step = 0; step < n_; ++step) {
    // From each part, the highest-gain movable (vertex, target) whose
    // move is acceptable. Scanning past infeasible entries mirrors the
    // classic bucket-list walk-down; with a heap that means popping them
    // into `deferred` and restoring them afterwards.
    deferred.clear();
    std::fill(has_cand.begin(), has_cand.end(), 0);
    for (int side = 0; side < k_; ++side) {
      GainHeap& heap = heaps[side];
      while (!heap.empty()) {
        const HeapEntry entry = heap.top();
        heap.pop();
        const size_t slot
            = static_cast<size_t>(entry.vertex) * k_ + entry.target;
        if (locked_[entry.vertex] || entry.stamp != stamp_[slot]) {
          continue;  // dead entry (lazy deletion)
        }
        if (moveIsAcceptable(entry.vertex, entry.target)) {
          cand[side] = entry;
          has_cand[side] = 1;
          break;
        }
        deferred.push_back(entry);
      }
    }

    // Across parts, higher gain wins; exact ties break to the smaller
    // vertex index, then the smaller target part — the same rule as
    // inside a heap, so selection stays deterministic.
    int side = -1;
    for (int s = 0; s < k_; ++s) {
      if (!has_cand[s]) {
        continue;
      }
      if (side == -1) {
        side = s;
        continue;
      }
      const HeapEntry& a = cand[s];
      const HeapEntry& b = cand[side];
      if (a.gain != b.gain) {
        if (a.gain > b.gain) {
          side = s;
        }
      } else if (a.vertex != b.vertex) {
        if (a.vertex < b.vertex) {
          side = s;
        }
      } else if (a.target < b.target) {
        side = s;
      }
    }

    // Restore scanned-past entries and the losing candidates.
    for (const HeapEntry& entry : deferred) {
      heaps[part_[entry.vertex]].push(entry);
    }
    for (int s = 0; s < k_; ++s) {
      if (s != side && has_cand[s]) {
        heaps[s].push(cand[s]);
      }
    }
    if (side == -1) {
      break;  // no acceptable move anywhere: pass ends early
    }

    // Classic FM: apply the move even if its gain is negative — the
    // best-prefix rollback below keeps only the profitable prefix, and
    // riding through negative-gain moves is what lets a pass escape
    // local minima that greedy improvement cannot.
    move_from.push_back(part_[cand[side].vertex]);
    applyMove(cand[side].vertex, cand[side].target, heaps.data());
    move_order.push_back(cand[side].vertex);

    const bool now_balanced = isBalanced();
    const double now_infeas = infeasibility();
    bool better = false;
    if (now_balanced != best_balanced) {
      better = now_balanced;
    } else if (now_balanced) {
      better = cut_ < best_cut - kEps;
    } else {
      better = now_infeas < best_infeas - kEps;
    }
    if (better) {
      best_prefix = move_order.size();
      best_cut = cut_;
      best_balanced = now_balanced;
      best_infeas = now_infeas;
    }
  }

  // Roll back to the best prefix. Only part_ and the part weights need
  // rewinding — counts and gains are rebuilt at the next pass start.
  for (size_t k = move_order.size(); k > best_prefix; --k) {
    const int v = move_order[k - 1];
    const int here = part_[v];  // the part the move landed v in
    const int back = move_from[k - 1];
    part_[v] = back;
    part_weight_[here] -= vertex_weight_[v];
    part_weight_[back] += vertex_weight_[v];
  }
  cut_ = best_cut;

  if (params_.logger != nullptr) {
    params_.logger->debug(utl::UKN,
                          "fm",
                          "pass: cut {:.6g} -> {:.6g}, moves tried {}, "
                          "kept {}, balanced {}",
                          start_cut,
                          best_cut,
                          move_order.size(),
                          best_prefix,
                          best_balanced);
  }

  return best_prefix > 0;
}

FMResult FMEngine::run(const std::vector<int>* initial)
{
  n_ = hg_.numVertices();
  m_ = hg_.numHyperedges();

  FMResult result;
  if (n_ == 0) {
    result.balanced = true;  // empty split trivially satisfies the bound
    return result;
  }

  loadWeights();
  buildIncidence();
  setInitialPartition(initial);

  for (int pass = 0; pass < params_.max_passes; ++pass) {
    ++result.passes_run;
    if (!runPass()) {
      break;
    }
  }

  // Report the cut from a fresh full count, not the incrementally
  // maintained value: same result modulo FP drift, but this one is a
  // pure fixed-order function of the final partition.
  computeCountsAndCut();
  result.partition = std::move(part_);
  result.cut_cost = cut_;
  result.balanced = isBalanced();
  return result;
}

}  // namespace

FMResult partitionFM(const Hypergraph& hg,
                     const FMParams& params,
                     const std::vector<int>* initial_partition)
{
  return FMEngine(hg, params).run(initial_partition);
}

}  // namespace eda
