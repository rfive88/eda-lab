// See cli_config.h.

#include "engines/netlistgen/cli_config.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "engines/netlistgen/netlist_validation.h"
#include "engines/netlistgen/netlist_writers.h"
#include "odb/db.h"
#include "support/logging.h"
#include "utl/Logger.h"

namespace eda {

using nlohmann::json;

namespace {

// The five combinational_pin_distribution bucket keys, in [2, 3, 4, 5, 6+]
// order. Kept parallel to kNumCombBuckets / kSyntheticBucketAnchors.
constexpr std::array<const char*, kNumCombBuckets> kBucketKeys = {"2", "3", "4",
                                                                  "5", "6+"};

// CLI-layer info phase-marker ids (utl::UKN; see support/logging.h). These are
// the default-visible narrative of a CLI run; library internals stay silent
// unless -verbosity raises the shared logger's debug level.
constexpr const char* kGroup = "netlistgen";
constexpr int kMsgParsing = 320;
constexpr int kMsgParsed = 321;
constexpr int kMsgGenerating = 322;
constexpr int kMsgGenerated = 323;
constexpr int kMsgValidating = 324;
constexpr int kMsgValidated = 325;
constexpr int kMsgWroteDef = 326;
constexpr int kMsgWroteOdb = 327;
constexpr int kMsgDone = 328;
constexpr int kMsgCreatedDir = 329;

}  // namespace

bool parseCliConfig(const std::string& json_text,
                    CliConfig& out,
                    std::string& error)
{
  json j;
  try {
    j = json::parse(json_text);
  } catch (const json::parse_error& e) {
    error = std::string("JSON parse error: ") + e.what();
    return false;
  }
  if (!j.is_object()) {
    error = "config root must be a JSON object";
    return false;
  }

  SyntheticNetlistSpec& spec = out.spec;

  try {
    // ---- Required: instance_count ----
    if (!j.contains("instance_count") || j.at("instance_count").is_null()) {
      error = "missing required field 'instance_count'";
      return false;
    }
    spec.num_insts = j.at("instance_count").get<int>();

    // ---- Optional scalars ----
    if (j.contains("seed") && !j.at("seed").is_null()) {
      spec.seed = j.at("seed").get<uint32_t>();
    }
    // net_count: null (or absent) means "as many as the pin pools allow" (-1).
    if (j.contains("net_count") && !j.at("net_count").is_null()) {
      spec.num_nets = j.at("net_count").get<int>();
    }
    if (j.contains("distribution_tolerance_pct")
        && !j.at("distribution_tolerance_pct").is_null()) {
      spec.distribution_tolerance_pct =
          j.at("distribution_tolerance_pct").get<double>();
    }

    // ---- fanout_range { min, max } ----
    if (j.contains("fanout_range") && !j.at("fanout_range").is_null()) {
      const json& fr = j.at("fanout_range");
      if (!fr.is_object() || !fr.contains("min") || !fr.contains("max")) {
        error = "'fanout_range' must be an object with 'min' and 'max'";
        return false;
      }
      spec.min_fanout = fr.at("min").get<int>();
      spec.max_fanout = fr.at("max").get<int>();
    }

    // ---- LEF-backed generation ----
    if (j.contains("tech_lef_path") && !j.at("tech_lef_path").is_null()) {
      spec.tech_lef_path = j.at("tech_lef_path").get<std::string>();
    }
    if (j.contains("cell_lef_paths") && !j.at("cell_lef_paths").is_null()) {
      spec.cell_lef_paths =
          j.at("cell_lef_paths").get<std::vector<std::string>>();
    }

    // ---- Statistical cell mix ----
    if (j.contains("sequential_ratio") && !j.at("sequential_ratio").is_null()) {
      spec.sequential_ratio = j.at("sequential_ratio").get<double>();
    }
    if (j.contains("target_avg_fanout")
        && !j.at("target_avg_fanout").is_null()) {
      spec.target_avg_fanout = j.at("target_avg_fanout").get<double>();
    }
    if (j.contains("combinational_pin_distribution")
        && !j.at("combinational_pin_distribution").is_null()) {
      const json& cpd = j.at("combinational_pin_distribution");
      if (!cpd.is_object()) {
        error = "'combinational_pin_distribution' must be an object keyed by "
                "bucket ('2','3','4','5','6+')";
        return false;
      }
      std::array<double, kNumCombBuckets> dist{};
      for (int i = 0; i < kNumCombBuckets; ++i) {
        if (!cpd.contains(kBucketKeys[i])) {
          error = std::string("'combinational_pin_distribution' is missing "
                              "bucket key '")
                  + kBucketKeys[i] + "'";
          return false;
        }
        dist[i] = cpd.at(kBucketKeys[i]).get<double>();
      }
      spec.combinational_pin_distribution = dist;
    }

    // ---- Peak fanout sub-clusters (optional; see netlistgen.h) ----
    if (j.contains("peak_avg_fanout") && !j.at("peak_avg_fanout").is_null()) {
      spec.peak_avg_fanout = j.at("peak_avg_fanout").get<double>();
    }
    if (j.contains("peak_cluster_pct")
        && !j.at("peak_cluster_pct").is_null()) {
      spec.peak_cluster_pct = j.at("peak_cluster_pct").get<double>();
    }
    if (j.contains("num_peak_clusters")
        && !j.at("num_peak_clusters").is_null()) {
      spec.num_peak_clusters = j.at("num_peak_clusters").get<int>();
    }

    // ---- Stage E1: primary I/O generation via Rent's rule (optional) ----
    if (j.contains("rent_k") && !j.at("rent_k").is_null()) {
      spec.rent_k = j.at("rent_k").get<double>();
    }
    if (j.contains("rent_p") && !j.at("rent_p").is_null()) {
      spec.rent_p = j.at("rent_p").get<double>();
    }
    if (j.contains("io_input_ratio") && !j.at("io_input_ratio").is_null()) {
      spec.io_input_ratio = j.at("io_input_ratio").get<double>();
    }
    if (j.contains("io_pin_type_distribution")
        && !j.at("io_pin_type_distribution").is_null()) {
      const json& pt = j.at("io_pin_type_distribution");
      if (!pt.is_object()) {
        error = "'io_pin_type_distribution' must be an object keyed by "
                "'combinational', 'buffered', 'registered'";
        return false;
      }
      IoPinTypeDistribution dist;
      if (pt.contains("combinational")) {
        dist.combinational = pt.at("combinational").get<double>();
      }
      if (pt.contains("buffered")) {
        dist.buffered = pt.at("buffered").get<double>();
      }
      if (pt.contains("registered")) {
        dist.registered = pt.at("registered").get<double>();
      }
      spec.io_pin_type_distribution = dist;
    }

    // ---- CLI-only I/O fields ----
    if (j.contains("output_def_path") && !j.at("output_def_path").is_null()) {
      out.output_def_path = j.at("output_def_path").get<std::string>();
    }
    if (j.contains("output_odb_path") && !j.at("output_odb_path").is_null()) {
      out.output_odb_path = j.at("output_odb_path").get<std::string>();
    }
  } catch (const json::exception& e) {
    // Widened from json::type_error to the whole json::exception hierarchy so a
    // future edit that adds an unchecked .at() (json::out_of_range) or similar
    // still fails cleanly here instead of escaping as an uncaught exception.
    error = std::string("config field error: ") + e.what();
    return false;
  }

  // ---- CLI-level cross-field rule: at least one output path ----
  if (!out.output_def_path.has_value() && !out.output_odb_path.has_value()) {
    error = "at least one of 'output_def_path' / 'output_odb_path' must be set";
    return false;
  }
  return true;
}

namespace {

// A write to `path` needs its containing directory to exist — an ofstream on a
// path in a missing directory fails, and OpenROAD's DefOut would otherwise hit
// that failure deep inside a writer. Create the directory up front (including
// any missing parents) so a run doesn't fail just because the output dir hasn't
// been made yet; only a genuine failure to create it (e.g. a path component is
// a file, or a permission error) is converted to a clean diagnostic. An empty
// parent (a bare filename in the CWD) needs nothing created.
bool ensureOutputDir(const std::string& path,
                     utl::Logger* logger,
                     std::ostream& err)
{
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty() || std::filesystem::exists(parent)) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    err << "cannot create output directory: " << parent.string() << " (for "
        << path << "): " << ec.message() << "\n";
    return false;
  }
  if (logger != nullptr) {
    logger->info(utl::UKN, kMsgCreatedDir, "Created output directory: {}",
                 parent.string());
  }
  return true;
}

