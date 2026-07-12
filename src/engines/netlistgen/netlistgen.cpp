// See netlistgen.h for the construction protocol and generator model.

#include "engines/netlistgen/netlistgen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>

#include "odb/db.h"
#include "odb/geom.h"
#include "odb/lefin.h"
#include "support/logging.h"
#include "utl/Logger.h"

namespace eda {

namespace {

// Debug group for netlistgen library verbosity. generateSynthetic and
// NetlistBuilder are library entry points; their phase markers are debug-gated
// (group "netlistgen") so in-memory callers stay silent at verbosity 0 while
// the standalone CLI's default info-level markers narrate the run. See
// support/logging.h for the level scheme.
constexpr const char* kGroup = "netlistgen";

// Message ids for netlistgen diagnostics (utl::UKN group; hypergraph uses
// the 100s, so netlistgen takes the 300s). warn() never throws at the pinned
// SHA — Logger::error() does, and this engine signals failure by return value.
constexpr int kMsgBothModes = 300;
constexpr int kMsgNoMode = 301;
constexpr int kMsgDistSum = 302;
constexpr int kMsgSeqRatio = 303;
constexpr int kMsgTargetRange = 304;
constexpr int kMsgEmptyBucket = 305;
constexpr int kMsgEmptySeq = 306;
constexpr int kMsgExcludeMaster = 307;
constexpr int kMsgDerivedDist = 308;
constexpr int kMsgTolerance = 309;
constexpr int kMsgLefLoad = 310;
constexpr int kMsgNoLefMasters = 311;
constexpr int kMsgSeqBootstrap = 312;
constexpr int kMsgPeakConfig = 313;
constexpr int kMsgRentConfig = 314;
constexpr int kMsgRentCap = 315;
constexpr int kMsgRentDegenerate = 316;

// Implementation constant: fraction of a cluster net's sink slots preferring
// an intra-cluster receiver over background. Not exposed in the JSON config
// (spec brief: "p_intra = 0.8, hardcoded").
constexpr double kPeakIntraClusterProb = 0.8;
// Rejection-sampling cap for "pick an eligible receiver in cluster c": after
// this many misses, fall back to any eligible receiver (background). Keeps
// the draw O(1) in the common case while still degrading gracefully — same
// as the brief's documented edge case ("cluster c has fewer [eligible] cells
// than the requested fanout") — to background, never a duplicate/stalled pick.
constexpr int kPeakClusterPickAttempts = 20;

// Bucket 0..4 for a signal-pin count: 2->0, 3->1, 4->2, 5->3, >=6->4.
// Anything below 2 (tie/fill/antenna cells) belongs to no bucket.
int bucketIndex(int signal_pins)
{
  if (signal_pins < 2) {
    return -1;
  }
  if (signal_pins >= 6) {
    return kNumCombBuckets - 1;
  }
  return signal_pins - 2;
}

// Count of OUTPUT-direction signal pins (a combinational cell must have
// exactly one). CLOCK counts as signal, POWER/GROUND never do.
int signalOutputCount(odb::dbMaster* master)
{
  int outs = 0;
  for (odb::dbMTerm* mterm : master->getMTerms()) {
    const odb::dbSigType st = mterm->getSigType();
    if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
      continue;
    }
    if (mterm->getIoType() == odb::dbIoType::OUTPUT) {
      ++outs;
    }
  }
  return outs;
}

// Case-insensitive match of a pin name against a fixed candidate list.
bool pinNameIsOneOf(const std::string& name,
                    std::initializer_list<const char*> candidates)
{
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  for (const char* candidate : candidates) {
    if (upper == candidate) {
      return true;
    }
  }
  return false;
}

// A clock input pin marks a sequential cell. Many libraries (Nangate45 among
// them) declare the clock pin with USE SIGNAL rather than USE CLOCK, so the
// sig type alone misses every flip-flop. Recognise the conventional clock-pin
// names as a fallback. Combinational cells in these libraries never use these
// names (verified against Nangate45), so this does not misclassify them.
bool isClockPinName(const std::string& name)
{
  return pinNameIsOneOf(name, {"CK", "CLK", "CLOCK", "CP"});
}

// A level-sensitive latch's gate/enable pin. In Nangate45 these names (G/GN)
// belong exclusively to the D-latch cells (DLH/DLL/TLAT) and never to a
// combinational gate, so they cleanly single out latches.
bool isLatchEnablePinName(const std::string& name)
{
  return pinNameIsOneOf(name, {"G", "GN"});
}

// A clock-gating cell's gated-clock output pin. In Nangate45 GCK is the OUTPUT
// of exactly the CLKGATE/CLKGATETST cells; GCLK/ECK are the same idea in other
// libraries. Being an output name, it never collides with a data output.
bool isGatedClockPinName(const std::string& name)
{
  return pinNameIsOneOf(name, {"GCK", "GCLK", "ECK"});
}

double meanOfTilt(const std::array<double, kNumCombBuckets>& anchors,
                  double theta)
{
  // Numerically stable softmax-weighted mean: subtract the max exponent.
  double amax = *std::max_element(anchors.begin(), anchors.end());
  double z = 0.0;
  double num = 0.0;
  for (double a : anchors) {
    const double w = std::exp(theta * (a - amax));
    z += w;
    num += w * a;
  }
  return num / z;
}

}  // namespace

int signalPinCount(odb::dbMaster* master)
{
  int pins = 0;
  for (odb::dbMTerm* mterm : master->getMTerms()) {
    const odb::dbSigType st = mterm->getSigType();
    if (st == odb::dbSigType::SIGNAL || st == odb::dbSigType::CLOCK) {
      ++pins;
    }
  }
  return pins;
}

bool isSequentialMaster(odb::dbMaster* master)
{
  for (odb::dbMTerm* mterm : master->getMTerms()) {
    if (mterm->getSigType() == odb::dbSigType::CLOCK) {
      return true;
    }
    // Fallback for libraries that mark the clock pin USE SIGNAL (e.g. Nangate45,
    // where a DFF's CK pin is USE SIGNAL and the cell has two outputs Q/QN — so
    // without this it would be miscounted as a multi-output combinational cell
    // and excluded, leaving no sequential masters to satisfy sequential_ratio).
    if (mterm->getIoType() == odb::dbIoType::INPUT
        && isClockPinName(mterm->getName())) {
      return true;
    }
  }
  return false;
}

bool isLatchMaster(odb::dbMaster* master)
{
  // A cell with a clock pin is a flip-flop (or clock gate), not a plain latch.
  if (isSequentialMaster(master)) {
    return false;
  }
  for (odb::dbMTerm* mterm : master->getMTerms()) {
    if (mterm->getIoType() == odb::dbIoType::INPUT
        && isLatchEnablePinName(mterm->getName())) {
      return true;
    }
  }
  return false;
}

bool isClockGateMaster(odb::dbMaster* master)
{
  for (odb::dbMTerm* mterm : master->getMTerms()) {
    if (mterm->getIoType() == odb::dbIoType::OUTPUT
        && isGatedClockPinName(mterm->getName())) {
      return true;
    }
  }
  return false;
}

std::array<double, kNumCombBuckets> maxEntropyDistribution(
    const std::array<double, kNumCombBuckets>& anchors,
    double target_mean)
{
  // mean(theta) is monotonically increasing in theta from min(anchors)
  // (theta -> -inf) to max(anchors) (theta -> +inf); bisect for the theta
  // that hits target_mean.
  double lo = -50.0;
  double hi = 50.0;
  for (int it = 0; it < 200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (meanOfTilt(anchors, mid) < target_mean) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double theta = 0.5 * (lo + hi);

  const double amax = *std::max_element(anchors.begin(), anchors.end());
  std::array<double, kNumCombBuckets> p{};
  double z = 0.0;
  for (int i = 0; i < kNumCombBuckets; ++i) {
    p[i] = std::exp(theta * (anchors[i] - amax));
    z += p[i];
  }
  for (double& v : p) {
    v /= z;
  }
  return p;
}

std::vector<int> assignPeakClusters(int num_insts,
                                    double peak_cluster_pct,
                                    int num_peak_clusters,
                                    std::mt19937& rng)
{
  std::vector<int> cluster_id(num_insts, -1);
  std::vector<int> order(num_insts);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), rng);

  const int cluster_size = static_cast<int>(
      std::floor(peak_cluster_pct * num_insts / num_peak_clusters));
  int pos = 0;
  for (int k = 0; k < num_peak_clusters; ++k) {
    for (int j = 0; j < cluster_size; ++j, ++pos) {
      cluster_id[order[pos]] = k;
    }
  }
  return cluster_id;
}

