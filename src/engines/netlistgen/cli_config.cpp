// See cli_config.h.

#include "engines/netlistgen/cli_config.h"

#include <array>
#include <filesystem>
#include <fstream>
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

// A write to `path` needs its containing directory to already exist — an ofstream
// on a path in a missing directory fails, and OpenROAD's DefOut would otherwise
// hit that failure deep inside a writer. Check up front and convert to a clean
// diagnostic. An empty parent (a bare filename in the CWD) is always fine.
bool outputDirExists(const std::string& path, std::ostream& err)
{
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty() && !std::filesystem::exists(parent)) {
    err << "output directory does not exist: " << parent.string()
        << " (for " << path << ")\n";
    return false;
  }
  return true;
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
  // Fail before writing anything if a requested output directory is missing, so
  // no partial output is produced.
  if (config.output_def_path.has_value()
      && !outputDirExists(*config.output_def_path, err)) {
    return false;
  }
  if (config.output_odb_path.has_value()
      && !outputDirExists(*config.output_odb_path, err)) {
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
  const int nets = generateSynthetic(builder, config.spec);
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
  if (!validateAndWrite(builder, config, err)) {
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
  logger.info(utl::UKN, kMsgDone, "Done.");
  return 0;
}

}  // namespace eda
