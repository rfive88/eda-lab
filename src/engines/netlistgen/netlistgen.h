// eda-lab: programmatic netlist construction (Phase 0).
//
// Builds a dbBlock through OpenDB API calls only — no LEF/DEF input — so
// tests and benchmarks can construct netlists of any size with exactly
// known or statistically controlled topology, then hand the block to
// Hypergraph::buildFromBlock().
//
// Two layers:
//
//   - NetlistBuilder: owns a fresh dbDatabase (tech, lib, chip, top block)
//     and wraps the create/connect calls, including the master-freeze
//     protocol OpenDB requires before instantiation. Masters made here
//     have pins named i0..i{n-1} / o0..o{m-1} and no geometry — they are
//     connectivity-only.
//
//   - generateSynthetic(): populates a builder's block from a
//     SyntheticNetlistSpec — a weighted cell mix, an instance count, and a
//     net fanout range — using a seeded RNG, so a given (spec, seed) pair
//     reproduces the same netlist. Every iterm is used at most once, so
//     the result is a valid netlist (each pin on at most one net).

#pragma once

#include <cstdint>
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

class NetlistBuilder
{
 public:
  explicit NetlistBuilder(const std::string& design_name = "synth");
  ~NetlistBuilder();

  NetlistBuilder(const NetlistBuilder&) = delete;
  NetlistBuilder& operator=(const NetlistBuilder&) = delete;

  // Create a connectivity-only master with input pins i0..i{num_inputs-1}
  // and output pins o0..o{num_outputs-1}. The master is frozen on return
  // and ready to instantiate. Returns nullptr if the name already exists.
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

  odb::dbBlock* block() const { return block_; }
  odb::dbDatabase* db() const { return db_; }

 private:
  utl::Logger* logger_ = nullptr;
  odb::dbDatabase* db_ = nullptr;
  odb::dbBlock* block_ = nullptr;
};

// One cell type in the synthetic mix. `weight` is the relative frequency
// with which instances of this master are created (need not sum to 1).
struct MasterSpec
{
  std::string name;
  int num_inputs = 2;
  int num_outputs = 1;
  double weight = 1.0;
};

struct SyntheticNetlistSpec
{
  std::vector<MasterSpec> masters;  // must be non-empty
  int num_insts = 0;                // must be > 0

  // Nets are created until this count is reached, or until the free
  // driver/sink pin pools run out, whichever comes first. -1 means "as
  // many as the pin pools allow".
  int num_nets = -1;

  // Total pins per net, driver included, drawn uniformly from
  // [min_fanout, max_fanout]. A net gets one output pin as driver and
  // fanout-1 input pins as sinks (fewer if the sink pool runs dry).
  int min_fanout = 2;
  int max_fanout = 4;

  uint32_t seed = 1;
};

// Populate builder's block from the spec. Instances are named u0..u{n-1}
// and nets n0..n{k-1}. Deterministic for a given (spec, seed) and
// standard library. Returns the number of nets actually created.
int generateSynthetic(NetlistBuilder& builder,
                      const SyntheticNetlistSpec& spec);

}  // namespace eda
