// eda-lab: shared error/result type for graceful failure propagation
// (repo-wide convention). See the "Error handling" section of CLAUDE.md.
//
// Expected, recoverable failures — a file path that does not exist, a LEF that
// fails to parse, an output directory that is missing — are surfaced as a
// value that bubbles up to main(), not as a crash. This header is the
// single-source primary mechanism: a function that can hit such a failure
// returns a Status, and its caller checks it before proceeding on an
// assumed-valid value. Status is [[nodiscard]] so an accidentally-ignored
// failure is a compiler warning, not a silent bug.
//
// Relationship to the older idioms: some already-correct code signals failure
// with `bool` + a human-readable `std::string& error` (netlistgen's cli_config)
// or an `int` process exit code (runCliFromFile). Those are grandfathered and
// equally valid "explicit return-value" forms; a bool+message maps trivially
// onto a Status, and a Status maps onto an exit code at the CLI boundary. New
// code should prefer Status.
//
// This is layer 1 of the two-layer contract. Layer 2 is the top-level
// try/catch in every main() (see CLAUDE.md), the backstop for anything layer 1
// misses — including OpenROAD's utl::Logger::error(), which THROWS
// std::runtime_error at the pinned SHA (see support/logging.h). eda-lab's own
// APIs never throw; they return a Status.
//
// Header-only, like support/cli.h and support/logging.h.

#pragma once

#include <string>
#include <utility>

namespace eda {

// Coarse failure category, so callers can branch on the kind of failure without
// parsing the message string. Extend this enum as new failure modes appear;
// the message carries the human-readable specifics.
enum class ErrorCode
{
  Ok,
  FileNotFound,       // an input path does not exist / cannot be opened
  ParseError,         // malformed input (JSON, LEF, DEF, ...)
  InvalidConfig,      // input parsed but violates a required rule
  LefLoadFailed,      // a LEF library failed to load
  DefLoadFailed,      // a DEF design failed to load
  GenerationFailed,   // netlist generation rejected the spec
  ValidationFailed,   // a produced artifact failed well-formedness checks
  OutputWriteFailed,  // an output file could not be written
};

// The value every failure-capable eda-lab API returns. Default-constructed is
// success. [[nodiscard]] so a caller that forgets to check it gets a warning.
struct [[nodiscard]] Status
{
  ErrorCode code = ErrorCode::Ok;
  std::string message;

  bool ok() const { return code == ErrorCode::Ok; }
};

// Success.
inline Status okStatus()
{
  return {};
}

// Failure with a category and a human-readable message.
inline Status makeError(ErrorCode code, std::string message)
{
  return {code, std::move(message)};
}

}  // namespace eda