bool validateSpecConfig(const SyntheticNetlistSpec& spec, utl::Logger* logger)
{
  if (spec.num_insts <= 0) {
    if (logger) {
      logger->warn(utl::UKN, kMsgNoMode, "num_insts must be > 0");
    }
    return false;
  }
  if (!spec.usesStatisticalMix()) {
    // Peak fanout sub-clusters need per-instance sequential/combinational
    // classification to build cluster-safe eligibility from (they piggyback
    // on formNetsAcyclic's pools) — not available on the legacy weighted-mix
    // path, so reject rather than silently ignoring the peak_avg_fanout the
    // caller asked for.
    if (spec.peak_avg_fanout.has_value()) {
      if (logger) {
        logger->warn(utl::UKN, kMsgPeakConfig,
                     "peak_avg_fanout requires the statistical mix to be "
                     "engaged (set sequential_ratio, target_avg_fanout, "
                     "combinational_pin_distribution, or tech_lef_path); "
                     "peak clustering is not supported in the legacy "
                     "weighted-mix path");
      }
      return false;
    }
    // Stage E1 boundary buffer/FF cells reuse the statistical mix's already-
    // resolved representative masters (comb bucket + seq class), unavailable
    // on the legacy path.
    if (spec.rent_k.has_value() || spec.rent_p.has_value()) {
      if (logger) {
        logger->warn(utl::UKN, kMsgRentConfig,
                     "rent_k/rent_p require the statistical mix to be "
                     "engaged (set sequential_ratio, target_avg_fanout, "
                     "combinational_pin_distribution, or tech_lef_path); "
                     "Stage E1 is not supported in the legacy weighted-mix "
                     "path");
      }
      return false;
    }
    // Legacy weighted mix: generateSynthetic checks masters non-emptiness.
    return true;
  }

  const bool has_dist = spec.combinational_pin_distribution.has_value();
  const bool has_target = spec.target_avg_fanout.has_value();
  if (has_dist && has_target) {
    if (logger) {
      logger->warn(utl::UKN, kMsgBothModes,
                   "both combinational_pin_distribution and "
                   "target_avg_fanout are set; exactly one is allowed");
    }
    return false;
  }
  if (!has_dist && !has_target) {
    if (logger) {
      logger->warn(utl::UKN, kMsgNoMode,
                   "statistical mix engaged but neither "
                   "combinational_pin_distribution nor target_avg_fanout "
                   "is set");
    }
    return false;
  }

  const double seq_ratio = spec.sequential_ratio.value_or(0.0);
  if (seq_ratio < 0.0 || seq_ratio > 1.0) {
    if (logger) {
      logger->warn(utl::UKN, kMsgSeqRatio,
                   "sequential_ratio must be in [0, 1], got {}", seq_ratio);
    }
    return false;
  }
  // Stage D bootstrap-source requirement: acyclic net formation needs
  // sequential Q outputs as the only always-valid signal source — no primary
  // input ports exist until Stage E (which relaxes this to "sequential_ratio
  // > 0 OR primary_input_count > 0"). An unset ratio counts as 0.
  if (seq_ratio <= 0.0) {
    if (logger) {
      logger->warn(utl::UKN, kMsgSeqBootstrap,
                   "sequential_ratio must be > 0 (got {}): sequential Q "
                   "outputs are the only bootstrap signal source for the "
                   "combinational DAG until primary input ports arrive in "
                   "Stage E",
                   seq_ratio);
    }
    return false;
  }

  if (has_dist) {
    double sum = 0.0;
    for (double v : *spec.combinational_pin_distribution) {
      if (v < 0.0) {
        if (logger) {
          logger->warn(utl::UKN, kMsgDistSum,
                       "combinational_pin_distribution has a negative "
                       "bucket weight");
        }
        return false;
      }
      sum += v;
    }
    if (std::abs(sum - 100.0) > 1e-6) {
      if (logger) {
        logger->warn(utl::UKN, kMsgDistSum,
                     "combinational_pin_distribution must sum to 100, got {}",
                     sum);
      }
      return false;
    }
  } else {
    // Mode B synthetic-anchor range check. target_avg_fanout is a FANOUT (load
    // pins per net, i.e. signal pins excluding the single driver/output pin), so
    // it maps onto the pin-count anchors as fanout + 1. Equivalently, the valid
    // fanout range is the anchor range shifted down by one — lower bound
    // exclusive, upper bound INCLUSIVE (at the top the max-entropy solve
    // degenerates to 100% top bucket, which hits the mean exactly). In LEF
    // mode the range is re-checked against measured anchors during plan
    // building.
    const double t_pins = *spec.target_avg_fanout + 1.0;
    const double lo = kSyntheticBucketAnchors.front();
    const double hi = kSyntheticBucketAnchors.back();
    if (!spec.tech_lef_path.has_value() && (t_pins <= lo || t_pins > hi)) {
      if (logger) {
        logger->warn(utl::UKN, kMsgTargetRange,
                     "target_avg_fanout must be in ({}, {}], got {}",
                     lo - 1.0, hi - 1.0, *spec.target_avg_fanout);
      }
      return false;
    }
  }

  // Peak fanout sub-clusters (optional, layered on the statistical mix — see
  // netlistgen.h). Ignored entirely (rules 2/3 below not checked) when
  // peak_avg_fanout itself is unset, per the spec's "ignore the other two
  // fields even if present" rule.
  if (spec.peak_avg_fanout.has_value()) {
    // "avg_fanout" has no literal field in this codebase (background fanout
    // is a [min_fanout, max_fanout] range, not a single scalar); the closest
    // analog is the midpoint of that range, which is what peak_avg_fanout
    // must exceed to be a genuine hot spot.
    const double background_avg =
        (spec.min_fanout + spec.max_fanout) / 2.0;
    if (*spec.peak_avg_fanout <= background_avg) {
      if (logger) {
        logger->warn(utl::UKN, kMsgPeakConfig,
                     "peak_avg_fanout must be greater than the background "
                     "average fanout {} (= (min_fanout + max_fanout) / 2), "
                     "got {}",
                     background_avg, *spec.peak_avg_fanout);
      }
      return false;
    }
    if (spec.peak_cluster_pct.has_value()
        && (*spec.peak_cluster_pct <= 0.0 || *spec.peak_cluster_pct >= 1.0)) {
      if (logger) {
        logger->warn(utl::UKN, kMsgPeakConfig,
                     "peak_cluster_pct must be in (0, 1), got {}",
                     *spec.peak_cluster_pct);
      }
      return false;
    }
    if (spec.num_peak_clusters.has_value() && *spec.num_peak_clusters < 1) {
      if (logger) {
        logger->warn(utl::UKN, kMsgPeakConfig,
                     "num_peak_clusters must be >= 1, got {}",
                     *spec.num_peak_clusters);
      }
      return false;
    }
  }

  // Stage E1: primary I/O generation via Rent's rule (optional). Engaged
  // only when BOTH rent_k and rent_p are set; exactly one alone is an error.
  const bool has_rent_k = spec.rent_k.has_value();
  const bool has_rent_p = spec.rent_p.has_value();
  if (has_rent_k != has_rent_p) {
    if (logger) {
      logger->warn(utl::UKN, kMsgRentConfig,
                   "rent_k and rent_p must both be set to engage Stage E1 "
                   "(got rent_k={}, rent_p={})",
                   has_rent_k ? "set" : "unset", has_rent_p ? "set" : "unset");
    }
    return false;
  }
  if (has_rent_k && has_rent_p) {
    if (*spec.rent_k <= 0.0) {
      if (logger) {
        logger->warn(utl::UKN, kMsgRentConfig, "rent_k must be > 0, got {}",
                     *spec.rent_k);
      }
      return false;
    }
    // (0, 1.0]: fine as given. (1.0, 1.2]: warn only — accepted, clamped to
    // 1.0 wherever rent_p feeds a computation (T_target). > 1.2: hard error.
    if (*spec.rent_p <= 0.0) {
      if (logger) {
        logger->warn(utl::UKN, kMsgRentConfig, "rent_p must be > 0, got {}",
                     *spec.rent_p);
      }
      return false;
    }
    if (*spec.rent_p > 1.2) {
      if (logger) {
        logger->warn(utl::UKN, kMsgRentConfig,
                     "rent_p must be in (0, 1.2], got {}", *spec.rent_p);
      }
      return false;
    }
    if (*spec.rent_p > 1.0 && logger) {
      logger->warn(utl::UKN, kMsgRentConfig,
                   "rent_p {} is in the degenerate-but-tolerable range "
                   "(1.0, 1.2]; clamping to 1.0 for the T_target computation",
                   *spec.rent_p);
    }
    if (spec.io_input_ratio.has_value()
        && (*spec.io_input_ratio <= 0.0 || *spec.io_input_ratio >= 1.0)) {
      if (logger) {
        logger->warn(utl::UKN, kMsgRentConfig,
                     "io_input_ratio must be in (0, 1), got {}",
                     *spec.io_input_ratio);
      }
      return false;
    }
    if (spec.io_pin_type_distribution.has_value()) {
      const IoPinTypeDistribution& d = *spec.io_pin_type_distribution;
      for (double v : {d.combinational, d.buffered, d.registered}) {
        if (v < 0.0 || v > 1.0) {
          if (logger) {
            logger->warn(utl::UKN, kMsgRentConfig,
                         "io_pin_type_distribution fractions must each be "
                         "in [0, 1], got {}",
                         v);
          }
          return false;
        }
      }
      const double sum = d.combinational + d.buffered + d.registered;
      if (std::abs(sum - 1.0) > 0.01) {
        if (logger) {
          logger->warn(utl::UKN, kMsgRentConfig,
                       "io_pin_type_distribution fractions must sum to 1.0 "
                       "+/- 0.01, got {}",
                       sum);
        }
        return false;
      }
    }
  }
  return true;
}