// Fanout-histogram bucketing: fanouts below kFanoutBucketLo get one row each;
// [kFanoutBucketLo, kFanoutBucketHi] collapse into a single "10-50" row and
// anything above kFanoutBucketHi into a ">50" row, so a design with large-fanout
// nets stays a compact table instead of one row per distinct value.
constexpr int kFanoutBucketLo = 10;
constexpr int kFanoutBucketHi = 50;

// Print a statistics summary of the generated design as the run's final,
// default-visible output: cell counts (combinational vs sequential),
// top-level pin counts (PI vs PO, by dbBTerm IoType), the combinational
// cells' signal-pin-count distribution, the net count, and the net fanout
// distribution. Emitted via report() (level OFF, no id/prefix) so it reads
// as a clean block; nothing here is gated on verbosity.
void reportDesignSummary(odb::dbBlock* block, utl::Logger& logger)
{
  // Cells: split combinational vs sequential, and histogram the combinational
  // cells by signal-pin count into the same 2/3/4/5/6+ buckets the generator
  // uses (index 0..4). Latches/clock gates are never placed, so every instance
  // is one or the other.
  int seq = 0;
  int comb = 0;
  std::array<int, 5> comb_pin_hist{};
  for (odb::dbInst* inst : block->getInsts()) {
    odb::dbMaster* master = inst->getMaster();
    if (isSequentialMaster(master)) {
      ++seq;
    } else {
      ++comb;
      const int pins = signalPinCount(master);
      const int idx = pins >= 6 ? 4 : std::max(0, pins - 2);
      ++comb_pin_hist[idx];
    }
  }
  const int total = seq + comb;

  // Top-level pins: dbBTerms split by direction. Only Stage E1 creates
  // these today (PI bTerms are always INPUT, PO bTerms OUTPUT), but count
  // generically by IoType rather than assuming a source, so this stays
  // correct if a future stage adds ports another way. INOUT counts as PO,
  // matching netlist_validation.h's "OUTPUT/INOUT is a sink" rule.
  int num_pi = 0;
  int num_po = 0;
  for (odb::dbBTerm* bterm : block->getBTerms()) {
    if (bterm->getIoType() == odb::dbIoType::INPUT) {
      ++num_pi;
    } else {
      ++num_po;
    }
  }

  // Nets: fanout is the number of load (sink) pins the net drives — i.e. its
  // pins EXCLUDING the driver. Compute it as total pins minus the OUTPUT
  // (driver) pins, so the driver is never counted regardless of net shape.
  const int num_nets = static_cast<int>(block->getNets().size());
  std::map<int, int> fanout_hist;
  long fanout_sum = 0;
  for (odb::dbNet* net : block->getNets()) {
    int pins = 0;
    int drivers = 0;
    for (odb::dbITerm* iterm : net->getITerms()) {
      ++pins;
      if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
        ++drivers;
      }
    }
    const int fanout = pins - drivers;
    ++fanout_hist[fanout];
    fanout_sum += fanout;
  }

  logger.report("");
  logger.report("===== Design summary =====");
  logger.report("Cells: {} total  (combinational {}, sequential {})", total,
                comb, seq);
  logger.report("Top-level pins: {} total  (PI {}, PO {})", num_pi + num_po,
                num_pi, num_po);
  logger.report("Combinational cells by signal-pin count:");
  static const char* const kPinLabels[5] = {"2", "3", "4", "5", "6+"};
  for (int i = 0; i < 5; ++i) {
    const double pct = comb > 0 ? 100.0 * comb_pin_hist[i] / comb : 0.0;
    logger.report("    {:>3} pins: {:6d}  ({:5.1f}%)", kPinLabels[i],
                  comb_pin_hist[i], pct);
  }
  logger.report("Nets: {}", num_nets);
  if (num_nets > 0) {
    const double avg = static_cast<double>(fanout_sum) / num_nets;
    logger.report("Average fanout per net: {:.2f} (driver excluded)", avg);
    logger.report("Net fanout distribution (loads per net, driver excluded):");
    const auto printRow = [&](const std::string& label, long count) {
      logger.report("    fanout {:>5}: {:6d}  ({:5.1f}%)", label, count,
                    100.0 * count / num_nets);
    };
    long mid = 0;   // fanout in [kFanoutBucketLo, kFanoutBucketHi]
    long high = 0;  // fanout > kFanoutBucketHi
    for (const auto& [fanout, count] : fanout_hist) {
      if (fanout < kFanoutBucketLo) {
        printRow(std::to_string(fanout), count);
      } else if (fanout <= kFanoutBucketHi) {
        mid += count;
      } else {
        high += count;
      }
    }
    if (mid > 0) {
      printRow(std::to_string(kFanoutBucketLo) + "-"
                   + std::to_string(kFanoutBucketHi),
               mid);
    }
    if (high > 0) {
      printRow(">" + std::to_string(kFanoutBucketHi), high);
    }
  }
  logger.report("==========================");
}

