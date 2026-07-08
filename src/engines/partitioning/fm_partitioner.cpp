// Flat 2-way FM. See fm_partitioner.h for the API contract.
//
// Implementation notes
// --------------------
// Gain container: hyperedge weights are doubles, so the classic
// integer-keyed bucket list does not apply directly. The options were
// (a) discretize gains into buckets — O(1) selection but adds a
// quantization knob whose granularity interacts with weight scale, or
// (b) a max-heap keyed by the exact double gain with lazy deletion —
// O(log n) selection, no quantization, much simpler to keep correct.
// Stage 1 uses (b): every gain change pushes a fresh entry and bumps a
// per-vertex stamp; entries whose stamp no longer matches are dead and
// get dropped when popped. Revisit (a) only if selection ever shows up
// in a profile.
//
// Multiplicity: a vertex can hold several pins of one hyperedge (one CSR
// entry per dbITerm). Moving the vertex moves all of them at once, so all
// count/gain math runs on distinct (vertex, multiplicity) incidence pairs,
// precomputed once per run from the CSR arrays.
//
// Gain bookkeeping — the classic FM bug source — avoids the textbook
// "if T(e)==0 / T(e)==1" case rules entirely (they assume unit
// multiplicity). Instead it uses one exact formula. With n_s(e) = pins of
// e in side s and vertex u in side `from` holding mult(u,e) pins:
//
//   contrib(u, e) = w(e) * [n_from(e) == mult(u,e)]   // move empties from
//                 - w(e) * [n_to(e)   == 0]           // move newly cuts e
//   gain(u) = sum over u's distinct edges of contrib(u, e)
//
// (When both brackets hold, e lies entirely inside `from` and the terms
// cancel — moving u drags the whole edge along, uncut either way.) When a
// vertex moves, each touched edge's counts change once, and every other
// unlocked member vertex's gain is adjusted by contrib(after) −
// contrib(before). That difference form is self-correcting: it only needs
// the counts to be right, not a case analysis to be exhaustive.

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
  uint64_t stamp;  // matches stamp_[vertex] iff this entry is alive
};

// Max-heap on gain; equal gains break toward the smaller vertex index so
// selection order is a pure function of the gain values (determinism).
struct EntryOrder
{
  bool operator()(const HeapEntry& a, const HeapEntry& b) const
  {
    if (a.gain != b.gain) {
      return a.gain < b.gain;
    }
    return a.vertex > b.vertex;
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
      : hg_(hg), params_(params), rng_(params.seed)
  {
  }

  FMResult run(const std::vector<int>* initial);

 private:
  void loadWeights();
  void buildIncidence();
  void setInitialPartition(const std::vector<int>* initial);
  void computeCountsAndCut();
  double vertexGain(int v) const;
  bool isBalanced() const;
  double imbalance() const { return std::abs(part_weight_[0] - total_weight_ / 2.0); }
  bool moveIsAcceptable(int v) const;
  void applyMove(int v, GainHeap* heaps);
  bool runPass();

  const Hypergraph& hg_;
  const FMParams& params_;

  int n_ = 0;  // vertices
  int m_ = 0;  // hyperedges

  std::vector<double> vertex_weight_;  // "area" plane or 1.0
  std::vector<double> edge_weight_;    // "weight" plane or 1.0
  double total_weight_ = 0.0;
  double min_part_ = 0.0;  // (1 - tol) * W/2
  double max_part_ = 0.0;  // (1 + tol) * W/2

  // Distinct incidence with multiplicity, both directions, CSR-style:
  // vertex v's distinct edges are (v_edge_[i], v_mult_[i]) for i in
  // [v_edge_off_[v], v_edge_off_[v+1]); e_* mirrors for hyperedges.
  std::vector<int> v_edge_off_, v_edge_, v_mult_;
  std::vector<int> e_vert_off_, e_vert_, e_mult_;

  std::vector<int> part_;      // vertex -> 0/1
  double part_weight_[2] = {0.0, 0.0};
  std::vector<int> pins_in_[2];  // per edge: pin count in each side
  double cut_ = 0.0;

  std::vector<double> gain_;
  std::vector<uint64_t> stamp_;
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
  min_part_ = (1.0 - params_.balance_tolerance) * total_weight_ / 2.0;
  max_part_ = (1.0 + params_.balance_tolerance) * total_weight_ / 2.0;

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
        && std::all_of(initial->begin(), initial->end(), [](const int p) {
             return p == 0 || p == 1;
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
                           "(null, wrong size, or values outside {{0,1}}); "
                           "falling back to seeded random initial");
    }
    // Balanced random initial: visit vertices in a seeded Fisher–Yates
    // order (own loop, not std::shuffle — see drawBelow) and put each on
    // the currently lighter side. Randomness lives in the visit order;
    // the greedy placement is what guarantees the result is balanced
    // whenever balance is achievable at all (final side gap <= max
    // single vertex weight).
    std::vector<int> order(n_);
    for (int v = 0; v < n_; ++v) {
      order[v] = v;
    }
    for (int i = n_ - 1; i > 0; --i) {
      std::swap(order[i], order[drawBelow(rng_, i + 1)]);
    }
    part_.assign(n_, 0);
    double w0 = 0.0;
    double w1 = 0.0;
    for (const int v : order) {
      const int side = (w0 <= w1) ? 0 : 1;
      part_[v] = side;
      (side == 0 ? w0 : w1) += vertex_weight_[v];
    }
  }

