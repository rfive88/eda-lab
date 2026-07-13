// eda-lab: programmatic netlist construction (Phase 0, Stage E1).
//
// Builds a dbBlock through OpenDB API calls so tests and benchmarks can
// construct netlists of any size with exactly known or statistically
// controlled topology, then hand the block to Hypergraph::buildFromBlock().
//
// Two source layers (netlistgen.h / netlistgen.cpp):
//
//   - NetlistBuilder: owns a fresh dbDatabase (tech, lib, chip, top block)
//     and wraps the create/connect calls, including the master-freeze
//     protocol OpenDB requires before instantiation. The tech/lib can be
//     either *synthetic* (connectivity-only masters with pins named
//     i0..i{n-1} / o0..o{m-1}, no geometry, built lazily on first use) or
//     *LEF-backed* (real tech + cell masters loaded via odb::lefin, see
//     loadLef()). A given builder is one or the other, never both.
//
//   - generateSynthetic(): populates a builder's block from a
//     SyntheticNetlistSpec using a seeded RNG, so a given (spec, seed) pair
//     reproduces the same netlist. Two mix regimes, selected by the spec:
//       * Legacy weighted mix (Stage A): explicit MasterSpec list; output is
//         bit-identical to Stage A for a given (spec, seed).
//       * Statistical mix (Stage B): a sequential/combinational ratio plus a
//         pin-count-bucket distribution (given directly, Mode A, or derived
//         from a target average fanout via a max-entropy solve, Mode B).
//         Works over synthetic representative masters or over LEF-loaded
//         cells classified by signal-pin count.
//
// Stage D: statistical-mix net formation is combinational-loop-free BY
// CONSTRUCTION. Instance creation order doubles as a topological (DAG) order:
// a combinational output may only drive sequential-instance inputs (any
// index) or combinational-instance inputs created LATER in the order, so
// every comb->comb edge goes strictly forward and no combinational cycle can
// form. Sequential (Q) outputs are unconstrained drivers (the loop-breaker);
// sequential inputs (D/CK) are unconstrained sinks (feedback through a
// register is legitimate, not combinational). This requires
// sequential_ratio > 0 in statistical mode (fail-fast). Stage E1's primary
// I/O ports (below) do NOT relax this: they run as a separate pass after
// net formation completes and never participate in its DAG bootstrap. The
// legacy weighted mix (Stage A path) keeps its original shuffled-pool net
// formation and makes no acyclicity guarantee.
//
// Stage D also GUARANTEES every instance ends up with >= 1 connected output
// (as hard an invariant as loop-freedom itself). The ordered statistical
// draw above can, by its own thin-tail design, leave a driver with zero
// eligible receivers (skipped, no net formed) — formNetsAcyclic's second,
// separate repair pass then gives every such instance exactly one receiver,
// respecting the same DAG rule, preferring leftover unconnected material and
// falling back to stealing a non-last sink of a multi-sink net only if none
// exists, never touching a live driver. See README.md's "Guaranteed instance
// connectivity" section for the full design (including why an earlier,
// up-front reservation scheme was discarded) and netlist_validation.h for
// the hard gate (validateNetlist) this repair exists to satisfy.
//
// Stage E1 (optional): primary input/output port generation sized by
// Rent's rule (T = k * G^p), engaged when both rent_k and rent_p are set.
// Runs as a separate pass over the already-formed dbBlock, once net
// formation completes — never a change to formation itself. NEITHER PI NOR
// PO EVER TOUCHES A LIVE DRIVER: this repo enforces "exactly one driver per
// net" (validateNetlist) AND "no dangling instances" (an instance whose
// output drives nothing) as equally hard invariants, so a PI targets Stage
// D's own leftover, never-connected internal input pins first (falling
// back to stealing a non-last sink of a multi-sink net only if that pool is
// empty), and a PO prefers claiming a leftover, never-connected output pin
// (repairing a dead-output instance) before falling back to adding one more
// sink onto any existing net. Requires the statistical mix (reuses its
// representative masters for boundary buffer/FF cells). netlistgen still
// never touches the Hypergraph engine — see RentStats below and README.md's
// "Primary I/O generation (Stage E1)" for the full rationale (including why
// is_pi/is_po end up as hyperedge, not vertex, planes). See README.md /
// FLOW.md.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace odb {
class dbBlock;
class dbDatabase;
class dbInst;
class dbMaster;
class dbNet;
}  // namespace odb

