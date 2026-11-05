#include "ClangCFGLifter.h"
#include "Clang18.h"
#include "clang/Analysis/CFG.h"
#include "clang/AST/ExprCXX.h"

namespace wpgkit {
namespace {

BlockId blockId(const clang::CFGBlock &block) {
  return BlockId{static_cast<std::uint64_t>(block.getBlockID()) + 1};
}

std::optional<CFGEventKind> eventKind(const clang::CFGElement &element) {
  if (const auto statement = element.getAs<clang::CFGStmt>())
    if (const auto *call =
            llvm::dyn_cast<clang::CallExpr>(statement->getStmt());
        call && llvm::isa<clang::CXXPseudoDestructorExpr>(
                    call->getCallee()->IgnoreParenImpCasts()))
      return CFGEventKind::LifetimeEnds;
  using K = clang::CFGElement;
  switch (element.getKind()) {
  case K::Initializer:
    return CFGEventKind::Initializer;
  case K::ScopeBegin:
    return CFGEventKind::ScopeBegin;
  case K::ScopeEnd:
    return CFGEventKind::ScopeEnd;
  case K::NewAllocator:
    return CFGEventKind::NewAllocator;
  case K::LifetimeEnds:
    return CFGEventKind::LifetimeEnds;
  case K::LoopExit:
    return CFGEventKind::LoopExit;
  case K::Statement:
    return CFGEventKind::Statement;
  case K::Constructor:
    return CFGEventKind::Constructor;
  case K::CXXRecordTypedCall:
    return CFGEventKind::RecordTypedCall;
  case K::AutomaticObjectDtor:
    return CFGEventKind::AutomaticObjectDtor;
  case K::DeleteDtor:
    return CFGEventKind::DeleteDtor;
  case K::BaseDtor:
    return CFGEventKind::BaseDtor;
  case K::MemberDtor:
    return CFGEventKind::MemberDtor;
  case K::TemporaryDtor:
    return CFGEventKind::TemporaryDtor;
  case K::CleanupFunction:
    return CFGEventKind::CleanupFunction;
  }
  return std::nullopt;
}

std::optional<CFGTerminatorKind>
terminatorKind(clang::CFGTerminator terminator) {
  if (!terminator.isValid())
    return std::nullopt;
  switch (terminator.getKind()) {
  case clang::CFGTerminator::StmtBranch:
    return CFGTerminatorKind::StatementBranch;
  case clang::CFGTerminator::TemporaryDtorsBranch:
    return CFGTerminatorKind::TemporaryDtorsBranch;
  case clang::CFGTerminator::VirtualBaseBranch:
    return CFGTerminatorKind::VirtualBaseBranch;
  }
  return std::nullopt;
}

llvm::Error failure(const char *message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "%s", message);
}

} // namespace

llvm::Expected<FunctionCFG> ClangCFGLifter::lift(
    const clang::FunctionDecl &function, clang::ASTContext &context,
    FunctionId id,
    llvm::function_ref<void(const clang::CFGElement &, PointId, BlockId,
                            std::uint64_t)>
        capture,
    llvm::function_ref<void(clang::CFG *)> prepareFacts) const {
  if (!id.isValid() || !function.hasBody())
    return failure("CFG lift requires a valid function ID and a body");

  clang::CFG::BuildOptions options;
  options.AddInitializers = true;
  options.AddImplicitDtors = true;
  options.AddTemporaryDtors = true;
  options.AddEHEdges = true;
  auto source =
      clang::CFG::buildCFG(&function, function.getBody(), &context, options);
  if (prepareFacts)
    prepareFacts(source.get());
  if (!source)
    return failure("Clang could not construct the function CFG");

  std::vector<CFGBlock> blocks(source->size());
  std::uint64_t nextPoint = 0;
  for (const clang::CFGBlock *original : *source) {
    const auto index = original->getBlockID();
    if (index >= blocks.size())
      return failure("Clang CFG block ID is out of range");
    auto &block = blocks[index];
    block.id = blockId(*original);
    for (const clang::CFGElement &element : *original) {
      const auto kind = eventKind(element);
      if (!kind)
        return failure("Clang CFG contains an unsupported event kind");
      const PointId point{id, nextPoint++};
      block.events.push_back({point, *kind});
      if (capture)
        capture(element, point, block.id, block.events.size() - 1);
    }
    const auto terminator = original->getTerminator();
    block.terminator = terminatorKind(terminator);
    if (terminator.isValid() && !block.terminator)
      return failure("Clang CFG contains an unsupported terminator kind");
    for (const auto &successor : original->succs()) {
      CFGSuccessor edge;
      if (const auto *target = successor.getReachableBlock())
        edge.reachable = blockId(*target);
      if (const auto *target = successor.getPossiblyUnreachableBlock())
        edge.pruned = blockId(*target);
      block.successors.push_back(edge);
    }
  }
  for (const auto &block : blocks)
    if (!block.id.isValid())
      return failure("Clang CFG has a missing block ID");
  return FunctionCFG{id, blockId(source->getEntry()),
                     blockId(source->getExit()), std::move(blocks)};
}

} // namespace wpgkit