NetlistBuilder::NetlistBuilder(const std::string& design_name,
                               utl::Logger* logger)
    : design_name_(design_name)
{
  if (logger != nullptr) {
    logger_ = logger;  // shared; not owned
  } else {
    logger_ = new utl::Logger();
    owns_logger_ = true;
  }
  db_ = odb::dbDatabase::create();
  db_->setLogger(logger_);
}

NetlistBuilder::~NetlistBuilder()
{
  odb::dbDatabase::destroy(db_);
  if (owns_logger_) {
    delete logger_;
  }
}

void NetlistBuilder::ensureSyntheticTech()
{
  if (tech_ready_) {
    return;
  }
  // Set database units before creating the block: the block inherits
  // dbu-per-micron from the tech at creation, and DefOut divides def-units by
  // dbu-per-micron (0 on a fresh db) when writing UNITS/DIEAREA. 2000 DBU/um
  // matches the nominal site pitch estimateDieArea() uses in synthetic mode.
  db_->setDbuPerMicron(2000);
  odb::dbTech* tech = odb::dbTech::create(db_, "tech");
  odb::dbLib::create(db_, "lib", tech);
  odb::dbChip* chip = odb::dbChip::create(db_, tech);
  block_ = odb::dbBlock::create(chip, design_name_.c_str());
  tech_ready_ = true;
}

bool NetlistBuilder::loadLef(const std::string& tech_lef_path,
                             const std::vector<std::string>& cell_lef_paths)
{
  if (tech_ready_) {
    logger_->warn(utl::UKN, kMsgLefLoad,
                  "loadLef called on an already-initialised builder");
    return false;
  }
  // Check each LEF exists BEFORE handing it to lefin: on a missing/unreadable
  // file OpenROAD's createTechAndLib calls logger->error(), which THROWS
  // (see support/logging.h) — and with utl's swig error path linked in that
  // unwinds into a crash. Prechecking keeps a missing path an ordinary
  // return-false failure at its source rather than a thrown exception.
  if (!std::filesystem::exists(tech_lef_path)) {
    logger_->warn(utl::UKN, kMsgLefLoad, "tech LEF not found: {}",
                  tech_lef_path);
    return false;
  }
  for (const std::string& cell_lef : cell_lef_paths) {
    if (!std::filesystem::exists(cell_lef)) {
      logger_->warn(utl::UKN, kMsgLefLoad, "cell LEF not found: {}", cell_lef);
      return false;
    }
  }
  // Wrap the reader calls in a try/catch AT this boundary. A present-but-
  // malformed LEF makes lefin call logger->error(), which throws; catching it
  // here — close to the throw, where the stack unwinds cleanly — keeps it an
  // ordinary return-false failure. (A catch further up, e.g. in the CLI's
  // main(), does NOT contain an odb throw: unwinding it that far fails and
  // crashes. The handler must sit near the reader call.)
  try {
    // createTechAndLib is a 3-arg call (tech_name, lib_name, lef_file); the
    // tech LEF doubles as the source of any macros it happens to define.
    odb::lefin reader(db_, logger_, /*ignore_non_routing_layers=*/false);
    odb::dbLib* tech_lib =
        reader.createTechAndLib("tech", "tech_lib", tech_lef_path.c_str());
    if (tech_lib == nullptr) {
      logger_->warn(utl::UKN, kMsgLefLoad, "failed to load tech LEF {}",
                    tech_lef_path);
      return false;
    }
    odb::dbTech* tech = tech_lib->getTech();
    int idx = 0;
    for (const std::string& cell_lef : cell_lef_paths) {
      odb::dbLib* lib = reader.createLib(
          tech, ("cells" + std::to_string(idx++)).c_str(), cell_lef.c_str());
      if (lib == nullptr) {
        logger_->warn(utl::UKN, kMsgLefLoad, "failed to load cell LEF {}",
                      cell_lef);
        return false;
      }
    }
    // Masters loaded by lefin arrive already frozen (lefinReader freezes each
    // MACRO at END; verified against the pinned SHA), so no setFrozen() here —
    // unlike the synthetic path where NetlistBuilder freezes explicitly.
    odb::dbChip* chip = odb::dbChip::create(db_, tech, design_name_);
    block_ = odb::dbBlock::create(chip, design_name_.c_str());
    tech_ready_ = true;
    return true;
  } catch (const std::exception& e) {
    logger_->warn(utl::UKN, kMsgLefLoad, "LEF load failed for {}: {}",
                  tech_lef_path, e.what());
    return false;
  }
}

std::vector<odb::dbMaster*> NetlistBuilder::masters() const
{
  std::vector<odb::dbMaster*> out;
  if (db_ == nullptr) {
    return out;
  }
  for (odb::dbLib* lib : db_->getLibs()) {
    for (odb::dbMaster* m : lib->getMasters()) {
      out.push_back(m);
    }
  }
  return out;
}

odb::dbMaster* NetlistBuilder::makeMaster(const std::string& name,
                                          const int num_inputs,
                                          const int num_outputs,
                                          const bool clocked)
{
  ensureSyntheticTech();
  odb::dbLib* lib = *db_->getLibs().begin();
  odb::dbMaster* master = odb::dbMaster::create(lib, name.c_str());
  if (master == nullptr) {
    return nullptr;
  }
  master->setType(odb::dbMasterType::CORE);
  for (int i = 0; i < num_inputs; ++i) {
    // A clocked master's first input is its clock pin (CLOCK sig type), so it
    // is recognised as sequential; the rest stay SIGNAL data inputs.
    const odb::dbSigType sig = (clocked && i == 0) ? odb::dbSigType::CLOCK
                                                   : odb::dbSigType::SIGNAL;
    odb::dbMTerm::create(master,
                         ("i" + std::to_string(i)).c_str(),
                         odb::dbIoType::INPUT,
                         sig);
  }
  for (int o = 0; o < num_outputs; ++o) {
    odb::dbMTerm::create(master,
                         ("o" + std::to_string(o)).c_str(),
                         odb::dbIoType::OUTPUT,
                         odb::dbSigType::SIGNAL);
  }
  // OpenDB requires a frozen master before dbInst::create.
  master->setFrozen();
  return master;
}

odb::dbInst* NetlistBuilder::makeInst(odb::dbMaster* master,
                                      const std::string& name)
{
  ensureSyntheticTech();
  return odb::dbInst::create(block_, master, name.c_str());
}

odb::dbNet* NetlistBuilder::makeNet(const std::string& name)
{
  ensureSyntheticTech();
  return odb::dbNet::create(block_, name.c_str());
}

bool NetlistBuilder::connect(odb::dbInst* inst,
                             const std::string& pin,
                             odb::dbNet* net)
{
  odb::dbITerm* iterm = inst->findITerm(pin.c_str());
  if (iterm == nullptr) {
    return false;
  }
  iterm->connect(net);
  return true;
}

NetlistBuilder::DieArea NetlistBuilder::estimateDieArea(const int num_insts,
                                                        const double utilization)
{
  ensureSyntheticTech();

  // Site pitch: pull the first site from a loaded library (LEF mode), else
  // fall back to a nominal 0.1um x 1.0um pitch at a 2000 DBU/um scale.
  int site_w = 200;
  int site_h = 2000;
  for (odb::dbLib* lib : db_->getLibs()) {
    auto sites = lib->getSites();
    if (sites.begin() != sites.end()) {
      odb::dbSite* site = *sites.begin();
      site_w = site->getWidth();
      site_h = site->getHeight();
      break;
    }
  }

  const double util = (utilization > 0.0 && utilization <= 1.0) ? utilization
                                                                : 0.7;
  const double cell_area = static_cast<double>(site_w) * site_h;
  const double total_area =
      static_cast<double>(num_insts) * cell_area / util;
  const double side = std::sqrt(std::max(total_area, cell_area));

  // Snap to whole rows/columns of the site pitch.
  const int rows = std::max(1, static_cast<int>(std::ceil(side / site_h)));
  const int cols = std::max(
      1, static_cast<int>(std::ceil(side / site_w)));
  DieArea area;
  area.ux = cols * site_w;
  area.uy = rows * site_h;
  block_->setDieArea(odb::Rect(area.lx, area.ly, area.ux, area.uy));
  return area;
}

