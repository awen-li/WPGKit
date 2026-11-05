#pragma once

#include "FunctionCFG.h"

namespace wpgkit {

using BodyId = Id<class BodyIdTag>;
using CallSiteId = Id<class CallSiteIdTag>;
using CallConnectionId = Id<class CallConnectionIdTag>;

// Boundary before event[offset]; offset == events.size() is the block end.
struct CFGPosition final {
  BodyId body;
  BlockId block;
  std::uint64_t offset = 0;
  auto operator<=>(const CFGPosition &) const = default;
};

} // namespace wpgkit
