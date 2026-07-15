// eda-lab: repo-wide utl::Logger verbosity convention (single source of truth).
//
// This header documents the confirmed utl::Logger API and the runtime-verbosity
// convention every component in this repo follows. It is header-only: it adds a
// tiny applyVerbosity() helper and the level constants, nothing else.
//
// ---------------------------------------------------------------------------
// utl::Logger API, confirmed against the pinned OpenROAD SHA (a3d4865),
// third_party/openroad/src/utl/include/utl/Logger.h + src/Logger.cpp:
//
//   - Severity methods: report(msg,...) (level OFF, no id/prefix),
//     info(tool,id,msg,...), warn(tool,id,msg,...),
//     error(tool,id,msg,...)  -> THROWS std::runtime_error at this SHA,
//     critical(tool,id,msg,...) -> calls exit(). Because this repo signals
//     failure by return value and never throws from its APIs, library code
//     uses warn() for problems, never error()/critical().
//   - Debug tier (the runtime-verbosity mechanism): the
//        debugPrint(logger, tool, group, level, msg, ...)
//     macro emits only when setDebugLevel(tool, group, L) was previously called
//     with L >= level (Logger::debugCheck). OpenROAD's own tools use exactly
//     this for verbosity, so the -verbosity CLI flag maps straight onto it
//     rather than inventing a parallel numbering scheme. NOTE: debugPrint
//     dereferences its logger argument, so guard a possibly-null logger with an
//     explicit `if (logger != nullptr)` before the macro.
//   - Output routing: a Logger writes to a stdout colour sink (Logger::Logger).
//     DEF / .odb / Verilog / partition results are written to their own file
//     paths by odb writers, so log output is naturally kept out of the
//     deterministic data output files. The spdlog logger level is 'debug', so
//     debug() messages are emitted whenever debugCheck() passes.
//
// ---------------------------------------------------------------------------
// Convention (authoritative statement lives in CLAUDE.md):
//
//   - Tool id: utl::UKN for every eda-lab message — the pinned ToolId enum has
//     no dedicated eda-lab tool. Message ids (used only by info()/warn(), never
//     by debug()) are partitioned per component so they stay unique across the
//     shared UKN namespace: hypergraph 100-119, fm 120-129, hg_metrics
//     130-149, hello_odb 200-209, netlistgen library 300-319, netlistgen CLI
//     320-349, structural_metrics core 350-374, structural_metrics CLI
//     375-399.
//   - Debug groups: one per component ("hypergraph", "fm", "netlistgen",
//     "hg_metrics", "hello_odb", "structural_metrics"), so raising -verbosity
//     lifts detail for a whole run.
//   - Verbosity levels (the integer passed to -verbosity / setDebugLevel):
//       0  default: phase markers (info, at the executable layer) plus a final
//          summary and any warnings/errors. Library internals stay silent.
//       1  per-phase sub-detail: counts, achieved-vs-requested statistics, and
//          library-level phase markers.
//       2  periodic progress heartbeats for long-running operations.
//       3  low-level per-item tracing (per-net formation, per-move gains);
//          CAPPED for large runs — the emitting site prints a "trace capped"
//          note when it stops so the omission is explicit.

#pragma once

#include "utl/Logger.h"

namespace eda {

// Verbosity level names, mapped 1:1 onto utl::Logger debug levels.
inline constexpr int kVerbosityDefault = 0;
inline constexpr int kVerbosityDetail = 1;
inline constexpr int kVerbosityHeartbeat = 2;
inline constexpr int kVerbosityTrace = 3;

// Per-run cap on level-3 per-item trace lines, so a large design cannot emit
// unbounded output. The emitting site announces when it hits the cap.
inline constexpr int kTraceCap = 64;

// Apply a -verbosity level to `logger` for the debug `group`. A level <= 0 (or
// a null logger) is a no-op: only info phase markers and warnings then show.
inline void applyVerbosity(utl::Logger* logger, const char* group, int level)
{
  if (logger != nullptr && level > 0) {
    logger->setDebugLevel(utl::UKN, group, level);
  }
}

}  // namespace eda