namespace {

// The generation plan resolved from the spec: which masters populate each
// combinational bucket and the sequential class, the (normalised) bucket
// probabilities, and the sequential ratio.
struct GenPlan
{
  std::array<std::vector<odb::dbMaster*>, kNumCombBuckets> comb;
  std::vector<odb::dbMaster*> seq;
  std::array<double, kNumCombBuckets> prob{};
  std::array<double, kNumCombBuckets> anchors = kSyntheticBucketAnchors;
  double seq_ratio = 0.0;
};

// Populate the plan's buckets/anchors from a loaded LEF library. Clock-gating
// cells and latches are dropped entirely; multi-output combinational masters
// and cells with no valid bucket are excluded (all logged).
void populateLefBuckets(NetlistBuilder& builder, GenPlan& plan,
                        utl::Logger* logger)
{
  std::array<double, kNumCombBuckets> sum{};
  std::array<int, kNumCombBuckets> cnt{};
  for (odb::dbMaster* m : builder.masters()) {
    // Clock-gating cells carry a clock pin but are not used as sequential (or
    // any) masters. Checked before isSequentialMaster, which they also satisfy.
    if (isClockGateMaster(m)) {
      if (logger) {
        logger->warn(utl::UKN, kMsgExcludeMaster,
                     "excluding clock-gating master {}: clock gates are not used",
                     m->getName());
      }
      continue;
    }
    if (isSequentialMaster(m)) {
      plan.seq.push_back(m);
      continue;
    }
    // Level-sensitive latches are used as neither sequential nor combinational.
    if (isLatchMaster(m)) {
      if (logger) {
        logger->warn(utl::UKN, kMsgExcludeMaster,
                     "excluding latch master {}: latches are not used",
                     m->getName());
      }
      continue;
    }
    const int outs = signalOutputCount(m);
    if (outs != 1) {
      if (logger) {
        logger->warn(utl::UKN, kMsgExcludeMaster,
                     "excluding combinational master {}: expected exactly "
                     "one output, found {}",
                     m->getName(), outs);
      }
      continue;
    }
    const int pins = signalPinCount(m);
    const int b = bucketIndex(pins);
    if (b < 0) {
      if (logger) {
        logger->warn(utl::UKN, kMsgExcludeMaster,
                     "excluding master {}: {} signal pins is below the "
                     "smallest bucket",
                     m->getName(), pins);
      }
      continue;
    }
    plan.comb[b].push_back(m);
    sum[b] += pins;
    ++cnt[b];
  }
  for (int i = 0; i < kNumCombBuckets; ++i) {
    plan.anchors[i] =
        cnt[i] > 0 ? sum[i] / cnt[i] : kSyntheticBucketAnchors[i];
  }
}

// Resolve bucket probabilities (Mode A direct, Mode B max-entropy). Returns
// false only if a Mode-B target is outside the (LEF-measured) anchor range.
bool resolveProbabilities(const SyntheticNetlistSpec& spec, GenPlan& plan,
                          utl::Logger* logger)
{
  if (spec.combinational_pin_distribution.has_value()) {
    for (int i = 0; i < kNumCombBuckets; ++i) {
      plan.prob[i] = (*spec.combinational_pin_distribution)[i] / 100.0;
    }
    return true;
  }
  // target_avg_fanout is a fanout (load pins, driver excluded); the anchors are
  // signal-pin counts (driver included), so the equivalent pin-count target is
  // fanout + 1.
  const double target_fanout = *spec.target_avg_fanout;
  const double target_pins = target_fanout + 1.0;
  const double lo = *std::min_element(plan.anchors.begin(), plan.anchors.end());
  const double hi = *std::max_element(plan.anchors.begin(), plan.anchors.end());
  // Lower bound exclusive, upper bound inclusive (degenerate all-top-bucket
  // distribution at exactly the top anchor) — same rule as validateSpecConfig.
  if (target_pins <= lo || target_pins > hi) {
    if (logger) {
      logger->warn(utl::UKN, kMsgTargetRange,
                   "target_avg_fanout {} is outside the valid fanout "
                   "range ({}, {}]",
                   target_fanout, lo - 1.0, hi - 1.0);
    }
    return false;
  }
  plan.prob = maxEntropyDistribution(plan.anchors, target_pins);
  if (logger) {
    logger->info(utl::UKN, kMsgDerivedDist,
                 "max-entropy bucket distribution for fanout target {}: "
                 "[{:.3f}, {:.3f}, {:.3f}, {:.3f}, {:.3f}]",
                 target_fanout, plan.prob[0], plan.prob[1], plan.prob[2],
                 plan.prob[3], plan.prob[4]);
  }
  return true;
}

// Build (and validate) the generation plan. Returns false on any structural
// error: an empty requested bucket or sequential class in LEF mode, a
// Mode-B target outside the anchor range, or no LEF masters at all.
bool buildPlan(NetlistBuilder& builder, const SyntheticNetlistSpec& spec,
               GenPlan& plan)
{
  utl::Logger* logger = builder.logger();
  plan.seq_ratio = spec.sequential_ratio.value_or(0.0);

  const bool lef_mode = spec.tech_lef_path.has_value();
  if (lef_mode) {
    if (builder.masters().empty()) {
      logger->warn(utl::UKN, kMsgNoLefMasters,
                   "LEF mode requested but no masters were loaded");
      return false;
    }
    populateLefBuckets(builder, plan, logger);
  } else {
    plan.anchors = kSyntheticBucketAnchors;
  }

  if (!resolveProbabilities(spec, plan, logger)) {
    return false;
  }

  if (lef_mode) {
    for (int i = 0; i < kNumCombBuckets; ++i) {
      if (plan.prob[i] > 0.0 && plan.comb[i].empty()) {
        logger->warn(utl::UKN, kMsgEmptyBucket,
                     "combinational bucket {} (pin-count {}) has weight {} "
                     "but no matching masters in the loaded library",
                     i, i + 2, plan.prob[i]);
        return false;
      }
    }
    if (plan.seq_ratio > 0.0 && plan.seq.empty()) {
      logger->warn(utl::UKN, kMsgEmptySeq,
                   "sequential_ratio is {} but no sequential (CLOCK-pin) "
                   "masters were found in the loaded library",
                   plan.seq_ratio);
      return false;
    }
  } else {
    // Synthetic mode: materialise one representative master per requested
    // bucket and one sequential representative (D, CK, Q = 3 signal pins).
    for (int i = 0; i < kNumCombBuckets; ++i) {
      if (plan.prob[i] > 0.0) {
        const int pin_count = static_cast<int>(kSyntheticBucketAnchors[i]);
        odb::dbMaster* m = builder.makeMaster(
            "COMB_" + std::to_string(pin_count), pin_count - 1, 1);
        plan.comb[i].push_back(m);
      }
    }
    if (plan.seq_ratio > 0.0) {
      plan.seq.push_back(builder.makeMaster("SEQ", 2, 1, /*clocked=*/true));
    }
  }
  return true;
}

// Form nets from two shuffled pin pools: one driver (OUTPUT) plus `fanout`
// sinks (INPUT/INOUT) per net — fanout is the load count, driver excluded —
// popping each iterm at most once. Power/ground pins are never eligible.
// LEGACY (Stage A) PATH ONLY since Stage D: unconstrained pairing can create
// combinational cycles, so the statistical path uses formNetsAcyclic below.
// (Fanout was redefined from pins-per-net to loads, so net shapes shift by
// one sink vs. the original Stage A output.)
int formNets(NetlistBuilder& builder,
             const std::vector<odb::dbInst*>& insts,
             const SyntheticNetlistSpec& spec, std::mt19937& rng)
{
  std::vector<odb::dbITerm*> drivers;
  std::vector<odb::dbITerm*> sinks;
  for (odb::dbInst* inst : insts) {
    for (odb::dbITerm* iterm : inst->getITerms()) {
      const odb::dbSigType st = iterm->getSigType();
      if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
        continue;
      }
      switch (iterm->getIoType().getValue()) {
        case odb::dbIoType::OUTPUT:
          drivers.push_back(iterm);
          break;
        case odb::dbIoType::INPUT:
        case odb::dbIoType::INOUT:
          sinks.push_back(iterm);
          break;
        case odb::dbIoType::FEEDTHRU:
          break;
      }
    }
  }
  std::shuffle(drivers.begin(), drivers.end(), rng);
  std::shuffle(sinks.begin(), sinks.end(), rng);

  std::uniform_int_distribution<int> pick_fanout(spec.min_fanout,
                                                 spec.max_fanout);
  utl::Logger* logger = builder.logger();
  int nets_made = 0;
  while (!drivers.empty() && !sinks.empty()
         && (spec.num_nets < 0 || nets_made < spec.num_nets)) {
    odb::dbNet* net = builder.makeNet("n" + std::to_string(nets_made));
    drivers.back()->connect(net);
    drivers.pop_back();

    // fanout is the number of load (sink) pins, driver excluded, so the net
    // gets one driver plus `fanout` sinks (total fanout+1 pins).
    const int num_sinks = pick_fanout(rng);
    int connected = 0;
    for (int s = 0; s < num_sinks && !sinks.empty(); ++s) {
      sinks.back()->connect(net);
      sinks.pop_back();
      ++connected;
    }
    if (logger != nullptr && nets_made < kTraceCap) {
      debugPrint(logger, utl::UKN, kGroup, kVerbosityTrace,
                 "  net n{}: 1 driver + {} sinks{}", nets_made, connected,
                 nets_made + 1 == kTraceCap ? " (per-net trace capped)" : "");
    }
    ++nets_made;
    if (logger != nullptr && nets_made % 100000 == 0) {
      debugPrint(logger, utl::UKN, kGroup, kVerbosityHeartbeat,
                 "formNets: {} nets formed, {} drivers / {} sinks left",
                 nets_made, static_cast<int>(drivers.size()),
                 static_cast<int>(sinks.size()));
    }
  }
  return nets_made;
}

