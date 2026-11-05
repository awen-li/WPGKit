#pragma once

#include "DataFlowGraph.h"
#include <limits>

namespace wpgkit {

// Narrow extraction records. Node indices are zero-based within this function;
// pointer indices refer to the existing PTA. No AST objects or language IR.
inline constexpr std::uint32_t NoDataIndex =
    std::numeric_limits<std::uint32_t>::max();

struct MemoryAccess final {
  std::uint32_t node, pointer;
  bool indirect, write;
};

struct DataCall final {
  CallSiteId site;
  std::uint32_t result;
  std::vector<std::uint32_t> arguments;
  std::vector<std::uint32_t> pointers;  // NoDataIndex for nonpointer arguments.
  std::uint32_t receiver = NoDataIndex; // PTA pointer for external effects.
  std::uint32_t receiverValue =
      NoDataIndex; // Local DFG node, separate from args.
};

struct FunctionDataFlow final {
  std::uint32_t addNode(DataNode node) {
    const auto index = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back(node);
    return index;
  }

  std::vector<DataNode> nodes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> values;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> uses;
  std::vector<MemoryAccess> memory;
  std::vector<std::uint32_t> parameters, returns;
  std::uint32_t receiver = NoDataIndex; // Local DFG node for implicit this.
  std::vector<DataCall> calls;
  std::vector<DataGap> gaps;
};

} // namespace wpgkit
