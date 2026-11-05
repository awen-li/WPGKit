#pragma once

#include "FunctionCFG.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

namespace clang {
class ASTContext;
class CFG;
class CFGElement;
} // namespace clang

namespace wpgkit {

class ClangCFGLifter final {
public:
  // Invokes capture synchronously with the event's point ID, block and offset.
  // CFG elements borrow temporary Clang storage.
  llvm::Expected<FunctionCFG>
  lift(const clang::FunctionDecl &function, clang::ASTContext &context,
       FunctionId id,
       llvm::function_ref<void(const clang::CFGElement &, PointId, BlockId,
                               std::uint64_t)>
           capture = {},
       // Runs before capture; null if CFG construction failed.
       llvm::function_ref<void(clang::CFG *)> prepareFacts = {}) const;
};

} // namespace wpgkit