// Stage D: ordered, combinational-loop-free net formation for the statistical
// path. Instance creation order (the insts vector, u0..u{n-1}) doubles as a
// topological (DAG) order over the combinational cells:
//
//   - A combinational output at index i may only drive (a) inputs of
//     sequential instances — any index — or (b) inputs of combinational
//     instances at indices > i. Every comb->comb edge therefore goes strictly
//     forward in index order, so no combinational cycle can be constructed.
//   - A sequential (Q) output is an always-valid driver and may feed any
//     unused input, earlier or later — a register output is the loop-breaker.
//   - Sequential inputs (D/CK) accept any driver; feedback through a register
//     is a legitimate sequential loop, not a combinational one.
//
// Only the receiver-ELIGIBILITY ruleset differs from Stage B's formNets: the
// receiver count per net is still drawn uniformly from
// [min_fanout, max_fanout] (keep these concerns separate — Stage E adds
// primary ports as one more driver/receiver class without touching the count
// machinery). Documented bootstrap/tail behavior when the eligible pool runs
// thin (typically late in the order, where only sequential inputs remain
// eligible for a combinational driver): the net is formed with fewer
// receivers than min_fanout (>= 1, counted + debug-logged), and a driver with
// ZERO eligible receivers forms no net at all (skipped, counted +
// debug-logged) — deterministic, never a sinkless net, never a stall.
//
// Peak fanout sub-clusters (optional; see netlistgen.h): when
// spec.peak_avg_fanout is set, a per-instance cluster id is assigned once up
// front (assignPeakClusters) and a net driven from inside a cluster draws its
// receiver count from a distribution centred on peak_avg_fanout (same
// uniform shape/width as the background, just re-centred) and prefers
// intra-cluster receivers for a p_intra fraction of its sink slots — via
// rejection sampling capped at kPeakClusterPickAttempts, so a scarce/depleted
// cluster falls back to any eligible receiver rather than stalling or
// duplicating a sink. Cluster preference only ever narrows a pick WITHIN the
// eligible pools above; it can never make an ineligible iterm eligible, so
// the acyclicity guarantee is unaffected by clustering.
int formNetsAcyclic(NetlistBuilder& builder,
                    const std::vector<odb::dbInst*>& insts,
                    const SyntheticNetlistSpec& spec, std::mt19937& rng,
                    std::vector<int>* out_cluster_id = nullptr)
{
  const int n = static_cast<int>(insts.size());
  utl::Logger* logger = builder.logger();

  // Receiver pools. Every entry is an unused non-power/ground INPUT/INOUT
  // iterm. seq_pool: sequential-instance inputs, eligible for every driver.
  // comb_active: combinational-instance inputs whose owner has not been
  // processed yet, eligible for every driver. comb_retired: combinational
  // inputs whose owner HAS been processed — eligible for sequential drivers
  // only (a combinational driver may not feed an earlier-or-own instance).
  std::vector<odb::dbITerm*> seq_pool;
  std::vector<odb::dbITerm*> comb_active;
  std::vector<odb::dbITerm*> comb_retired;
  std::unordered_map<odb::dbITerm*, size_t> active_pos;  // iterm -> index
  std::vector<std::vector<odb::dbITerm*>> comb_inputs_of(n);
  std::vector<char> inst_is_seq(n, 0);

  // Peak fanout sub-clusters: assigned once, up front, from the same rng
  // used by the rest of generation. cluster_id stays empty when the feature
  // is not engaged (peak_enabled false) — every lookup below then reports
  // background (-1), so the rest of this function is unchanged in that case.
  const bool peak_enabled = spec.peak_avg_fanout.has_value();
  std::vector<int> cluster_id;
  if (peak_enabled) {
    cluster_id = assignPeakClusters(n, spec.peak_cluster_pct.value_or(0.10),
                                    spec.num_peak_clusters.value_or(1), rng);
    if (out_cluster_id != nullptr) {
      *out_cluster_id = cluster_id;
    }
  }
  // iterm -> cluster id, populated only for iterms owned by a clustered
  // instance (cheap: bounded by peak_cluster_pct * total pins). A lookup
  // miss means background, same as an explicit -1.
  std::unordered_map<odb::dbITerm*, int> iterm_cluster;
  auto clusterOf = [&](odb::dbITerm* t) -> int {
    if (!peak_enabled) {
      return -1;
    }
    const auto it = iterm_cluster.find(t);
    return it == iterm_cluster.end() ? -1 : it->second;
  };

  for (int i = 0; i < n; ++i) {
    inst_is_seq[i] = isSequentialMaster(insts[i]->getMaster()) ? 1 : 0;
    for (odb::dbITerm* iterm : insts[i]->getITerms()) {
      const odb::dbSigType st = iterm->getSigType();
      if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
        continue;
      }
      const auto io = iterm->getIoType().getValue();
      if (io != odb::dbIoType::INPUT && io != odb::dbIoType::INOUT) {
        continue;
      }
      if (peak_enabled && cluster_id[i] >= 0) {
        iterm_cluster[iterm] = cluster_id[i];
      }
      if (inst_is_seq[i]) {
        seq_pool.push_back(iterm);
      } else {
        active_pos[iterm] = comb_active.size();
        comb_active.push_back(iterm);
        comb_inputs_of[i].push_back(iterm);
      }
    }
  }

  // Swap-remove from comb_active, keeping active_pos consistent.
  auto takeFromActive = [&](size_t idx) {
    odb::dbITerm* taken = comb_active[idx];
    active_pos.erase(taken);
    comb_active[idx] = comb_active.back();
    comb_active.pop_back();
    if (idx < comb_active.size()) {
      active_pos[comb_active[idx]] = idx;
    }
    return taken;
  };

  std::uniform_int_distribution<int> pick_fanout(spec.min_fanout,
                                                 spec.max_fanout);
  // Peak-cluster fanout distribution: same uniform width as the background
  // range, re-centred on peak_avg_fanout (brief: "same distribution shape ...
  // applied to the new centre value" — this codebase's shape is a uniform
  // range, so re-centre that range rather than swapping in a new family).
  const int fanout_width = spec.max_fanout - spec.min_fanout;
  int peak_min_fanout = 1;
  int peak_max_fanout = 1;
  if (peak_enabled) {
    peak_min_fanout = std::max(
        1, static_cast<int>(std::lround(*spec.peak_avg_fanout
                                        - fanout_width / 2.0)));
    peak_max_fanout = peak_min_fanout + fanout_width;
  }
  std::uniform_int_distribution<int> pick_peak_fanout(peak_min_fanout,
                                                       peak_max_fanout);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  int nets_made = 0;
  int loosened = 0;
  int skipped = 0;
  int cluster_nets_made = 0;
  for (int i = 0;
       i < n && (spec.num_nets < 0 || nets_made < spec.num_nets); ++i) {
    // Retire this instance's still-unused inputs BEFORE forming its net(s):
    // from here on they are reachable only from sequential (Q) drivers, and
    // a combinational driver can never feed its own instance.
    for (odb::dbITerm* iterm : comb_inputs_of[i]) {
      const auto pos = active_pos.find(iterm);
      if (pos != active_pos.end()) {
        takeFromActive(pos->second);
        comb_retired.push_back(iterm);
      }
    }

    const bool seq_driver = inst_is_seq[i] != 0;
    const int driver_cluster = peak_enabled ? cluster_id[i] : -1;
    const bool cluster_net = driver_cluster >= 0;
    for (odb::dbITerm* out : insts[i]->getITerms()) {
      if (spec.num_nets >= 0 && nets_made >= spec.num_nets) {
        break;
      }
      const odb::dbSigType st = out->getSigType();
      if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND
          || out->getIoType() != odb::dbIoType::OUTPUT) {
        continue;
      }
      size_t eligible = seq_pool.size() + comb_active.size()
                        + (seq_driver ? comb_retired.size() : 0);
      if (eligible == 0) {
        ++skipped;
        if (logger != nullptr && skipped <= kTraceCap) {
          debugPrint(logger, utl::UKN, kGroup, kVerbosityTrace,
                     "  driver {} skipped: no eligible receivers{}",
                     out->getName(),
                     skipped == kTraceCap ? " (skip trace capped)" : "");
        }
        continue;
      }
      const int want = cluster_net ? pick_peak_fanout(rng) : pick_fanout(rng);
      const int k = static_cast<int>(
          std::min<size_t>(static_cast<size_t>(want), eligible));

      odb::dbNet* net = builder.makeNet("n" + std::to_string(nets_made));
      out->connect(net);
      // Peek/take helpers over the flat union of eligible pools (order:
      // seq_pool, comb_active, comb_retired-if-seq-driver), matching the
      // background draw exactly; used both for the plain draw and for
      // rejection-sampling a cluster-preferred draw without mutating state
      // on a rejected attempt.
      auto peekEligible = [&](size_t r) -> odb::dbITerm* {
        if (r < seq_pool.size()) {
          return seq_pool[r];
        }
        r -= seq_pool.size();
        if (r < comb_active.size()) {
          return comb_active[r];
        }
        r -= comb_active.size();
        return comb_retired[r];
      };
      auto takeEligible = [&](size_t r) -> odb::dbITerm* {
        if (r < seq_pool.size()) {
          odb::dbITerm* t = seq_pool[r];
          seq_pool[r] = seq_pool.back();
          seq_pool.pop_back();
          return t;
        }
        r -= seq_pool.size();
        if (r < comb_active.size()) {
          return takeFromActive(r);
        }
        r -= comb_active.size();
        odb::dbITerm* t = comb_retired[r];
        comb_retired[r] = comb_retired.back();
        comb_retired.pop_back();
        return t;
      };

      for (int s = 0; s < k; ++s) {
        std::uniform_int_distribution<size_t> pick(0, eligible - 1);
        odb::dbITerm* sink = nullptr;
        if (cluster_net && unit(rng) < kPeakIntraClusterProb) {
          for (int attempt = 0;
               attempt < kPeakClusterPickAttempts && sink == nullptr;
               ++attempt) {
            const size_t r = pick(rng);
            if (clusterOf(peekEligible(r)) == driver_cluster) {
              sink = takeEligible(r);
            }
          }
        }
        if (sink == nullptr) {
          sink = takeEligible(pick(rng));
        }
        sink->connect(net);
        --eligible;
      }
      if (cluster_net) {
        ++cluster_nets_made;
      }
      if (k < spec.min_fanout) {
        ++loosened;
        if (logger != nullptr && loosened <= kTraceCap) {
          debugPrint(logger, utl::UKN, kGroup, kVerbosityTrace,
                     "  net n{} loosened: {} receivers (min_fanout {}), "
                     "eligible pool thin{}",
                     nets_made, k, spec.min_fanout,
                     loosened == kTraceCap ? " (loosen trace capped)" : "");
        }
      } else if (logger != nullptr && nets_made < kTraceCap) {
        debugPrint(logger, utl::UKN, kGroup, kVerbosityTrace,
                   "  net n{}: 1 driver + {} sinks{}", nets_made, k,
                   nets_made + 1 == kTraceCap ? " (per-net trace capped)" : "");
      }
      ++nets_made;
      if (logger != nullptr && nets_made % 100000 == 0) {
        debugPrint(logger, utl::UKN, kGroup, kVerbosityHeartbeat,
                   "formNetsAcyclic: {} nets formed, pools: {} seq / {} "
                   "active / {} retired",
                   nets_made, static_cast<int>(seq_pool.size()),
                   static_cast<int>(comb_active.size()),
                   static_cast<int>(comb_retired.size()));
      }
    }
  }
  if (logger != nullptr) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "formNetsAcyclic: {} nets ({} below min_fanout at the thin "
               "tail of the order, {} drivers skipped with no eligible "
               "receiver{})",
               nets_made, loosened, skipped,
               peak_enabled
                   ? ", " + std::to_string(cluster_nets_made)
                         + " peak-cluster nets"
                   : "");
  }
  return nets_made;
}