namespace utl {
class Logger;
}

namespace eda {

// Number of combinational pin-count buckets: [2, 3, 4, 5, 6-or-more].
inline constexpr int kNumCombBuckets = 5;

// Pin-count anchor for each bucket in *synthetic* mode. Bucket "6-or-more"
// is pinned to a single 7-pin representative (fanout 6, a fixed value, not a
// range): with the top anchor at 7, Mode B can reach mean fanouts up to and
// INCLUDING 6 — at exactly 6 the derived distribution degenerates to 100%
// top bucket. (It was 6 before; raised so avg fanout 6 is representable.)
inline constexpr std::array<double, kNumCombBuckets> kSyntheticBucketAnchors =
    {2.0, 3.0, 4.0, 5.0, 7.0};

class NetlistBuilder
{
 public:
  // A null `logger` (the default) makes the builder own a fresh utl::Logger;
  // pass a non-null one to share a logger with the caller (e.g. a CLI that set
  // its debug verbosity) — an external logger is used but never deleted. The
  // logger is threaded to generateSynthetic and the odb readers/writers so
  // library callers get the same debuggability the CLI does (see
  // support/logging.h; netlistgen debug group "netlistgen").
  explicit NetlistBuilder(const std::string& design_name = "synth",
                          utl::Logger* logger = nullptr);
  ~NetlistBuilder();

  NetlistBuilder(const NetlistBuilder&) = delete;
  NetlistBuilder& operator=(const NetlistBuilder&) = delete;

  // Create a connectivity-only synthetic master with input pins
  // i0..i{num_inputs-1} and output pins o0..o{num_outputs-1}. The master is
  // frozen on return and ready to instantiate. Returns nullptr if the name
  // already exists. Triggers lazy synthetic-tech creation on first use. When
  // `clocked` is true, input pin i0 is given dbSigType::CLOCK (its clock pin),
  // so the master reads as sequential via isSequentialMaster — used for the
  // synthetic sequential representative. Topology is unaffected: a CLOCK input
  // is still a sink in net formation, exactly as a SIGNAL input was.
  odb::dbMaster* makeMaster(const std::string& name,
                            int num_inputs,
                            int num_outputs,
                            bool clocked = false);

  // Returns nullptr if the name already exists in the block.
  odb::dbInst* makeInst(odb::dbMaster* master, const std::string& name);
  odb::dbNet* makeNet(const std::string& name);

  // Connect the named pin (e.g. "i0", "o0") of inst to net. Returns false
  // if the inst has no such pin.
  static bool connect(odb::dbInst* inst, const std::string& pin,
                      odb::dbNet* net);

  // Load a real tech + cell library set from LEF, replacing the synthetic
  // tech. Must be called on a fresh builder (before any make*/block() use);
  // returns false if the builder is already initialised or a LEF fails to
  // parse. tech_lef_path supplies the technology (and any macros it holds);
  // each entry in cell_lef_paths adds cells against that same tech.
  bool loadLef(const std::string& tech_lef_path,
               const std::vector<std::string>& cell_lef_paths);

  // All masters across every loaded library (synthetic or LEF-backed).
  std::vector<odb::dbMaster*> masters() const;

  // Auto-computed die bounding box (the DEF writer consumes it for DIEAREA;
  // recorded on the block). lx/ly are 0; ux/uy size a
  // near-square placement region for num_insts cells at the given target
  // utilisation, using the loaded tech's site pitch when available and a
  // nominal pitch otherwise. Instances remain UNPLACED.
  struct DieArea
  {
    int lx = 0, ly = 0, ux = 0, uy = 0;
  };
  DieArea estimateDieArea(int num_insts, double utilization = 0.7);