// Step 7 of Stage E1 (spike-netlistgen-E1-io-rent.md): append the Rent's-rule
// summary (and, when peak fanout sub-clusters are also engaged, the
// per-cluster + background Rent parameters) after the design summary above.
// A no-op when Stage E1 wasn't engaged (`stats.engaged` false).
void reportPrimaryIoSummary(const RentStats& stats, double sequential_ratio,
                            utl::Logger& logger)
{
  if (!stats.engaged) {
    return;
  }
  logger.report("");
  logger.report("===== Primary I/O Summary =====");
  logger.report("Target Rent:  k={:.3f}  p={:.3f}  ->  T_target={}",
                stats.rent_k_target, stats.rent_p_target, stats.T_target);
  if (stats.capped) {
    logger.report("  (T_target exceeded the net count; capped — see the "
                  "warning above)");
  }
  logger.report("Actual Rent:  k={:.3f}  p={:.3f}  ->  T_actual={}  (PI={}, "
                "PO={})",
                stats.k_actual, stats.p_actual, stats.T_actual, stats.T_in,
                stats.T_out);
  logger.report("Pin types:    combinational={}  buffered={}  registered={}",
                stats.n_combinational, stats.n_buffered, stats.n_registered);
  logger.report("Boundary FFs: {}  (tracked separately from internal "
                "sequential_ratio={:.2f})",
                stats.n_boundary_ff, sequential_ratio);

  if (stats.has_clusters) {
    logger.report("");
    logger.report("===== Sub-cluster Rent Parameters =====");
    for (const ClusterRentStats& cr : stats.cluster_rent) {
      logger.report(
          "Cluster {}:  G={}  T_c={}  k={:.3f}  p={:.3f}  avg_fanout={:.2f}",
          cr.cluster_idx, cr.G_c, cr.T_c, cr.k_c, cr.p_c, cr.avg_fanout_c);
    }
    if (stats.background_valid) {
      logger.report("Background:  G={}  T_bg={}  k={:.3f}  p={:.3f}",
                    stats.G_bg, stats.T_bg, stats.k_bg, stats.p_bg);
    }
  }
  logger.report("================================");
}

