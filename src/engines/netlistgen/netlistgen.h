// eda-lab: programmatic netlist construction (Phase 0, Stage B).
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
// Stage B does NOT yet guarantee combinational-loop freedom — net formation
// still pairs drivers and sinks from shuffled pools, so cycles are possible.
// Provably-acyclic net formation lands in Stage C. See README.md / FLOW.md.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
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
// is pinned to exactly 6 (a fixed value, not a range) this stage.
inline constexpr std::array<double, kNumCombBuckets> kSyntheticBucketAnchors =
    {2.0, 3.0, 4.0, 5.0, 6.0};

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
  // already exists. Triggers lazy synthetic-tech creation on first use.
  odb::dbMaster* makeMaster(const std::string& name,
                            int num_inputs,
                            int num_outputs);

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

  // Auto-computed die bounding box (Stage D's DEF writer will consume it;
  // this stage only records it on the block). lx/ly are 0; ux/uy size a
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

struct SyntheticNetlistSpec
{
  // ---- Legacy weighted mix (Stage A) ----
  std::vector<MasterSpec> masters;  // must be non-empty in legacy mode
  int num_insts = 0;                // must be > 0

  // Nets are created until this count is reached, or until the free
  // driver/sink pin pools run out, whichever comes first. -1 means "as
  // many as the pin pools allow".
  int num_nets = -1;

  // Total pins per net, driver included, drawn uniformly from
  // [min_fanout, max_fanout]. A net gets one output pin as driver and
  // fanout-1 sink pins.
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
  // Defaults to 0.0 when engaged but unset (0.0 is valid this stage).
  std::optional<double> sequential_ratio;
  // Mode A (forward): percentages across the five pin-count buckets
  // [2, 3, 4, 5, 6+], which must sum to 100.
  std::optional<std::array<double, kNumCombBuckets>>
      combinational_pin_distribution;
  // Mode B (inverse): desired average signal-pin fanout. The generator
  // back-solves a maximum-entropy bucket distribution to hit it. Must be
  // strictly inside the anchor range (synthetic: (2, 6)).
  std::optional<double> target_avg_fanout;

  // After generation, empirical proportions are compared against the
  // targets; a deviation past this tolerance is a logged warning only,
  // never a generation failure.
  double distribution_tolerance_pct = 2.0;

  // True when the statistical mix should drive generation.
  bool usesStatisticalMix() const
  {
    return tech_lef_path.has_value() || sequential_ratio.has_value()
           || combinational_pin_distribution.has_value()
           || target_avg_fanout.has_value();
  }
};

// Populate builder's block from the spec. Instances are named u0..u{n-1}
// and nets n0..n{k-1}. Deterministic for a given (spec, seed). Returns the
// number of nets created, or -1 if the spec fails validation (bad config,
// or a requested bucket/class with no matching masters in a loaded LEF).
int generateSynthetic(NetlistBuilder& builder,
                      const SyntheticNetlistSpec& spec);

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

// Config-only spec validation (everything that does not depend on a loaded
// LEF library): mutual exclusivity of the two combinational modes, the
// distribution summing to 100, sequential_ratio in [0, 1], and
// target_avg_fanout strictly inside the synthetic anchor range. Logs the
// first problem via `logger` (if non-null) and returns false. Returns true
// for legacy-mode specs (they are validated by generateSynthetic itself).
bool validateSpecConfig(const SyntheticNetlistSpec& spec, utl::Logger* logger);

// Maximum-entropy bucket distribution over `anchors` whose weighted mean
// equals `target_mean`: p_i = exp(theta*a_i)/Z with a single scalar theta
// found by bisection. `target_mean` must lie strictly inside
// [min(anchors), max(anchors)]. Spreads mass across all buckets as evenly
// as the mean constraint allows.
std::array<double, kNumCombBuckets> maxEntropyDistribution(
    const std::array<double, kNumCombBuckets>& anchors,
    double target_mean);

}  // namespace eda