// The three I/O pin types Stage E1 samples independently for each PI/PO net.
enum class IoPinKind
{
  kCombinational,
  kBuffered,
  kRegistered
};

// First non-power/ground OUTPUT iterm on `inst`, or nullptr. Used to find a
// freshly-created boundary buffer/FF instance's driver pin (never fails in
// practice — every comb/seq representative master Stage E1 reuses has at
// least one signal output, per Stage B's own invariants).
odb::dbITerm* firstOutputIterm(odb::dbInst* inst)
{
  for (odb::dbITerm* it : inst->getITerms()) {
    const odb::dbSigType st = it->getSigType();
    if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
      continue;
    }
    if (it->getIoType() == odb::dbIoType::OUTPUT) {
      return it;
    }
  }
  return nullptr;
}

// First non-power/ground, non-clock INPUT/INOUT iterm on `inst` — the "data"
// input a PI feeds (never the CK pin of a boundary FF). Clock recognition
// mirrors isSequentialMaster: dbSigType::CLOCK or the conventional-name
// fallback, both checked at the owning dbMTerm.
odb::dbITerm* firstDataInputIterm(odb::dbInst* inst)
{
  for (odb::dbITerm* it : inst->getITerms()) {
    const odb::dbSigType st = it->getSigType();
    if (st == odb::dbSigType::POWER || st == odb::dbSigType::GROUND) {
      continue;
    }
    const auto io = it->getIoType().getValue();
    if (io != odb::dbIoType::INPUT && io != odb::dbIoType::INOUT) {
      continue;
    }
    odb::dbMTerm* mt = it->getMTerm();
    if (mt->getSigType() == odb::dbSigType::CLOCK) {
      continue;
    }
    if (mt->getIoType() == odb::dbIoType::INPUT
        && isClockPinName(mt->getName())) {
      continue;
    }
    return it;
  }
  return nullptr;
}