// Print the well-formedness check result — the hard structural gate from
// netlist_validation.h (single driver per net, >=1 load per net, no
// dangling nets, no dangling instances, no control pins on signal nets) —
// always, pass or fail, right before the design summary: a failing run
// still surfaces exactly what's wrong alongside the rest of the run's
// diagnostics, rather than only as a terse stderr line.
void reportWellFormedness(const NetlistValidation& v, utl::Logger& logger)
{
  logger.report("");
  logger.report("===== Well-formedness Check =====");
  if (v.ok) {
    logger.report("Status: PASS");
    logger.report("  (single driver per net, >=1 load per net, no dangling "
                  "nets/instances, no control pins on signal nets)");
  } else {
    logger.report("Status: FAIL");
    logger.report("  {}", v.message);
  }
  logger.report("==================================");
}

}  // namespace

bool validateAndWrite(NetlistBuilder& builder,
                      const CliConfig& config,
                      std::ostream& err)
{
  const NetlistValidation v = validateNetlist(builder.block());
  if (!v.ok) {
    err << "netlist validation failed: " << v.message
        << " (refusing to write output)\n";
    return false;
  }
  // Create any missing output directory before writing, and fail before writing
  // anything if one cannot be created, so no partial output is produced.
  if (config.output_def_path.has_value()
      && !ensureOutputDir(*config.output_def_path, builder.logger(), err)) {
    return false;
  }
  if (config.output_odb_path.has_value()
      && !ensureOutputDir(*config.output_odb_path, builder.logger(), err)) {
    return false;
  }
  if (config.output_def_path.has_value()) {
    if (!writeDef(builder.block(), *config.output_def_path, builder.logger())) {
      err << "failed to write DEF to " << *config.output_def_path << "\n";
      return false;
    }
  }
  if (config.output_odb_path.has_value()) {
    if (!writeOdb(builder.db(), *config.output_odb_path)) {
      err << "failed to write .odb to " << *config.output_odb_path << "\n";
      return false;
    }
  }
  return true;
}