  part_weight_[0] = part_weight_[1] = 0.0;
  for (int v = 0; v < n_; ++v) {
    part_weight_[part_[v]] += vertex_weight_[v];
  }
}

void FMEngine::computeCountsAndCut()
{
  const std::vector<int>& eoff = hg_.hyperedgeOffsets();
  const std::vector<int>& pins = hg_.pinList();
  pins_in_[0].assign(m_, 0);
  pins_in_[1].assign(m_, 0);
  cut_ = 0.0;
  for (int e = 0; e < m_; ++e) {
    for (int p = eoff[e]; p < eoff[e + 1]; ++p) {
      ++pins_in_[part_[pins[p]]][e];
    }
    if (pins_in_[0][e] > 0 && pins_in_[1][e] > 0) {
      cut_ += edge_weight_[e];
    }
  }
}

double FMEngine::vertexGain(const int v) const
{
  // See the file header: exact count-based form, valid for any pin
  // multiplicity, summed in fixed incidence order (determinism).
  const int from = part_[v];
  const int to = 1 - from;
  double g = 0.0;
  for (int i = v_edge_off_[v]; i < v_edge_off_[v + 1]; ++i) {
    const int e = v_edge_[i];
    const int mult = v_mult_[i];
    if (pins_in_[from][e] == mult) {
      g += edge_weight_[e];
    }
    if (pins_in_[to][e] == 0) {
      g -= edge_weight_[e];
    }
  }
  return g;
}

bool FMEngine::isBalanced() const
{
  return part_weight_[0] >= min_part_ - kEps
         && part_weight_[0] <= max_part_ + kEps
         && part_weight_[1] >= min_part_ - kEps
         && part_weight_[1] <= max_part_ + kEps;
}

bool FMEngine::moveIsAcceptable(const int v) const
{
  const int from = part_[v];
  const double new_from = part_weight_[from] - vertex_weight_[v];
  const double new_to = part_weight_[1 - from] + vertex_weight_[v];
  if (new_from >= min_part_ - kEps && new_to <= max_part_ + kEps) {
    return true;  // move keeps (or establishes) balance
  }
  // Recovery mode for an unbalanced provided initial: a move that
  // strictly shrinks the deviation from W/2 is allowed even though the
  // result is still out of tolerance, so FM can walk back into the
  // feasible region instead of freezing. Never fires from a balanced
  // state (there the branch above already accepted any tolerable move,
  // and an intolerable one must not be taken regardless of deviation).
  if (isBalanced()) {
    return false;
  }
  const double new_w0 = (from == 0) ? new_from : new_to;
  return std::abs(new_w0 - total_weight_ / 2.0) < imbalance() - kEps;
}

void FMEngine::applyMove(const int v, GainHeap* heaps)
{
  const int from = part_[v];
  const int to = 1 - from;
  locked_[v] = 1;

  for (int i = v_edge_off_[v]; i < v_edge_off_[v + 1]; ++i) {
    const int e = v_edge_[i];
    const int mult = v_mult_[i];

    // Counts before/after v's pins change sides.
    const int old_cnt[2] = {pins_in_[0][e], pins_in_[1][e]};
    int new_cnt[2] = {old_cnt[0], old_cnt[1]};
    new_cnt[from] -= mult;
    new_cnt[to] += mult;

    // Cut delta straight from the counts — no case analysis to get wrong.
    const bool was_cut = old_cnt[0] > 0 && old_cnt[1] > 0;
    const bool now_cut = new_cnt[0] > 0 && new_cnt[1] > 0;
    if (was_cut != now_cut) {
      cut_ += now_cut ? edge_weight_[e] : -edge_weight_[e];
    }

    // Re-derive every other member's contribution from e under the old
    // and new counts; the difference is its gain adjustment. Locked
    // vertices are skipped — they cannot move again this pass, so their
    // gains are dead until the pass-start rebuild.
    for (int j = e_vert_off_[e]; j < e_vert_off_[e + 1]; ++j) {
      const int u = e_vert_[j];
      if (u == v || locked_[u]) {
        continue;
      }
      const int u_from = part_[u];
      const int u_to = 1 - u_from;
      const int u_mult = e_mult_[j];
      const double old_contrib
          = edge_weight_[e]
            * ((old_cnt[u_from] == u_mult ? 1.0 : 0.0)
               - (old_cnt[u_to] == 0 ? 1.0 : 0.0));
      const double new_contrib
          = edge_weight_[e]
            * ((new_cnt[u_from] == u_mult ? 1.0 : 0.0)
               - (new_cnt[u_to] == 0 ? 1.0 : 0.0));
      if (old_contrib != new_contrib) {
        gain_[u] += new_contrib - old_contrib;
        ++stamp_[u];  // kills every older heap entry for u
        heaps[u_from].push(HeapEntry{gain_[u], u, stamp_[u]});
      }
    }

    pins_in_[0][e] = new_cnt[0];
    pins_in_[1][e] = new_cnt[1];
  }

  part_[v] = to;
  part_weight_[from] -= vertex_weight_[v];
  part_weight_[to] += vertex_weight_[v];
}