// Stage E1: primary input/output port generation governed by Rent's rule
// (T = k * G^p). Runs once, after formNetsAcyclic completes, directly on the
// already-formed dbBlock — a separate pass, not a change to net formation
// itself. No-op (returns a default-constructed, `engaged = false` result)
// unless both spec.rent_k and spec.rent_p are set (validateSpecConfig has
// already enforced they're both-or-neither).
//
// PI/PO realization (see README.md's "Primary I/O generation (Stage E1)"
// section for the full rationale): every net Stage D forms already has
// exactly one committed internal driver, so a PI cannot be "added" as a
// second driver without producing a two-driver net — this codebase treats
// "exactly one driver per net" as a hard invariant (validateNetlist), and
// preserving that real-world net<->driver correspondence was an explicit
// design decision for this feature. A PI-selected net's existing driver
// ITerm is therefore DISCONNECTED (freed, left unused) and the PI's
// dbBTerm(INPUT) becomes the net's sole driver. A PO has no such conflict —
// its dbBTerm(OUTPUT) is simply one more sink/observer alongside the net's
// unchanged existing driver and sinks.
//
// Boundary buffer/FF cells reuse the statistical mix's already-resolved
// representative masters (the first non-empty combinational bucket for a
// buffer; the (guaranteed non-empty, by Stage D's bootstrap rule)
// sequential class for a boundary FF) rather than inventing new ones —
// works identically in synthetic and LEF mode with no extra master-sourcing
// logic. Reused instance/net naming continues the existing u<i>/n<i>
// sequences (planes, not names, are what distinguish a boundary cell from
// an internal one — same philosophy already used for is_boundary_reg
// vs internal sequential instances).
RentStats applyPrimaryIoStageE1(NetlistBuilder& builder,
                                const SyntheticNetlistSpec& spec,
                                std::mt19937& rng, const GenPlan& plan,
                                int next_inst_id, int next_net_id,
                                const std::vector<int>& cluster_id)
{
  RentStats stats;
  if (!spec.rent_k.has_value() || !spec.rent_p.has_value()) {
    return stats;
  }
  utl::Logger* logger = builder.logger();
  stats.engaged = true;

  // ---- Step 1: target terminal count ----
  const int G = spec.num_insts;
  stats.G = G;
  stats.rent_k_target = *spec.rent_k;
  stats.rent_p_target = std::min(*spec.rent_p, 1.0);  // clamp (1.0, 1.2]
  const int T_target = static_cast<int>(
      std::lround(stats.rent_k_target * std::pow(G, stats.rent_p_target)));
  stats.T_target = T_target;
  const double io_input_ratio = spec.io_input_ratio.value_or(0.60);

  // ---- Step 2: identify boundary candidates via random sampling ----
  std::vector<odb::dbNet*> net_list;
  for (odb::dbNet* net : builder.block()->getNets()) {
    net_list.push_back(net);
  }
  std::shuffle(net_list.begin(), net_list.end(), rng);

  int T_in = static_cast<int>(std::lround(T_target * io_input_ratio));
  int T_out = T_target - T_in;
  if (T_in + T_out > static_cast<int>(net_list.size())) {
    if (logger != nullptr) {
      logger->warn(utl::UKN, kMsgRentCap,
                   "E1: T ({}) exceeds net count ({}); capping at net count",
                   T_in + T_out, static_cast<int>(net_list.size()));
    }
    stats.capped = true;
    const int T_capped = static_cast<int>(net_list.size());
    T_in = static_cast<int>(std::lround(T_capped * io_input_ratio));
    T_out = T_capped - T_in;
  }
  const std::vector<odb::dbNet*> pi_nets(net_list.begin(),
                                         net_list.begin() + T_in);
  const std::vector<odb::dbNet*> po_nets(net_list.begin() + T_in,
                                         net_list.begin() + T_in + T_out);
  stats.T_in = T_in;
  stats.T_out = T_out;
  stats.T_actual = T_in + T_out;

  // ---- Step 3: assign pin types and insert boundary cells ----
  IoPinTypeDistribution dist =
      spec.io_pin_type_distribution.value_or(IoPinTypeDistribution{});
  const double dist_sum = dist.combinational + dist.buffered + dist.registered;
  if (dist_sum > 0.0) {  // normalise silently, per the config's own rule
    dist.combinational /= dist_sum;
    dist.buffered /= dist_sum;
    dist.registered /= dist_sum;
  }
  std::discrete_distribution<int> pick_pin_type(
      {dist.combinational, dist.buffered, dist.registered});

  // Reused representative masters for boundary cells: the first populated
  // combinational bucket (buffer) and the sequential class (boundary FF),
  // guaranteed non-empty by validateSpecConfig's mode-exclusivity check and
  // Stage D's sequential_ratio > 0 bootstrap rule respectively.
  odb::dbMaster* buffer_master = nullptr;
  for (const auto& bucket : plan.comb) {
    if (!bucket.empty()) {
      buffer_master = bucket.front();
      break;
    }
  }
  odb::dbMaster* ff_master = plan.seq.empty() ? nullptr : plan.seq.front();

  auto makeFeedNet = [&](odb::dbITerm* sink_iterm) {
    // PI-only helper: the tiny feed net for a buffered/registered PI, whose
    // only elements are the new instance's data-input sink and the PI
    // bTerm itself.
    odb::dbNet* feed = builder.makeNet("n" + std::to_string(next_net_id++));
    sink_iterm->connect(feed);
    const std::string bname = "pi" + std::to_string(stats.pi_nets.size());
    odb::dbBTerm* bterm = odb::dbBTerm::create(feed, bname.c_str());
    bterm->setIoType(odb::dbIoType::INPUT);
    return feed;
  };

  for (odb::dbNet* net : pi_nets) {
    odb::dbITerm* old_driver = nullptr;
    for (odb::dbITerm* it : net->getITerms()) {
      if (it->getIoType() == odb::dbIoType::OUTPUT) {
        old_driver = it;
        break;
      }
    }
    if (old_driver != nullptr) {
      old_driver->disconnect();
    }
    const IoPinKind kind = static_cast<IoPinKind>(pick_pin_type(rng));
    switch (kind) {
      case IoPinKind::kCombinational: {
        const std::string bname = "pi" + std::to_string(stats.pi_nets.size());
        odb::dbBTerm* bterm = odb::dbBTerm::create(net, bname.c_str());
        bterm->setIoType(odb::dbIoType::INPUT);
        ++stats.n_combinational;
        break;
      }
      case IoPinKind::kBuffered: {
        odb::dbInst* buf = builder.makeInst(
            buffer_master, "u" + std::to_string(next_inst_id++));
        odb::dbITerm* out = firstOutputIterm(buf);
        odb::dbITerm* in = firstDataInputIterm(buf);
        out->connect(net);
        makeFeedNet(in);
        stats.boundary_buf_insts.push_back(buf);
        ++stats.n_buffered;
        break;
      }
      case IoPinKind::kRegistered: {
        odb::dbInst* ff = builder.makeInst(
            ff_master, "u" + std::to_string(next_inst_id++));
        odb::dbITerm* q = firstOutputIterm(ff);
        odb::dbITerm* d = firstDataInputIterm(ff);
        q->connect(net);
        makeFeedNet(d);
        stats.boundary_reg_insts.push_back(ff);
        ++stats.n_registered;
        ++stats.n_boundary_ff;
        break;
      }
    }
    stats.pi_nets.push_back(net);
  }

  for (odb::dbNet* net : po_nets) {
    const IoPinKind kind = static_cast<IoPinKind>(pick_pin_type(rng));
    switch (kind) {
      case IoPinKind::kCombinational: {
        const std::string bname = "po" + std::to_string(stats.po_nets.size());
        odb::dbBTerm* bterm = odb::dbBTerm::create(net, bname.c_str());
        bterm->setIoType(odb::dbIoType::OUTPUT);
        ++stats.n_combinational;
        break;
      }
      case IoPinKind::kBuffered: {
        odb::dbInst* buf = builder.makeInst(
            buffer_master, "u" + std::to_string(next_inst_id++));
        odb::dbITerm* in = firstDataInputIterm(buf);
        odb::dbITerm* out = firstOutputIterm(buf);
        in->connect(net);  // additional sink of the existing net
        odb::dbNet* feed = builder.makeNet("n" + std::to_string(next_net_id++));
        out->connect(feed);
        const std::string bname = "po" + std::to_string(stats.po_nets.size());
        odb::dbBTerm* bterm = odb::dbBTerm::create(feed, bname.c_str());
        bterm->setIoType(odb::dbIoType::OUTPUT);
        stats.boundary_buf_insts.push_back(buf);
        ++stats.n_buffered;
        break;
      }
      case IoPinKind::kRegistered: {
        odb::dbInst* ff = builder.makeInst(
            ff_master, "u" + std::to_string(next_inst_id++));
        odb::dbITerm* d = firstDataInputIterm(ff);
        odb::dbITerm* q = firstOutputIterm(ff);
        d->connect(net);  // additional sink of the existing net
        odb::dbNet* feed = builder.makeNet("n" + std::to_string(next_net_id++));
        q->connect(feed);
        const std::string bname = "po" + std::to_string(stats.po_nets.size());
        odb::dbBTerm* bterm = odb::dbBTerm::create(feed, bname.c_str());
        bterm->setIoType(odb::dbIoType::OUTPUT);
        stats.boundary_reg_insts.push_back(ff);
        ++stats.n_registered;
        ++stats.n_boundary_ff;
        break;
      }
    }
    stats.po_nets.push_back(net);
  }

  // ---- Step 6: actual Rent computation for the full design ----
  if (G > 1 && stats.T_actual > 0) {
    stats.p_actual = std::log(stats.T_actual) / std::log(G);
    stats.k_actual = stats.T_actual / std::pow(G, stats.p_actual);
  }

  // ---- Step 5: sub-cluster (+ background) Rent computation ----
  // cluster_id is index-aligned with the ORIGINAL internal instances
  // (u0..u{G-1}); boundary buf/FF cells created just above (u{G}..) have no
  // entry and are treated as background (-1) for cut-net purposes, same as
  // any dbBTerm — both are, definitionally, outside every user cluster.
  const bool has_any_cluster =
      std::any_of(cluster_id.begin(), cluster_id.end(),
                  [](int c) { return c >= 0; });
  if (has_any_cluster) {
    stats.has_clusters = true;
    auto clusterOfInst = [&](odb::dbInst* inst) -> int {
      const int idx = std::atoi(inst->getName().c_str() + 1);
      return idx < static_cast<int>(cluster_id.size()) ? cluster_id[idx] : -1;
    };
    // For each net, the set of distinct clusters its endpoints touch
    // (instance-owned ITerms via clusterOfInst; a connected dbBTerm always
    // counts as "outside every cluster").
    auto touchedClusters = [&](odb::dbNet* net, bool* touches_boundary) {
      std::vector<int> clusters;
      *touches_boundary = !net->getBTerms().empty();
      for (odb::dbITerm* it : net->getITerms()) {
        clusters.push_back(clusterOfInst(it->getInst()));
      }
      return clusters;
    };

    const int num_clusters = spec.num_peak_clusters.value_or(1);
    for (int c = 0; c < num_clusters; ++c) {
      int G_c = 0;
      for (int i = 0; i < G; ++i) {
        if (cluster_id[i] == c) {
          ++G_c;
        }
      }
      int T_c = 0;
      for (odb::dbNet* net : builder.block()->getNets()) {
        bool touches_boundary = false;
        const std::vector<int> clusters = touchedClusters(net, &touches_boundary);
        const bool in_c = std::any_of(clusters.begin(), clusters.end(),
                                      [c](int x) { return x == c; });
        const bool out_c =
            touches_boundary
            || std::any_of(clusters.begin(), clusters.end(),
                           [c](int x) { return x != c; });
        if (in_c && out_c) {
          ++T_c;
        }
      }
      if (G_c < 2 || T_c < 1) {
        if (logger != nullptr) {
          logger->warn(utl::UKN, kMsgRentDegenerate,
                       "E1: cluster {} is degenerate (G_c={}, T_c={}); "
                       "skipping its Rent stats",
                       c, G_c, T_c);
        }
        continue;
      }
      ClusterRentStats cr;
      cr.cluster_idx = c;
      cr.G_c = G_c;
      cr.T_c = T_c;
      cr.p_c = std::log(T_c) / std::log(G_c);
      cr.k_c = T_c / std::pow(G_c, cr.p_c);
      stats.cluster_rent.push_back(cr);
    }

    // Background: G_bg = original internal instances with cluster_id == -1.
    // T_bg = nets with a background endpoint AND (a non-background-cluster
    // endpoint OR a PI/PO bTerm) — a strict superset of the cross-cluster
    // rule above, since a net entirely inside the background that also
    // reaches a boundary port is itself a boundary-crossing (I/O) net.
    int G_bg = 0;
    for (int i = 0; i < G; ++i) {
      if (cluster_id[i] < 0) {
        ++G_bg;
      }
    }
    int T_bg = 0;
    for (odb::dbNet* net : builder.block()->getNets()) {
      bool touches_boundary = false;
      const std::vector<int> clusters = touchedClusters(net, &touches_boundary);
      const bool has_bg = std::any_of(clusters.begin(), clusters.end(),
                                      [](int x) { return x < 0; });
      const bool has_non_bg_cluster =
          std::any_of(clusters.begin(), clusters.end(),
                     [](int x) { return x >= 0; });
      if (has_bg && (has_non_bg_cluster || touches_boundary)) {
        ++T_bg;
      }
    }
    stats.G_bg = G_bg;
    stats.T_bg = T_bg;
    if (G_bg < 2 || T_bg < 1) {
      if (logger != nullptr) {
        logger->warn(utl::UKN, kMsgRentDegenerate,
                     "E1: background is degenerate (G_bg={}, T_bg={}); "
                     "skipping its Rent stats",
                     G_bg, T_bg);
      }
    } else {
      stats.background_valid = true;
      stats.p_bg = std::log(T_bg) / std::log(G_bg);
      stats.k_bg = T_bg / std::pow(G_bg, stats.p_bg);
    }
  }

  if (logger != nullptr) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "E1: T_target {} -> T_actual {} (PI {}, PO {}); pin types "
               "comb {} / buf {} / reg {}; k_actual {:.3f} p_actual {:.3f}",
               T_target, stats.T_actual, stats.T_in, stats.T_out,
               stats.n_combinational, stats.n_buffered, stats.n_registered,
               stats.k_actual, stats.p_actual);
  }
  return stats;
}

