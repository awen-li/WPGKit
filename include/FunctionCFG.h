#pragma once

#include "Ids.h"
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace wpgkit {

using BlockId = Id<class BlockIdTag>;

// Clang CFG events retained in execution order. Their semantic operands are
// extracted separately; this graph never borrows AST or Clang CFG storage.
enum class CFGEventKind : std::uint8_t {
  Initializer,
  ScopeBegin,
  ScopeEnd,
  NewAllocator,
  LifetimeEnds,
  LoopExit,
  Statement,
  Constructor,
  RecordTypedCall,
  AutomaticObjectDtor,
  DeleteDtor,
  BaseDtor,
  MemberDtor,
  TemporaryDtor,
  CleanupFunction
};

enum class CFGTerminatorKind : std::uint8_t {
  StatementBranch,
  TemporaryDtorsBranch,
  VirtualBaseBranch
};

struct CFGEvent final {
  PointId point;
  CFGEventKind kind;
};

// A successor slot may be pruned by Clang yet still name an alternate block.
// Keeping both targets prevents the lifter from silently discarding that edge.
struct CFGSuccessor final {
  std::optional<BlockId> reachable;
  std::optional<BlockId> pruned;
};

struct CFGBlock final {
  BlockId id;
  std::vector<CFGEvent> events;
  std::optional<CFGTerminatorKind> terminator;
  std::vector<CFGSuccessor> successors;
};

class FunctionCFG final {
public:
  FunctionCFG(FunctionId function, BlockId entry, BlockId exit,
              std::vector<CFGBlock> blocks)
      : function_(function), entry_(entry), exit_(exit),
        blocks_(std::move(blocks)) {}

  [[nodiscard]] FunctionId function() const noexcept {
    return function_;
  }

  [[nodiscard]] BlockId entry() const noexcept {
    return entry_;
  }

  [[nodiscard]] BlockId exit() const noexcept {
    return exit_;
  }

  [[nodiscard]] const std::vector<CFGBlock> &blocks() const noexcept {
    return blocks_;
  }

  [[nodiscard]] const CFGBlock *block(BlockId id) const noexcept {
    const auto index = id.value();
    if (index == 0 || index > blocks_.size())
      return nullptr;
    return &blocks_[index - 1];
  }

private:
  FunctionId function_;
  BlockId entry_;
  BlockId exit_;
  std::vector<CFGBlock> blocks_;
};

} // namespace wpgkit