  odb::dbBlock* block() const { return block_; }
  odb::dbDatabase* db() const { return db_; }
  utl::Logger* logger() const { return logger_; }

 private:
  void ensureSyntheticTech();

  std::string design_name_;
  utl::Logger* logger_ = nullptr;
  bool owns_logger_ = false;  // true iff the ctor allocated logger_
  odb::dbDatabase* db_ = nullptr;
  odb::dbBlock* block_ = nullptr;
  bool tech_ready_ = false;  // synthetic or LEF tech/lib/chip/block created
};

// One cell type in the *legacy* weighted mix. `weight` is the relative
// frequency with which instances of this master are created (need not sum
// to 1). Ignored when the spec engages the statistical mix.
struct MasterSpec
{
  std::string name;
  int num_inputs = 2;
  int num_outputs = 1;
  double weight = 1.0;
};

// Fractional split of primary I/O pin types (Stage E1): each PI/PO gets one
// type sampled independently from this distribution. See
// SyntheticNetlistSpec::io_pin_type_distribution for validation/defaulting.
struct IoPinTypeDistribution
{
  double combinational = 0.70;
  double buffered = 0.20;
  double registered = 0.10;
};

struct SyntheticNetlistSpec
{
  // ---- Legacy weighted mix (Stage A) ----
  std::vector<MasterSpec> masters;  // must be non-empty in legacy mode
  int num_insts = 0;                // must be > 0

  // Nets are created until this count is reached, or until the free
  // driver/sink pin pools run out, whichever comes first. -1 means "as
  // many as the pin pools allow".
  int num_nets = -1;

  // Fanout = number of load (sink) pins per net, driver EXCLUDED, drawn
  // uniformly from [min_fanout, max_fanout]. A net gets one output pin as
  // driver plus `fanout` sink pins (fanout+1 pins total).
  int min_fanout = 2;
  int max_fanout = 4;

  uint32_t seed = 1;

  // ---- LEF-backed generation (Stage B, optional) ----
  // If tech_lef_path is set, the builder loads a real tech + cells and the
  // statistical mix draws from them; `masters` is then ignored.
  std::optional<std::string> tech_lef_path;
  std::vector<std::string> cell_lef_paths;

  // ---- Statistical cell mix (Stage B, optional) ----
  // The statistical mix is engaged when any of the four fields below (or
  // tech_lef_path) is set. When engaged, exactly one combinational-mix mode
  // must be present (combinational_pin_distribution XOR target_avg_fanout).
  //
  // Fraction of instances that are sequential; the rest are combinational.
  // MUST be set > 0 when the statistical mix is engaged (unset counts as 0
  // and fails validation): sequential Q outputs are the only bootstrap
  // signal source for the acyclic net formation until Stage E adds primary
  // input ports. Stage E relaxes this back to "sequential_ratio > 0 OR
  // primary_input_count > 0".
  std::optional<double> sequential_ratio;
  // Mode A (forward): percentages across the five pin-count buckets
  // [2, 3, 4, 5, 6+], which must sum to 100.
  std::optional<std::array<double, kNumCombBuckets>>
      combinational_pin_distribution;
  // Mode B (inverse): desired average combinational fanout, i.e. load pins per
  // net — a cell's signal pins EXCLUDING its single driver/output pin (#pins-1).
  // The generator back-solves a maximum-entropy pin-count bucket distribution
  // (anchors are pin counts = fanout+1) to hit it. Must be inside the fanout
  // range, lower bound EXCLUSIVE, upper bound INCLUSIVE (synthetic: (1, 6];
  // LEF: the measured anchor range minus one). At exactly the upper bound the
  // distribution degenerates to 100% top bucket.
  std::optional<double> target_avg_fanout;

