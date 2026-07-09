// Library-linkage smoke test for the `netlistgen` engine (Stage A).
//
// This target links against the `netlistgen` library the way any external
// consumer would — `target_link_libraries(... PRIVATE netlistgen odb utl)` —
// and calls into it, without compiling netlistgen's sources directly. Its
// job is not to test generation quality (netlistgen_test does that) but to
// prove `netlistgen` is a real, linkable library target: if it ever
// regresses to header-only or its symbols stop being emitted, this binary
// fails to link and CTest reports it.
//
// No GTest and no data files: a plain main() returning non-zero on failure
// keeps the linkage surface minimal.

#include <cstdio>

#include "engines/netlistgen/netlistgen.h"
#include "odb/db.h"

int main()
{
  eda::NetlistBuilder builder("link_smoke");

  eda::SyntheticNetlistSpec spec;
  spec.masters = {{"INV", 1, 1, 1.0}, {"NAND2", 2, 1, 1.0}};
  spec.num_insts = 32;
  spec.seed = 1;

  const int nets = eda::generateSynthetic(builder, spec);

  const int insts = static_cast<int>(builder.block()->getInsts().size());
  if (insts != spec.num_insts) {
    std::fprintf(stderr, "expected %d insts, got %d\n", spec.num_insts, insts);
    return 1;
  }
  if (nets <= 0) {
    std::fprintf(stderr, "expected >0 nets, got %d\n", nets);
    return 1;
  }
  std::printf("netlistgen link smoke ok: %d insts, %d nets\n", insts, nets);
  return 0;
}
