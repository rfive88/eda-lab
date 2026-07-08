// See netlistgen.h for the construction protocol and generator model.

#include "netlistgen/netlistgen.h"

#include <algorithm>
#include <random>

#include "odb/db.h"
#include "utl/Logger.h"

namespace eda {

NetlistBuilder::NetlistBuilder(const std::string& design_name)
{
  logger_ = new utl::Logger();
  db_ = odb::dbDatabase::create();
  db_->setLogger(logger_);

  odb::dbTech* tech = odb::dbTech::create(db_, "tech");
  odb::dbLib::create(db_, "lib", tech);
  odb::dbChip* chip = odb::dbChip::create(db_, tech);
  block_ = odb::dbBlock::create(chip, design_name.c_str());
}

NetlistBuilder::~NetlistBuilder()
{
  odb::dbDatabase::destroy(db_);
  delete logger_;
}

odb::dbMaster* NetlistBuilder::makeMaster(const std::string& name,
                                          const int num_inputs,
                                          const int num_outputs)
{
  odb::dbLib* lib = *db_->getLibs().begin();
  odb::dbMaster* master = odb::dbMaster::create(lib, name.c_str());
  if (master == nullptr) {
    return nullptr;
  }
  master->setType(odb::dbMasterType::CORE);
  for (int i = 0; i < num_inputs; ++i) {
    odb::dbMTerm::create(master,
                         ("i" + std::to_string(i)).c_str(),
                         odb::dbIoType::INPUT,
                         odb::dbSigType::SIGNAL);
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
  return odb::dbInst::create(block_, master, name.c_str());
}

odb::dbNet* NetlistBuilder::makeNet(const std::string& name)
{
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

int generateSynthetic(NetlistBuilder& builder,
                      const SyntheticNetlistSpec& spec)
{
  std::mt19937 rng(spec.seed);

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

  // Pools of unused pins. Popping from a shuffled pool guarantees each
  // iterm lands on at most one net, keeping the netlist valid.
  std::vector<odb::dbITerm*> drivers;
  std::vector<odb::dbITerm*> sinks;
  for (odb::dbInst* inst : insts) {
    for (odb::dbITerm* iterm : inst->getITerms()) {
      if (iterm->getIoType() == odb::dbIoType::OUTPUT) {
        drivers.push_back(iterm);
      } else {
        sinks.push_back(iterm);
      }
    }
  }
  std::shuffle(drivers.begin(), drivers.end(), rng);
  std::shuffle(sinks.begin(), sinks.end(), rng);

  std::uniform_int_distribution<int> pick_fanout(spec.min_fanout,
                                                 spec.max_fanout);
  int nets_made = 0;
  while (!drivers.empty() && !sinks.empty()
         && (spec.num_nets < 0 || nets_made < spec.num_nets)) {
    odb::dbNet* net = builder.makeNet("n" + std::to_string(nets_made));
    drivers.back()->connect(net);
    drivers.pop_back();

    const int num_sinks = pick_fanout(rng) - 1;
    for (int s = 0; s < num_sinks && !sinks.empty(); ++s) {
      sinks.back()->connect(net);
      sinks.pop_back();
    }
    ++nets_made;
  }
  return nets_made;
}

}  // namespace eda