bool FMEngine::runPass()
{
  computeCountsAndCut();

  // Fresh gains, stamps and heaps for the pass; one heap per current
  // side so the selection step can enforce balance directionally.
  locked_.assign(n_, 0);
  gain_.resize(n_);
  stamp_.resize(n_, 0);
  GainHeap heaps[2];
  for (int v = 0; v < n_; ++v) {
    gain_[v] = vertexGain(v);
    ++stamp_[v];
    heaps[part_[v]].push(HeapEntry{gain_[v], v, stamp_[v]});
  }

  const double start_cut = cut_;

  // Best-prefix tracking. Prefix 0 = the pass's starting state; a later
  // prefix only replaces it on strict improvement, so best_prefix > 0
  // is exactly "this pass improved". "Better" is lexicographic:
  // balanced beats unbalanced, then lower cut among balanced states,
  // then smaller imbalance among unbalanced ones (progress metric for
  // the recovery mode). Earlier prefixes win ties -> fewest moves kept.
  std::vector<int> move_order;
  size_t best_prefix = 0;
  double best_cut = cut_;
  bool best_balanced = isBalanced();
  double best_imb = imbalance();

  // Fresh-but-infeasible entries popped while scanning for a candidate;
  // re-pushed after the winner is chosen so they stay available later in
  // the pass (their stamps are untouched, so they remain alive).
  std::vector<HeapEntry> deferred;

  for (int step = 0; step < n_; ++step) {
    // From each side, the highest-gain movable vertex whose move is
    // acceptable. Scanning past infeasible entries mirrors the classic
    // bucket-list walk-down; with a heap that means popping them into
    // `deferred` and restoring them afterwards.
    bool has_cand[2] = {false, false};
    HeapEntry cand[2];
    deferred.clear();
    for (int side = 0; side < 2; ++side) {
      GainHeap& heap = heaps[side];
      while (!heap.empty()) {
        const HeapEntry entry = heap.top();
        heap.pop();
        if (locked_[entry.vertex] || entry.stamp != stamp_[entry.vertex]) {
          continue;  // dead entry (lazy deletion)
        }
        if (moveIsAcceptable(entry.vertex)) {
          cand[side] = entry;
          has_cand[side] = true;
          break;
        }
        deferred.push_back(entry);
      }
    }

    int side = -1;
    if (has_cand[0] && has_cand[1]) {
      // Higher gain wins; exact ties break to the smaller vertex index,
      // same rule as inside a heap, so selection stays deterministic.
      if (cand[0].gain != cand[1].gain) {
        side = cand[0].gain > cand[1].gain ? 0 : 1;
      } else {
        side = cand[0].vertex < cand[1].vertex ? 0 : 1;
      }
    } else if (has_cand[0] || has_cand[1]) {
      side = has_cand[0] ? 0 : 1;
    }

    // Restore scanned-past entries and the losing candidate.
    for (const HeapEntry& entry : deferred) {
      heaps[part_[entry.vertex]].push(entry);
    }
    if (side != -1 && has_cand[1 - side]) {
      heaps[1 - side].push(cand[1 - side]);
    }
    if (side == -1) {
      break;  // no acceptable move anywhere: pass ends early
    }

    // Classic FM: apply the move even if its gain is negative — the
    // best-prefix rollback below keeps only the profitable prefix, and
    // riding through negative-gain moves is what lets a pass escape
    // local minima that greedy improvement cannot.
    applyMove(cand[side].vertex, heaps);
    move_order.push_back(cand[side].vertex);

    const bool now_balanced = isBalanced();
    const double now_imb = imbalance();
    bool better = false;
    if (now_balanced != best_balanced) {
      better = now_balanced;
    } else if (now_balanced) {
      better = cut_ < best_cut - kEps;
    } else {
      better = now_imb < best_imb - kEps;
    }
    if (better) {
      best_prefix = move_order.size();
      best_cut = cut_;
      best_balanced = now_balanced;
      best_imb = now_imb;
    }
  }

  // Roll back to the best prefix. Only part_ and the side weights need
  // rewinding — counts and gains are rebuilt at the next pass start.
  for (size_t k = move_order.size(); k > best_prefix; --k) {
    const int v = move_order[k - 1];
    const int from = part_[v];
    part_[v] = 1 - from;
    part_weight_[from] -= vertex_weight_[v];
    part_weight_[1 - from] += vertex_weight_[v];
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
