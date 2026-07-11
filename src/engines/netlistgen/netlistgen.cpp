// See netlistgen.h for the construction protocol and generator model.

#include "engines/netlistgen/netlistgen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <random>
#include <string>

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

bool validateSpecConfig(const SyntheticNetlistSpec& spec, utl::Logger* logger)
{
  if (spec.num_insts <= 0) {
    if (logger) {
      logger->warn(utl::UKN, kMsgNoMode, "num_insts must be > 0");
    }
    return false;
  }
  if (!spec.usesStatisticalMix()) {
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
    // fanout range is the anchor range shifted down by one. In LEF mode the
    // range is re-checked against measured anchors during plan building.
    const double t_pins = *spec.target_avg_fanout + 1.0;
    const double lo = kSyntheticBucketAnchors.front();
    const double hi = kSyntheticBucketAnchors.back();
    if (!spec.tech_lef_path.has_value() && (t_pins <= lo || t_pins >= hi)) {
      if (logger) {
        logger->warn(utl::UKN, kMsgTargetRange,
                     "target_avg_fanout must be strictly inside ({}, {}), "
                     "got {}",
                     lo - 1.0, hi - 1.0, *spec.target_avg_fanout);
      }
      return false;
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
  if (target_pins <= lo || target_pins >= hi) {
    if (logger) {
      logger->warn(utl::UKN, kMsgTargetRange,
                   "target_avg_fanout {} is outside the measured fanout "
                   "range ({}, {})",
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

// Form nets from two shuffled pin pools: one driver (OUTPUT) plus fanout-1
// sinks (INPUT/INOUT) per net, popping each iterm at most once. Power/ground
// pins are never eligible. Shared by the legacy and statistical paths; for
// synthetic masters (no power pins) the classification is identical to the
// Stage A code, keeping legacy output bit-identical.
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

    const int num_sinks = pick_fanout(rng) - 1;
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

// Legacy weighted-mix generation, byte-for-byte identical to Stage A.
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
                        const SyntheticNetlistSpec& spec, std::mt19937& rng)
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

  const int nets_made = formNets(builder, insts, spec, rng);

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
  return nets_made;
}

}  // namespace

int generateSynthetic(NetlistBuilder& builder,
                      const SyntheticNetlistSpec& spec)
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
    nets = generateStatistical(builder, spec, rng);
  }
  if (logger != nullptr && nets >= 0) {
    debugPrint(logger, utl::UKN, kGroup, kVerbosityDetail,
               "generateSynthetic: done — {} instances, {} nets",
               spec.num_insts, nets);
  }
  return nets;
}

}  // namespace eda