int runCliFromFile(const std::string& config_path,
                   std::ostream& err,
                   int verbosity)
{
  // One logger for the whole run: the CLI's info phase markers and the library
  // engine (via NetlistBuilder) share it, so -verbosity lifts detail
  // everywhere at once. See support/logging.h.
  utl::Logger logger;
  applyVerbosity(&logger, kGroup, verbosity);

  logger.info(utl::UKN, kMsgParsing, "Parsing JSON config: {}", config_path);
  std::ifstream file(config_path);
  if (!file) {
    err << "cannot open config file: " << config_path << "\n";
    return 1;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  CliConfig config;
  std::string error;
  if (!parseCliConfig(buffer.str(), config, error)) {
    err << "config error: " << error << "\n";
    return 1;
  }
  logger.info(utl::UKN, kMsgParsed,
              "Config parsed: {} instances requested", config.spec.num_insts);

  NetlistBuilder builder("synth", &logger);
  logger.info(utl::UKN, kMsgGenerating, "Generating netlist: {} instances...",
              config.spec.num_insts);
  RentStats rent_stats;
  const int nets = generateSynthetic(builder, config.spec, /*out_cluster_id=*/
                                     nullptr, &rent_stats);
  if (nets < 0) {
    err << "generation failed: spec rejected (see logged diagnostics)\n";
    return 1;
  }

  odb::dbBlock* block = builder.block();
  long pins = 0;
  for (odb::dbInst* inst : block->getInsts()) {
    pins += inst->getITerms().size();
  }
  logger.info(utl::UKN, kMsgGenerated,
              "Generation complete: {} instances, {} nets, {} pins",
              block->getInsts().size(), block->getNets().size(), pins);

  // Auto-size the die area so the DEF carries a valid DIEAREA (nominal pitch in
  // synthetic mode, loaded site pitch in LEF mode). Instances stay UNPLACED.
  builder.estimateDieArea(config.spec.num_insts);

  logger.info(utl::UKN, kMsgValidating, "Running well-formedness validation...");
  // Computed once, up front, so the PASS/FAIL block below and the
  // write-gating in validateAndWrite agree on the exact same verdict.
  const NetlistValidation validation = validateNetlist(block);
  reportWellFormedness(validation, logger);

  if (!validateAndWrite(builder, config, err)) {
    // A structural violation still gets its stats printed (the design was
    // fully generated; the numbers are useful for diagnosing why) — but an
    // unrelated I/O failure (bad output dir, write error) with an
    // otherwise-valid netlist does not, matching the pre-existing
    // fail-fast-with-no-stats behavior for that case. Either way, no
    // output file was written (validateAndWrite's own gate) and the
    // process exits nonzero.
    if (!validation.ok) {
      reportDesignSummary(block, logger);
      reportPrimaryIoSummary(
          rent_stats, config.spec.sequential_ratio.value_or(0.0), logger);
    }
    return 1;
  }
  logger.info(utl::UKN, kMsgValidated, "Well-formedness validation passed.");

  if (config.output_def_path.has_value()) {
    logger.info(utl::UKN, kMsgWroteDef, "Wrote DEF: {}",
                *config.output_def_path);
  }
  if (config.output_odb_path.has_value()) {
    logger.info(utl::UKN, kMsgWroteOdb, "Wrote .odb: {}",
                *config.output_odb_path);
  }
  reportDesignSummary(block, logger);
  reportPrimaryIoSummary(rent_stats, config.spec.sequential_ratio.value_or(0.0),
                        logger);
  logger.info(utl::UKN, kMsgDone, "Done.");
  return 0;
}

}  // namespace eda
