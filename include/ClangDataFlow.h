#pragma once

#include "LocalDataFlow.h"
#include <functional>
#include <set>

namespace clang {
class Expr;
class BinaryOperator;
class UnaryOperator;
} // namespace clang

namespace wpgkit {

// Observes the existing body visitor. It never walks a body independently or
// computes reaching definitions. All AST-keyed state dies with that body.
class ClangDataFlow final {
public:
  using Pointer = std::function<Andersen::Node(const clang::Expr *)>;
  using Storage = std::function<Andersen::Node(const clang::ValueDecl *)>;
  ClangDataFlow(FunctionDataFlow &facts, const LocalDataFlow &locals,
                BodyId body, Pointer pointer, Storage storage, Pointer address,
                Storage capture);
  void statement(const clang::Stmt *statement);
  void variable(const clang::VarDecl *variable);
  void parameters(const clang::FunctionDecl &function);
  void field(std::uint32_t address, const clang::Expr *initializer);
  void copyField(std::uint32_t source, std::uint32_t destination,
                 const clang::Expr *expression);
  [[nodiscard]] std::span<const std::pair<std::uint32_t, std::uint32_t>>
  copiedFields(const clang::Expr *) const;
  void assignment(const clang::BinaryOperator *assignment);
  void increment(const clang::UnaryOperator *expression);
  void call(const clang::Expr *expression, CallSiteId site,
            std::span<const clang::Expr *const> arguments,
            std::vector<std::uint32_t> pointers, std::uint32_t receiver,
            const clang::FunctionDecl *callee);
  void destructor(CallSiteId site, CFGPosition position,
                  const clang::VarDecl *object = nullptr);
  void unknown();
  // Reuse the existing expression node when a downstream fact consumer needs
  // to attach an observation. This does not repeat reaching-definition work.
  [[nodiscard]] std::uint32_t
  expressionNode(const clang::Expr *expression) const;
  [[nodiscard]] std::uint32_t statementNode(const clang::Stmt *statement) const;
  [[nodiscard]] const DataCall *callFacts(CallSiteId site) const;
  [[nodiscard]] std::uint32_t
  variableNode(const clang::VarDecl *variable) const;
  [[nodiscard]] std::uint32_t fieldNode(const clang::Expr *initializer) const;

private:
  std::uint32_t node(const clang::Stmt *statement);
  void memory(const clang::Expr *expression, std::uint32_t event, bool write);
  void read(const clang::DeclRefExpr *expression, std::uint32_t event);
  void valueEdge(const clang::Stmt *source, std::uint32_t target);
  void destructorReceiver(const clang::Expr *expression, DataCall &call);
  FunctionDataFlow &facts_;
  const LocalDataFlow &locals_;
  const clang::FunctionDecl *function_ =
      nullptr; // Borrowed for this body only.
  BodyId body_;
  Pointer pointer_;
  Pointer address_;
  Storage storage_;
  Storage capture_;
  std::map<const clang::VarDecl *, std::uint32_t> captures_;
  std::map<const clang::Stmt *, std::uint32_t> nodes_;
  std::set<const clang::Stmt *> observed_;
  std::map<CallSiteId, std::size_t> calls_;
  std::map<const clang::Expr *, std::uint32_t> fields_;
  std::map<const clang::Expr *,
           std::vector<std::pair<std::uint32_t, std::uint32_t>>>
      copies_;
};

} // namespace wpgkit