  // After generation, empirical proportions are compared against the
  // targets; a deviation past this tolerance is a logged warning only,
  // never a generation failure.
  double distribution_tolerance_pct = 2.0;

  // ---- Peak fanout sub-clusters (optional; congestion hot-spot generation) ----
  // A subset of instances is grouped into num_peak_clusters clusters whose
  // intra-cluster nets are driven at a higher fanout (peak_avg_fanout) than
  // the background [min_fanout, max_fanout]. Layered on top of Stage D's
  // formNetsAcyclic: cluster-preferred sink selection only ever draws from
  // Stage D's already-eligible receiver pools, so the DAG/loop-freedom
  // guarantee is completely unaffected — a cluster can only bias WHICH
  // eligible receiver is picked, never make an ineligible one eligible.
  // Requires the statistical mix to be engaged (fails validation otherwise:
  // the legacy weighted-mix path has no per-instance sequential/combinational
  // classification to build cluster-safe eligibility from). Cluster
  // membership is a local bookkeeping vector inside generation, never
  // recorded on the dbBlock/Hypergraph.
  //
  // Target mean fanout (load pins, driver excluded) for nets driven from
  // inside a peak cluster. Engages the feature when set. Must be strictly
  // greater than the background average fanout (min_fanout + max_fanout)/2.
  std::optional<double> peak_avg_fanout;
  // Fraction of instances assigned to peak clusters (split evenly across
  // num_peak_clusters). Must be in (0, 1), exclusive both ends. Defaults to
  // 0.10 when peak_avg_fanout is set but this is not. Ignored (no
  // validation, no effect) when peak_avg_fanout is unset.
  std::optional<double> peak_cluster_pct;
  // Number of peak clusters. Must be >= 1. Defaults to 1. Ignored when
  // peak_avg_fanout is unset.
  std::optional<int> num_peak_clusters;

  // ---- Stage E1: primary I/O generation via Rent's rule (optional) ----
  // Engaged when BOTH rent_k and rent_p are set (exactly one alone is a
  // validation error). Requires the statistical mix — boundary buffer/FF
  // cells reuse its already-resolved combinational/sequential representative
  // masters, mirroring the peak-cluster feature's same requirement.
  //
  // Rent coefficient k in T = k * G^p. Must be > 0.
  std::optional<double> rent_k;
  // Rent exponent p. Must be in (0, 1.2]: (0, 1.0] is used as given; (1.0,
  // 1.2] is accepted but logged as a warning and CLAMPED to 1.0 for the T
  // computation (degenerate but tolerable — p > 1 is physically unusual);
  // > 1.2 is a validation error.
  std::optional<double> rent_p;
  // Fraction of the T terminals assigned as primary inputs; the remainder
  // are primary outputs. Must be in (0, 1). Defaults to 0.60.
  std::optional<double> io_input_ratio;
  // Fractional split of pin types, applied independently to the PI and PO
  // populations. Each fraction must be in [0, 1]; the three must sum to
  // 1.0 +/- 0.01 (normalised silently if within tolerance but not exactly
  // 1.0; a sum outside tolerance is a validation error). Defaults to
  // {0.70, 0.20, 0.10} when unset.
  std::optional<IoPinTypeDistribution> io_pin_type_distribution;

