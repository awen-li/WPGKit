#pragma once

#include "Andersen.h"
#include "CallGraphPTA.h"
#include "DataFlowFacts.h"

namespace wpgkit {

struct DataFlowInput final {
  BodyId body;
  FunctionDataFlow *facts;
};

class DataFlowBuilder final {
public:
  // Borrow inputs only during construction. The returned graph owns all records
  // and retains neither the solver nor a copy of its points-to relation.
  // consume releases input records as each construction stage finishes.
  static std::shared_ptr<const DataFlowGraph>
  build(const CallGraphResult &program,
        std::span<const DataFlowInput> functions, const Andersen &pta,
        bool consume = false);
};

} // namespace wpgkit
