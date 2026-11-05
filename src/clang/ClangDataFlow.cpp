#include "ClangDataFlow.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"

namespace wpgkit {

namespace {
const clang::Stmt *canonical(const clang::Stmt *statement) {
  if (const auto *expr = llvm::dyn_cast_or_null<clang::Expr>(statement))
    return expr->IgnoreParenImpCasts();
  return statement;
}
} // namespace

ClangDataFlow::ClangDataFlow(FunctionDataFlow &facts,
                             const LocalDataFlow &locals, BodyId body,
                             Pointer pointer, Storage storage)
    : facts_(facts), locals_(locals), body_(body), pointer_(std::move(pointer)),
      storage_(std::move(storage)) {}

std::uint32_t
ClangDataFlow::expressionNode(const clang::Expr *expression) const {
  return statementNode(expression);
}

std::uint32_t ClangDataFlow::statementNode(const clang::Stmt *statement) const {
  const auto found = nodes_.find(canonical(statement));
  return found == nodes_.end() ? NoDataIndex : found->second;
}

const DataCall *ClangDataFlow::callFacts(CallSiteId site) const {
  const auto found = calls_.find(site);
  return found == calls_.end() ? nullptr : &facts_.calls[found->second];
}

std::uint32_t
ClangDataFlow::variableNode(const clang::VarDecl *variable) const {
  if (const auto *parameter = llvm::dyn_cast<clang::ParmVarDecl>(variable)) {
    if (parameter->getDeclContext() != function_)
      return NoDataIndex;
    const auto index = parameter->getFunctionScopeIndex();
    return index < facts_.parameters.size() ? facts_.parameters[index]
                                            : NoDataIndex;
  }
  if (!variable->hasLocalStorage())
    return NoDataIndex;
  if (const auto definition = locals_.initialize(variable).data)
    return *definition;
  return expressionNode(variable->getInit());
}

void ClangDataFlow::parameters(const clang::FunctionDecl &function) {
  function_ = &function;
  for (const auto *param : function.parameters())
    if (!locals_.refines(param))
      facts_.memory.push_back(
          {facts_.parameters.at(param->getFunctionScopeIndex()),
           storage_(param), false, true});
}

std::uint32_t ClangDataFlow::node(const clang::Stmt *statement) {
  statement = canonical(statement);
  auto [found, added] = nodes_.try_emplace(statement);
  if (added) {
    const auto position = locals_.position(statement);
    found->second = facts_.addNode(
        {position.value_or(CFGPosition{body_, {}, 0}), DataNodeKind::Value, 0});
    if (!position)
      facts_.gaps.push_back(
          {{body_, {}, 0}, DataGapKind::UnsupportedEffect, {}});
  }
  return found->second;
}

void ClangDataFlow::valueEdge(const clang::Stmt *source, std::uint32_t target) {
  if (!source)
    return;
  const auto from = node(source);
  if (from != target)
    facts_.values.emplace_back(from, target);
}

void ClangDataFlow::read(const clang::DeclRefExpr *expression,
                         std::uint32_t event) {
  if (const auto definition = locals_.read(expression).data)
    facts_.uses.emplace_back(*definition, event);
  else if (!locals_.write(expression).data &&
           llvm::isa<clang::VarDecl>(expression->getDecl()))
    memory(expression, event, false);
}

void ClangDataFlow::memory(const clang::Expr *expression, std::uint32_t event,
                           bool write) {
  expression = expression->IgnoreParenImpCasts();
  std::uint32_t pointer = NoDataIndex;
  bool indirect = true;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
    pointer = storage_(ref->getDecl());
    indirect = ref->getDecl()->getType()->isReferenceType();
  } else if (const auto *member =
                 llvm::dyn_cast<clang::MemberExpr>(expression)) {
    if (llvm::isa<clang::FieldDecl>(member->getMemberDecl())) {
      pointer = storage_(member->getMemberDecl());
      indirect = member->getMemberDecl()->getType()->isReferenceType();
    }
  } else if (const auto *unary =
                 llvm::dyn_cast<clang::UnaryOperator>(expression);
             unary && unary->getOpcode() == clang::UO_Deref) {
    pointer = pointer_(unary->getSubExpr());
  } else if (const auto *subscript =
                 llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
    pointer = pointer_(subscript->getBase());
  }
  facts_.memory.push_back({event, pointer, indirect, write});
}

void ClangDataFlow::statement(const clang::Stmt *statement) {
  if (!llvm::isa<clang::Expr, clang::ReturnStmt>(statement))
    return;
  statement = canonical(statement);
  if (!observed_.insert(statement).second)
    return;
  const auto event = node(statement);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(statement))
    read(ref, event);
  if (llvm::isa<clang::ArraySubscriptExpr>(statement))
    memory(llvm::cast<clang::Expr>(statement), event, false);
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(statement);
      member && llvm::isa<clang::FieldDecl>(member->getMemberDecl()))
    memory(member, event, false);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(statement);
      unary && unary->getOpcode() == clang::UO_Deref &&
      !unary->getType()->isFunctionType())
    memory(unary, event, false);
  if (llvm::isa<clang::ReturnStmt>(statement)) {
    facts_.nodes[event].kind = DataNodeKind::Return;
    facts_.returns.push_back(event);
  }
  if (llvm::isa<clang::CXXThisExpr>(statement) &&
      facts_.receiver != NoDataIndex) {
    facts_.uses.emplace_back(facts_.receiver, event);
    return;
  }
  const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(statement);
  if (llvm::isa<clang::CXXThisExpr, clang::CXXDefaultArgExpr,
                clang::CXXDefaultInitExpr, clang::VAArgExpr, clang::StmtExpr>(
          statement) ||
      (lambda && lambda->capture_size())) {
    facts_.nodes[event].kind = DataNodeKind::Unknown;
    facts_.gaps.push_back(
        {facts_.nodes[event].position, DataGapKind::UnsupportedEffect, {}});
  }
  if (const auto *opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(statement))
    valueEdge(opaque->getSourceExpr(), event);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement);
      call && !call->getDirectCallee())
    valueEdge(call->getCallee(), event);
  // Calls receive arguments through actual/formal or external-summary edges.
  // Unevaluated operands and nested lambda bodies are never visited here.
  if (llvm::isa<clang::CallExpr, clang::CXXConstructExpr, clang::LambdaExpr,
                clang::UnaryExprOrTypeTraitExpr, clang::CXXNoexceptExpr,
                clang::CXXTypeidExpr, clang::TypeTraitExpr>(statement))
    return;
  for (const auto *child : statement->children())
    valueEdge(child, event);
}