  // True when the statistical mix should drive generation.
  bool usesStatisticalMix() const
  {
    return tech_lef_path.has_value() || sequential_ratio.has_value()
           || combinational_pin_distribution.has_value()
           || target_avg_fanout.has_value();
  }
};

// Per-cluster Rent statistics (Stage E1, Step 5): computed only when the
// peak fanout sub-cluster feature is also engaged (cluster_id has at least
// one non-background entry). A cluster with G_c < 2 or T_c < 1 is
// degenerate and omitted here (logged as a warning), never included with
// meaningless values.
struct ClusterRentStats
{
  int cluster_idx = -1;
  int G_c = 0;      // ORIGINAL internal instances (not boundary buf/FF) in
                    // this cluster
  int T_c = 0;      // cut nets: >=1 endpoint inside, >=1 endpoint outside
                    // this cluster (an endpoint is an owning instance's
                    // cluster, or -1/outside-every-cluster for a bTerm or a
                    // boundary buf/FF instance)
  double p_c = 0.0;
  double k_c = 0.0;
};

// Statistics and generated artifacts from Stage E1 (primary I/O generation
// via Rent's rule; T = k * G^p), returned from generateSynthetic via the
// optional `out_rent_stats` parameter. `engaged` is false (every other
// field default/empty) whenever rent_k/rent_p were not both set — the
// pipeline then behaves exactly as it did before Stage E1 existed.
//
// netlistgen never reads or writes Hypergraph attribute planes (see the
// "Input / output contract" in README.md) — the raw dbNet*/dbInst* lists
// below let a caller that wants hgm.is_pi / hgm.is_po / hgm.is_boundary_buf
// / hgm.is_boundary_reg hypergraph planes build them itself, from data
// already collected here, after its own Hypergraph::buildFromBlock() call.
// is_pi/is_po are naturally HYPEREDGE (net) planes — a bare port has no
// dbInst and hence no vertex in this codebase's dbInst-only vertex model —
// while is_boundary_buf/is_boundary_reg are VERTEX planes on the real
// boundary instances Stage E1 creates. See README.md's "Primary I/O
// generation (Stage E1)" section for the full rationale.
struct RentStats
{
  bool engaged = false;

  // ---- Target (Step 1) ----
  double rent_k_target = 0.0;  // as given; rent_k itself is never clamped
  double rent_p_target = 0.0;  // EFFECTIVE value used for T: clamped to 1.0
                               // if the input was in (1.0, 1.2]
  int G = 0;                   // internal instance count (spec.num_insts)
  int T_target = 0;            // round(rent_k * G^rent_p_target)
  bool capped = false;         // true if PI/PO target-pin supply fell short
                               // of Rent's rule's request (T_in was capped
                               // to the available pool up front, and/or an
                               // individual PI/PO port had to be skipped
                               // for lack of remaining material) — see
                               // README.md's "Primary I/O generation
                               // (Stage E1)" section

  // ---- Actual (Steps 2, 3, 6) ----
  int T_in = 0;      // PI count actually created
  int T_out = 0;     // PO count actually created
  int T_actual = 0;  // T_in + T_out
  double k_actual = 0.0;
  double p_actual = 0.0;

  int n_combinational = 0;  // PI+PO combined, by pin type
  int n_buffered = 0;
  int n_registered = 0;
  int n_boundary_ff = 0;  // == n_registered; tracked separately from
                         // spec.sequential_ratio, which this never touches

  std::vector<odb::dbNet*> pi_nets;
  std::vector<odb::dbNet*> po_nets;
  std::vector<odb::dbInst*> boundary_buf_insts;
  std::vector<odb::dbInst*> boundary_reg_insts;

