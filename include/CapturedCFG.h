#pragma once

#include "GraphMetadata.h"
#include "GraphIds.h"
#include <optional>
#include <vector>

namespace wpgkit {
using SourceUnitId = Id<class SourceUnitIdTag>;

// One representative body per function. Immutable snapshots share this
// topology, never compiler storage. Call facts may map to several CFG
// occurrences (e.g. a local destructor on different exits). A failed lift
// retains an empty graph.
struct CapturedCFG final {
  struct CallPoint {
    CallSiteId call;
    PointId point;
  };

  FunctionId function;
  SourceUnitId unit;
  SourceLocation definition;
  std::optional<FunctionCFG> graph;
  std::vector<CallPoint> calls;
  std::vector<PointId>
      unmodeledCalls; // CFG calls without an extracted call fact.
  std::vector<CallSiteId>
      unmappedCalls; // Extracted calls without a CFG occurrence.
  // Clang 18 can emit statements from noexcept/unevaluated builtin operands.
  // These points have no runtime effects and must be skipped by graph clients.
  std::vector<PointId> unevaluatedPoints;
};

} // namespace wpgkit