void ClangDataFlow::variable(const clang::VarDecl *variable) {
  if (!variable->hasInit())
    return;
  const auto value = node(variable->getInit());
  if (const auto definition = locals_.initialize(variable).data) {
    facts_.values.emplace_back(value, *definition);
  } else {
    // Reference initialization stores an address in the reference's storage;
    // later accesses follow the same PTA identity to the referent.
    facts_.memory.push_back({value, storage_(variable), false, true});
  }
}

void ClangDataFlow::field(const clang::ValueDecl *field,
                          const clang::Expr *initializer) {
  facts_.memory.push_back({node(initializer), storage_(field), false, true});
}

void ClangDataFlow::assignment(const clang::BinaryOperator *assignment) {
  if (!assignment->isAssignmentOp())
    return;
  const auto event = node(assignment);
  const auto *left = assignment->getLHS()->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(left))
    if (const auto definition = locals_.write(ref).data) {
      facts_.values.emplace_back(event, *definition);
      return;
    }
  memory(left, event, true);
}

void ClangDataFlow::increment(const clang::UnaryOperator *expression) {
  if (expression->isIncrementDecrementOp())
    memory(expression->getSubExpr(), node(expression), true);
}

void ClangDataFlow::call(const clang::Expr *expression, CallSiteId site,
                         std::span<const clang::Expr *const> arguments,
                         std::vector<std::uint32_t> pointers,
                         std::uint32_t receiver,
                         const clang::FunctionDecl *callee) {
  if (!calls_.emplace(site, facts_.calls.size()).second)
    return;
  auto result = node(expression);
  const bool isDestructor =
      llvm::isa_and_nonnull<clang::CXXDestructorDecl>(callee);
  if (isDestructor) {
    // Destruction has no expression result. Keep its effect node separate from
    // the temporary's value or the delete expression's deallocation call.
    const auto position = facts_.nodes[result].position;
    result = facts_.addNode({position, DataNodeKind::CallResult, 0});
  }
  facts_.nodes[result].kind = DataNodeKind::CallResult;
  DataCall call{site, result, {}, std::move(pointers), receiver};
  for (const auto *arg : arguments) {
    const auto argument =
        facts_.addNode({facts_.nodes[result].position,
                        arg ? DataNodeKind::Argument : DataNodeKind::Unknown,
                        static_cast<unsigned>(call.arguments.size())});
    if (!arg)
      facts_.gaps.push_back({facts_.nodes[result].position,
                             DataGapKind::UnsupportedEffect, site});
    valueEdge(arg, argument);
    call.arguments.push_back(argument);
  }
  if (isDestructor)
    destructorReceiver(expression, call);
  facts_.calls.push_back(std::move(call));
}

void ClangDataFlow::destructorReceiver(const clang::Expr *expression,
                                       DataCall &call) {
  const clang::Expr *object = nullptr;
  if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expression))
    object = member->getImplicitObjectArgument();
  else if (const auto *temporary =
               llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression))
    object = temporary->getSubExpr();
  else if (const auto *deleted =
               llvm::dyn_cast<clang::CXXDeleteExpr>(expression))
    object = deleted->getArgument();
  if (!object)
    return;
  call.receiverValue = facts_.addNode({facts_.nodes[call.result].position,
                                       DataNodeKind::Receiver, NoDataIndex});
  valueEdge(object, call.receiverValue);
}

void ClangDataFlow::destructor(CallSiteId site, CFGPosition position,
                               const clang::VarDecl *object) {
  auto found = calls_.find(site);
  if (found == calls_.end()) {
    if (!object)
      return;
    const auto result = facts_.addNode({position, DataNodeKind::CallResult, 0});
    // The implicit operand is the object's address, not a load of its contents.
    const auto receiver =
        facts_.addNode({position, DataNodeKind::Receiver, storage_(object)});
    found = calls_.emplace(site, facts_.calls.size()).first;
    facts_.calls.push_back({site, result, {}, {}, NoDataIndex, receiver});
  }
  const auto &call = facts_.calls[found->second];
  facts_.nodes[call.result].position = position;
  if (call.receiverValue != NoDataIndex)
    facts_.nodes[call.receiverValue].position = position;
  // One call fact may have several exit occurrences. Their distinct inter-CFG
  // connection IDs carry those positions; these nodes share the operand value.
}

void ClangDataFlow::unknown() {
  const CFGPosition position{body_, {}, 0};
  const auto event = facts_.addNode({position, DataNodeKind::Unknown, 0});
  facts_.memory.push_back({event, NoDataIndex, true, false});
  facts_.memory.push_back({event, NoDataIndex, true, true});
  facts_.gaps.push_back({position, DataGapKind::UnsupportedEffect, {}});
}

} // namespace wpgkit