  // ---- Sub-cluster / background (Step 5), only when clusters are engaged ----
  bool has_clusters = false;
  std::vector<ClusterRentStats> cluster_rent;
  bool background_valid = false;  // false if G_bg<2 or T_bg<1 (degenerate,
                                  // logged; the fields below are then 0)
  int G_bg = 0;
  int T_bg = 0;
  double p_bg = 0.0;
  double k_bg = 0.0;
};

// Populate builder's block from the spec. Instances are named u0..u{n-1}
// and nets n0..n{k-1}. Deterministic for a given (spec, seed). Returns the
// number of nets created, or -1 if the spec fails validation (bad config,
// or a requested bucket/class with no matching masters in a loaded LEF).
// If `out_cluster_id` is non-null and the spec engages peak fanout
// sub-clusters (statistical mix + peak_avg_fanout set), it is resized to
// num_insts and filled with each instance's cluster id, index-aligned with
// creation order (u<i>); -1 means background. Left untouched otherwise
// (legacy mix, invalid spec, or no peak_avg_fanout). Exposed purely so
// tests can verify cluster-biased fanout without adding cluster membership
// to the persistent dbBlock/Hypergraph data model.
// If `out_rent_stats` is non-null and the spec engages Stage E1 (both
// rent_k and rent_p set), it is filled with the statistics/artifacts above.
// Left untouched otherwise.
int generateSynthetic(NetlistBuilder& builder,
                      const SyntheticNetlistSpec& spec,
                      std::vector<int>* out_cluster_id = nullptr,
                      RentStats* out_rent_stats = nullptr);

// ---- Shared statistical-mix helpers (exposed for testing) ----

// Number of signal pins on a master: dbMTerms with dbSigType SIGNAL or
// CLOCK, excluding POWER/GROUND. One shared counting rule for both paths.
int signalPinCount(odb::dbMaster* master);

// True if the master has a clock pin — a dbMTerm with dbSigType::CLOCK, or an
// INPUT pin with a conventional clock name (CK/CLK/CLOCK/CP) for libraries that
// tag the clock pin USE SIGNAL (e.g. Nangate45). The auto-detection rule for
// sequential (flip-flop) cells in LEF mode.
bool isSequentialMaster(odb::dbMaster* master);

// True if the master is a level-sensitive latch: it has a latch gate/enable pin
// (INPUT named G/GN) but no clock pin, so it is neither a flip-flop nor a plain
// combinational gate. Latches are dropped entirely in LEF mode — used as
// neither sequential nor combinational masters.
bool isLatchMaster(odb::dbMaster* master);

// True if the master is a clock-gating cell (integrated clock gate): it drives a
// gated-clock OUTPUT pin (named GCK/GCLK/ECK). Even though it carries a clock
// pin, it is dropped entirely in LEF mode — not used as a sequential (or any)
// master.
bool isClockGateMaster(odb::dbMaster* master);

// Config-only spec validation (everything that does not depend on a loaded
// LEF library): mutual exclusivity of the two combinational modes, the
// distribution summing to 100, sequential_ratio in (0, 1] (strictly positive
// since Stage D — the acyclic net formation needs sequential Q outputs as
// its bootstrap signal source until Stage E adds primary inputs), and
// target_avg_fanout inside the synthetic fanout range (1, 6]. Logs the
// first problem via `logger` (if non-null) and returns false. Returns true
// for legacy-mode specs (they are validated by generateSynthetic itself).
bool validateSpecConfig(const SyntheticNetlistSpec& spec, utl::Logger* logger);

// Maximum-entropy bucket distribution over `anchors` whose weighted mean
// equals `target_mean`: p_i = exp(theta*a_i)/Z with a single scalar theta
// found by bisection. `target_mean` must lie in (min(anchors), max(anchors)]
// — at exactly max(anchors) the bisection saturates (theta = 50) and the
// result is numerically the degenerate all-top-bucket distribution. Spreads
// mass across all buckets as evenly as the mean constraint allows.
std::array<double, kNumCombBuckets> maxEntropyDistribution(
    const std::array<double, kNumCombBuckets>& anchors,
    double target_mean);

// Peak-fanout sub-cluster assignment: shuffles [0, num_insts) with `rng`
// (consuming it exactly once) and slices off num_peak_clusters contiguous
// chunks of floor(peak_cluster_pct * num_insts / num_peak_clusters)
// instances each, assigned cluster ids 0..num_peak_clusters-1 in order;
// every other instance gets -1 (background). Pure bookkeeping — the result
// is never attached to the dbBlock/Hypergraph model, only consumed
// in-memory by formNetsAcyclic's cluster-preferred sink selection. Exposed
// for direct unit testing of cluster-size / determinism properties.
std::vector<int> assignPeakClusters(int num_insts,
                                    double peak_cluster_pct,
                                    int num_peak_clusters,
                                    std::mt19937& rng);

}  // namespace eda
