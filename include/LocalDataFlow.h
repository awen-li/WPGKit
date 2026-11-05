#pragma once

#include "Andersen.h"
#include "DataFlowFacts.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include <map>
#include <optional>
#include <memory>
#include <set>

namespace clang {
class CFG;
class FunctionDecl;
class ValueDecl;
class VarDecl;
class DeclRefExpr;
class Stmt;
class ParentMap;
} // namespace clang

namespace wpgkit {

// One variable inventory and sparse reaching-definition computation for PTA
// and the DFG. Scalars need no points-to storage. AST lookups live only during
// extraction; consumers retain numeric definitions, joins and use links.
class LocalDataFlow final {
public:
  // A single reaching definition can serve both analyses. Scalars have only
  // a data node; pointer-only extraction leaves data empty.
  struct Definition final {
    std::optional<Andersen::Node> pointer;
    std::optional<std::uint32_t> data;
  };

  LocalDataFlow(
      const clang::FunctionDecl &function, clang::CFG *cfg, Andersen &pta,
      llvm::function_ref<Andersen::Node(const clang::ValueDecl *)> storage,
      FunctionDataFlow *data = nullptr, BodyId body = {});
  ~LocalDataFlow();
  Definition read(const clang::DeclRefExpr *expr) const;
  Definition write(const clang::DeclRefExpr *expr) const;
  Definition initialize(const clang::VarDecl *decl) const;
  std::size_t variables() const;
  std::size_t fallbacks() const;
  std::optional<CFGPosition> position(const clang::Stmt *) const;

  bool refines(const clang::VarDecl *variable) const {
    return localVariables_.contains(variable);
  }

private:
  class Builder;
  using Node = Andersen::Node;
  std::map<const clang::DeclRefExpr *, Definition> reads_, writes_;
  std::map<const clang::VarDecl *, Definition> initializers_;
  std::unique_ptr<clang::ParentMap> parents_;
  std::map<const clang::Stmt *, CFGPosition> positions_;
  std::set<const clang::VarDecl *> localVariables_;
  std::size_t refined_ = 0, fallback_ = 0;
};

} // namespace wpgkit