// Legacy weighted-mix generation (Stage A path). Deterministic for a given
// (spec, seed); net shapes reflect the loads-based fanout definition.
int generateLegacy(NetlistBuilder& builder, const SyntheticNetlistSpec& spec,
                   std::mt19937& rng)
{
  std::vector<odb::dbMaster*> masters;
  std::vector<double> weights;
  for (const MasterSpec& ms : spec.masters) {
    masters.push_back(
        builder.makeMaster(ms.name, ms.num_inputs, ms.num_outputs));
    weights.push_back(ms.weight);
  }

  std::discrete_distribution<int> pick_master(weights.begin(), weights.end());
  std::vector<odb::dbInst*> insts;
  for (int i = 0; i < spec.num_insts; ++i) {
    insts.push_back(
        builder.makeInst(masters[pick_master(rng)], "u" + std::to_string(i)));
  }
  return formNets(builder, insts, spec, rng);
}

// Statistical-mix generation (Stage B): per-instance sequential/combinational
// roll, then a bucket roll, then a uniform pick among the bucket's masters.
int generateStatistical(NetlistBuilder& builder,
                        const SyntheticNetlistSpec& spec, std::mt19937& rng,
                        std::vector<int>* out_cluster_id,
                        RentStats* out_rent_stats)
{
  utl::Logger* logger = builder.logger();
  GenPlan plan;
  if (!buildPlan(builder, spec, plan)) {
    return -1;
  }
  if (logger != nullptr) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "plan: seq_ratio {:.3f}, bucket prob [{:.3f}, {:.3f}, {:.3f}, "
               "{:.3f}, {:.3f}], seq masters {}",
               plan.seq_ratio, plan.prob[0], plan.prob[1], plan.prob[2],
               plan.prob[3], plan.prob[4], static_cast<int>(plan.seq.size()));
  }

  std::uniform_real_distribution<double> seq_roll(0.0, 1.0);
  std::discrete_distribution<int> pick_bucket(plan.prob.begin(),
                                              plan.prob.end());

  int seq_count = 0;
  long comb_signal_pins = 0;
  std::array<int, kNumCombBuckets> bucket_count{};
  std::vector<odb::dbInst*> insts;
  insts.reserve(spec.num_insts);
  for (int i = 0; i < spec.num_insts; ++i) {
    odb::dbMaster* master = nullptr;
    if (!plan.seq.empty() && seq_roll(rng) < plan.seq_ratio) {
      std::uniform_int_distribution<size_t> pick(0, plan.seq.size() - 1);
      master = plan.seq[pick(rng)];
      ++seq_count;
    } else {
      const int b = pick_bucket(rng);
      std::uniform_int_distribution<size_t> pick(0, plan.comb[b].size() - 1);
      master = plan.comb[b][pick(rng)];
      ++bucket_count[b];
      comb_signal_pins += signalPinCount(master);
    }
    insts.push_back(builder.makeInst(master, "u" + std::to_string(i)));
    if (logger != nullptr && (i + 1) % 100000 == 0) {
      debugPrint(logger, utl::UKN, kGroup, kVerbosityHeartbeat,
                 "instances: {} / {} created", i + 1, spec.num_insts);
    }
  }

  // Stage D: ordered, acyclic-by-construction net formation (the legacy path
  // keeps the original shuffled-pool formNets). cluster_id is threaded
  // through unconditionally — Stage E1's sub-cluster Rent computation below
  // needs it regardless of whether the caller asked for out_cluster_id.
  std::vector<int> cluster_id;
  const int nets_made = formNetsAcyclic(builder, insts, spec, rng, &cluster_id);
  if (out_cluster_id != nullptr) {
    *out_cluster_id = cluster_id;
  }

  // Post-generation empirical check: logged warning only, never a failure.
  const double tol = spec.distribution_tolerance_pct;
  const double obs_seq = 100.0 * seq_count / spec.num_insts;
  const double want_seq = 100.0 * plan.seq_ratio;

  // Achieved-vs-requested: warn() below only fires on a tolerance miss; at
  // verbosity 1 the achieved values are logged unconditionally (see brief §4).
  if (logger != nullptr) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "achieved: sequential {:.2f}% (target {:.2f}%), bucket shares "
               "[{}]",
               obs_seq, want_seq, "see per-bucket detail below");
    const int comb_dbg = spec.num_insts - seq_count;
    for (int i = 0; i < kNumCombBuckets && comb_dbg > 0; ++i) {
      debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
                 "  bucket {}: achieved {:.2f}% (target {:.2f}%)", i,
                 100.0 * bucket_count[i] / comb_dbg, 100.0 * plan.prob[i]);
    }
  }
  if (std::abs(obs_seq - want_seq) > tol) {
    logger->warn(utl::UKN, kMsgTolerance,
                 "empirical sequential ratio {:.2f}% deviates from target "
                 "{:.2f}% by more than {:.2f}%",
                 obs_seq, want_seq, tol);
  }
  const int comb_total = spec.num_insts - seq_count;
  if (comb_total > 0) {
    for (int i = 0; i < kNumCombBuckets; ++i) {
      const double obs = 100.0 * bucket_count[i] / comb_total;
      const double want = 100.0 * plan.prob[i];
      if (std::abs(obs - want) > tol) {
        logger->warn(utl::UKN, kMsgTolerance,
                     "empirical bucket {} share {:.2f}% deviates from target "
                     "{:.2f}% by more than {:.2f}%",
                     i, obs, want, tol);
      }
    }
  }
  if (spec.target_avg_fanout.has_value() && comb_total > 0) {
    // The pin-count distribution controls the mean signal-pin count of the
    // combinational instances. target_avg_fanout is a fanout (driver excluded),
    // so compare the observed mean fanout = mean signal-pin count minus the one
    // driver pin. (Actual per-net fanout is still governed by
    // [min_fanout, max_fanout] this stage.)
    const double obs_fanout =
        static_cast<double>(comb_signal_pins) / comb_total - 1.0;
    if (std::abs(obs_fanout - *spec.target_avg_fanout) > tol) {
      logger->warn(utl::UKN, kMsgTolerance,
                   "empirical mean combinational fanout {:.3f} "
                   "deviates from target_avg_fanout {:.3f}",
                   obs_fanout, *spec.target_avg_fanout);
    }
  }

  // Stage E1: primary I/O generation via Rent's rule, a separate pass over
  // the already-formed dbBlock (no-op unless rent_k/rent_p are both set).
  // Continues the u<i>/n<i> naming sequences from the internal counts above.
  const RentStats rent_stats = applyPrimaryIoStageE1(
      builder, spec, rng, plan, spec.num_insts, nets_made, cluster_id);
  if (out_rent_stats != nullptr) {
    *out_rent_stats = rent_stats;
  }
  return nets_made;
}

}  // namespace

int generateSynthetic(NetlistBuilder& builder,
                      const SyntheticNetlistSpec& spec,
                      std::vector<int>* out_cluster_id,
                      RentStats* out_rent_stats)
{
  utl::Logger* logger = builder.logger();
  const bool stat = spec.usesStatisticalMix();
  if (logger != nullptr) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "generateSynthetic: {} instances, seed {}, fanout [{}, {}], "
               "mix={}",
               spec.num_insts, spec.seed, spec.min_fanout, spec.max_fanout,
               stat ? "statistical" : "legacy");
  }

  if (!validateSpecConfig(spec, logger)) {
    return -1;
  }

  // LEF-backed builders load their tech/cells from the spec's paths before
  // any instance is created.
  if (spec.tech_lef_path.has_value()) {
    if (logger != nullptr) {
      debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
                 "generateSynthetic: loading LEF (tech {} + {} cell file(s))",
                 *spec.tech_lef_path,
                 static_cast<int>(spec.cell_lef_paths.size()));
    }
    if (!builder.loadLef(*spec.tech_lef_path, spec.cell_lef_paths)) {
      return -1;
    }
  }

  std::mt19937 rng(spec.seed);
  int nets = -1;
  if (!stat) {
    if (spec.masters.empty()) {
      logger->warn(utl::UKN, kMsgNoMode,
                   "legacy spec has an empty masters list");
      return -1;
    }
    nets = generateLegacy(builder, spec, rng);
  } else {
    nets = generateStatistical(builder, spec, rng, out_cluster_id,
                               out_rent_stats);
  }
  if (logger != nullptr && nets >= 0) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "generateSynthetic: done — {} instances, {} nets",
               spec.num_insts, nets);
  }
  return nets;
}

}  // namespace eda
