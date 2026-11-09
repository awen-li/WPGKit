#include "LocalDataFlow.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/CFG.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace wpgkit {
namespace {
using Node = Andersen::Node;

struct Position {
  unsigned block;
  unsigned offset;
};

// Only simple, independently sequenced writes qualify. Other lvalue uses
// (address taking, reference binding, captures, increments, inline assembly)
// exclude the variable, including uses before or after the first read.
class Candidates final : public clang::RecursiveASTVisitor<Candidates> {
public:
  Candidates(clang::ParentMap &parents, bool scalars,
             clang::ASTContext &context)
      : parents_(parents), scalars_(scalars), context_(context) {}

  bool TraverseTypeLoc(clang::TypeLoc type) {
    if (!type.isNull() && type.getType()->isVariablyModifiedType())
      opaque = true;
    return true;
  }

  bool TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr) {
    if (expr->getTypeOfArgument()->isVariablyModifiedType())
      opaque = true;
    return true;
  }

  bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *) {
    return true;
  }

  bool TraverseRequiresExpr(clang::RequiresExpr *) {
    return true;
  }

  bool TraverseTypeTraitExpr(clang::TypeTraitExpr *) {
    return true;
  }

  bool TraverseExpressionTraitExpr(clang::ExpressionTraitExpr *) {
    return true;
  }

  bool TraverseCXXTypeidExpr(clang::CXXTypeidExpr *expr) {
    return expr->isTypeOperand() || !expr->isPotentiallyEvaluated() ||
           TraverseStmt(expr->getExprOperand());
  }

  bool TraverseCallExpr(clang::CallExpr *expr) {
    return expr->isUnevaluatedBuiltinCall(context_) ||
           clang::RecursiveASTVisitor<Candidates>::TraverseCallExpr(expr);
  }

  bool TraverseChooseExpr(clang::ChooseExpr *expr) {
    return expr->isConditionDependent() ||
           TraverseStmt(expr->getChosenSubExpr());
  }

  bool TraverseGenericSelectionExpr(clang::GenericSelectionExpr *expr) {
    return expr->isResultDependent() || TraverseStmt(expr->getResultExpr());
  }

  bool VisitVarDecl(clang::VarDecl *decl) {
    if (eligible(decl))
      variables.insert(decl);
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    const auto *decl = llvm::dyn_cast<clang::VarDecl>(expr->getDecl());
    if (!decl || !eligible(decl))
      return true;
    const clang::Stmt *child = expr;
    auto *parent = parents_.getParent(expr);
    while (llvm::isa_and_nonnull<clang::ParenExpr>(parent)) {
      child = parent;
      parent = parents_.getParent(parent);
    }
    if (const auto *cast =
            llvm::dyn_cast_or_null<clang::ImplicitCastExpr>(parent);
        cast && cast->getCastKind() == clang::CK_LValueToRValue) {
      reads[decl].push_back(expr);
      return true;
    }
    const auto *assign = llvm::dyn_cast_or_null<clang::BinaryOperator>(parent);
    const auto *unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(parent);
    const bool update = decl->getType()->isIntegerType() &&
                        ((assign && assign->isCompoundAssignmentOp() &&
                          assign->getLHS() == child) ||
                         (unary && unary->isIncrementDecrementOp()));
    if (update && sequenced(parent)) {
      reads[decl].push_back(expr);
      writes[decl].push_back(expr);
      return true;
    }
    if (assign && assign->getOpcode() == clang::BO_Assign &&
        assign->getLHS() == child && sequenced(assign)) {
      writes[decl].push_back(expr);
      return true;
    }
    excluded.insert(decl);
    return true;
  }

  bool TraverseLambdaExpr(clang::LambdaExpr *expr) {
    for (const auto &capture : expr->captures())
      if (capture.capturesVariable())
        if (const auto *decl =
                llvm::dyn_cast<clang::VarDecl>(capture.getCapturedVar()))
          excluded.insert(decl);
    // Capture initializers execute here; the lambda body has its own CFG.
    for (auto *init : expr->capture_inits())
      TraverseStmt(init);
    return true;
  }

  bool VisitGCCAsmStmt(clang::GCCAsmStmt *) {
    opaque = true;
    return true;
  }

  bool VisitMSAsmStmt(clang::MSAsmStmt *) {
    opaque = true;
    return true;
  }

  bool VisitCoroutineBodyStmt(clang::CoroutineBodyStmt *) {
    opaque = true;
    return true;
  }

  std::set<const clang::VarDecl *> variables, excluded;
  std::map<const clang::VarDecl *, std::vector<const clang::DeclRefExpr *>>
      reads, writes;
  bool opaque = false;

private:
  bool eligible(const clang::VarDecl *decl) const {
    return decl->hasLocalStorage() &&
           (decl->getType()->isPointerType() ||
            (scalars_ && decl->getType()->isScalarType())) &&
           !decl->getType().isVolatileQualified();
  }

  bool sequenced(const clang::Stmt *stmt) const {
    const auto *parent = parents_.getParent(stmt);
    while (llvm::isa_and_nonnull<clang::ParenExpr, clang::ImplicitCastExpr,
                                 clang::ExprWithCleanups>(parent)) {
      stmt = parent;
      parent = parents_.getParent(parent);
    }
    if (llvm::isa_and_nonnull<clang::CompoundStmt, clang::DeclStmt,
                              clang::ReturnStmt>(parent))
      return true;
    if (const auto *branch = llvm::dyn_cast_or_null<clang::IfStmt>(parent))
      return stmt == branch->getThen() || stmt == branch->getElse();
    if (const auto *loop = llvm::dyn_cast_or_null<clang::WhileStmt>(parent))
      return stmt == loop->getBody();
    if (const auto *loop = llvm::dyn_cast_or_null<clang::DoStmt>(parent))
      return stmt == loop->getBody();
    if (const auto *loop = llvm::dyn_cast_or_null<clang::ForStmt>(parent))
      return stmt == loop->getBody() || stmt == loop->getInc() ||
             stmt == loop->getInit();
    return false;
  }

  clang::ParentMap &parents_;
  bool scalars_;
  clang::ASTContext &context_;
};
} // namespace

class LocalDataFlow::Builder final {
public:
  Builder(LocalDataFlow &flow, const clang::FunctionDecl &function,
          clang::CFG *cfg, Andersen &pta,
          llvm::function_ref<Node(const clang::ValueDecl *)> storage,
          FunctionDataFlow *data, BodyId body)
      : flow_(flow), function_(function), cfg_(cfg), pta_(pta),
        storage_(storage), data_(data), body_(body),
        candidates_(*flow.parents_, data != nullptr, function.getASTContext()),
        nextVariable_(function.getNumParams()) {}

  void run() {
    inventory();
    locations();
    for (const auto *decl : candidates_.variables)
      variable(decl);
  }

private:
  using Token = std::uint64_t;
  static constexpr Token Entry = Token{1} << 32;

  struct Event {
    Position position;
    const clang::DeclRefExpr *expression;
    bool definition;
  };

  struct Join {
    std::vector<Token> inputs;
    Token representative;
  };

  CFGPosition entryPosition() const {
    return {body_,
            cfg_ ? BlockId{cfg_->getEntry().getBlockID() + 1u} : BlockId{}, 0};
  }

  std::optional<unsigned> addData(CFGPosition position, DataNodeKind kind,
                                  unsigned slot) {
    if (!data_)
      return std::nullopt;
    return data_->addNode({position, kind, slot});
  }

  void inventory() {
    if (data_ && llvm::isa<clang::CXXDestructorDecl>(function_))
      data_->receiver =
          *addData(entryPosition(), DataNodeKind::Receiver, NoDataIndex);
    for (auto *param : function_.parameters()) {
      candidates_.VisitVarDecl(param);
      if (data_) {
        const auto id = addData(entryPosition(), DataNodeKind::Parameter,
                                param->getFunctionScopeIndex());
        parameters_.emplace(param, *id);
        data_->parameters.push_back(*id);
      }
    }
    candidates_.TraverseStmt(function_.getBody());
    if (const auto *ctor =
            llvm::dyn_cast<clang::CXXConstructorDecl>(&function_))
      for (const auto *init : ctor->inits()) {
        flow_.parents_->addStmt(init->getInit());
        candidates_.TraverseStmt(init->getInit());
      }
    if (candidates_.opaque)
      candidates_.variables.clear();
    for (const auto *decl : candidates_.excluded)
      candidates_.variables.erase(decl);
    flow_.localVariables_ = candidates_.variables;
  }

  void locations() {
    predecessors_.resize(cfg_ ? cfg_->getNumBlockIDs() : 0);
    if (cfg_)
      for (const auto *block : *cfg_) {
        unsigned offset = 0;
        for (const auto &element : *block) {
          const CFGPosition position{body_, BlockId{block->getBlockID() + 1u},
                                     offset++};
          if (const auto stmt = element.getAs<clang::CFGStmt>()) {
            flow_.positions_.emplace(stmt->getStmt(), position);
            if (const auto *decls =
                    llvm::dyn_cast<clang::DeclStmt>(stmt->getStmt()))
              for (const auto *decl : decls->decls())
                if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl))
                  declarations_.emplace(variable, position);
          } else if (const auto init = element.getAs<clang::CFGInitializer>())
            flow_.positions_.emplace(init->getInitializer()->getInit(),
                                     position);
        }
        for (const auto &edge : block->succs()) {
          const auto *reachable = edge.getReachableBlock(),
                     *pruned = edge.getPossiblyUnreachableBlock();
          if (reachable)
            predecessors_[reachable->getBlockID()].push_back(
                block->getBlockID());
          if (pruned && pruned != reachable)
            predecessors_[pruned->getBlockID()].push_back(block->getBlockID());
        }
      }
    // Clang splits multi-declarations into synthetic CFG statements.
    for (const auto &[decl, position] : declarations_)
      if (decl->hasInit())
        flow_.positions_.try_emplace(decl->getInit(), position);
  }

  bool events() {
    bool mapped = cfg_ != nullptr;
    const auto append = [&](CFGPosition position,
                            const clang::DeclRefExpr *expr, bool write) {
      events_.push_back({{static_cast<unsigned>(position.block.value() - 1),
                          static_cast<unsigned>(position.offset)},
                         expr,
                         write});
    };
    if (!parameter_ && decl_->hasInit()) {
      const auto found = declarations_.find(decl_);
      if (found == declarations_.end())
        mapped = false;
      else
        append(found->second, nullptr, true);
    }
    for (const bool write : {false, true})
      for (const auto *expr :
           write ? candidates_.writes[decl_] : candidates_.reads[decl_]) {
        const clang::Stmt *stmt = expr;
        if (write) {
          stmt = flow_.parents_->getParent(stmt);
          while (llvm::isa_and_nonnull<clang::ParenExpr>(stmt))
            stmt = flow_.parents_->getParent(stmt);
        }
        if (const auto position = flow_.position(stmt))
          append(*position, expr, write);
        else
          mapped = false;
      }
    std::sort(events_.begin(), events_.end(), [](const auto &a, const auto &b) {
      return std::tie(a.position.block, a.position.offset, a.definition) <
             std::tie(b.position.block, b.position.offset, b.definition);
    });
    return mapped;
  }

  Node base() {
    if (!entryValue_) {
      entryValue_ = static_cast<Node>(definitions_.size());
      const auto pointer =
          pointer_ ? std::optional{storage_(decl_)} : std::nullopt;
      if (pointer_ && !parameter_)
        pta_.markUnknown(*pointer);
      const auto data =
          parameter_ && data_
              ? parameters_.at(decl_)
              : addData(entryPosition(), DataNodeKind::Unknown, variableId_);
      definitions_.push_back({pointer, data});
      if (data_ && !parameter_)
        data_->gaps.push_back(
            {entryPosition(), DataGapKind::UninitializedRead, {}});
    }
    return *entryValue_;
  }

  Node allocate(CFGPosition position, DataNodeKind kind) {
    const auto index = static_cast<Node>(definitions_.size());
    definitions_.push_back(
        {pointer_ ? std::optional{pta_.addNode()} : std::nullopt,
         addData(position, kind, variableId_)});
    return index;
  }

  void bindRead(const clang::DeclRefExpr *expr, Node node) {
    flow_.reads_.emplace(expr, definitions_.at(node));
  }

  void bindWrite(const clang::DeclRefExpr *expr, Node node) {
    const auto &definition = definitions_.at(node);
    if (expr)
      flow_.writes_.emplace(expr, definition);
    else
      flow_.initializers_.emplace(decl_, definition);
  }

  void fallback() {
    if (pointer_)
      ++flow_.fallback_;
    if (data_)
      data_->gaps.push_back(
          {entryPosition(), DataGapKind::UnsupportedEffect, {}});
    const auto node = base();
    if (!parameter_ && decl_->hasInit())
      bindWrite(nullptr, node);
    for (const auto *expr : candidates_.reads[decl_])
      bindRead(expr, node);
    for (const auto *expr : candidates_.writes[decl_])
      bindWrite(expr, node);
  }

  Token entry(unsigned block) {
    const auto token = Entry | block;
    if (joins_.try_emplace(block, Join{{}, token}).second)
      pending_.push_back(block);
    return token;
  }

  void define() {
    for (const auto &event : events_) {
      const auto block = event.position.block;
      if (event.definition) {
        const auto node =
            allocate({body_, BlockId{block + 1u}, event.position.offset},
                     DataNodeKind::Definition);
        last_.insert_or_assign(block, node);
        bindWrite(event.expression, node);
      } else {
        const auto found = last_.find(block);
        uses_.emplace_back(event.expression,
                           found == last_.end() ? entry(block) : found->second);
      }
    }
  }

  void collectInputs() {
    for (std::size_t i = 0; i < pending_.size(); ++i) {
      const auto block = pending_[i];
      auto &inputs = joins_.at(block).inputs;
      if (block == cfg_->getEntry().getBlockID() ||
          predecessors_[block].empty())
        inputs.push_back(base());
      for (const auto predecessor : predecessors_[block]) {
        const auto found = last_.find(predecessor);
        inputs.push_back(found == last_.end() ? entry(predecessor)
                                              : found->second);
      }
    }
  }

  Token canonical(Token token) {
    auto result = token;
    while (result >= Entry) {
      const auto next = joins_.at(static_cast<unsigned>(result)).representative;
      if (next == result)
        break;
      result = next;
    }
    while (token >= Entry && token != result) {
      auto &link = joins_.at(static_cast<unsigned>(token)).representative;
      token = std::exchange(link, result);
    }
    return result;
  }

  void simplify() {
    bool changed;
    do {
      changed = false;
      for (auto &[block, join] : joins_) {
        const auto self = Entry | block;
        if (join.representative != self)
          continue;
        std::optional<Token> only;
        bool different = false;
        for (const auto input : join.inputs) {
          const auto source = canonical(input);
          if (source == self)
            continue;
          if (only && *only != source) {
            different = true;
            break;
          }
          only = source;
        }
        if (!different) {
          join.representative = only ? *only : base();
          changed = true;
        }
      }
    } while (changed);
  }

  Node value(Token token) {
    token = canonical(token);
    if (token < Entry)
      return static_cast<Node>(token);
    auto [found, added] = materialized_.try_emplace(token);
    if (added) {
      found->second =
          allocate({body_, BlockId{static_cast<unsigned>(token) + 1u}, 0},
                   DataNodeKind::Join);
      remaining_.push_back(token);
    }
    return found->second;
  }

  void connectUses() {
    for (const auto &[expr, token] : uses_)
      bindRead(expr, value(token));
    for (std::size_t i = 0; i < remaining_.size(); ++i) {
      const auto token = remaining_[i];
      const auto destination = materialized_.at(token);
      std::set<Node> sources;
      for (const auto input : joins_.at(static_cast<unsigned>(token)).inputs)
        sources.insert(value(input));
      for (const auto source : sources)
        if (source != destination) {
          if (pointer_)
            pta_.copy(*definitions_.at(destination).pointer,
                      *definitions_.at(source).pointer);
          if (data_)
            data_->uses.emplace_back(*definitions_.at(source).data,
                                     *definitions_.at(destination).data);
        }
    }
  }

  void variable(const clang::VarDecl *decl) {
    decl_ = decl;
    const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(decl);
    variableId_ = param ? param->getFunctionScopeIndex() : nextVariable_++;
    parameter_ = param != nullptr;
    pointer_ = decl->getType()->isPointerType();
    if ((parameter_ || !decl->hasInit()) && candidates_.reads[decl].empty() &&
        candidates_.writes[decl].empty())
      return;
    definitions_.clear();
    events_.clear();
    joins_.clear();
    pending_.clear();
    last_.clear();
    uses_.clear();
    materialized_.clear();
    remaining_.clear();
    entryValue_.reset();
    if (!events()) {
      fallback();
      return;
    }
    if (events_.empty())
      return;
    if (pointer_)
      ++flow_.refined_;
    define();
    collectInputs();
    simplify();
    connectUses();
  }

  LocalDataFlow &flow_;
  const clang::FunctionDecl &function_;
  clang::CFG *cfg_;
  Andersen &pta_;
  llvm::function_ref<Node(const clang::ValueDecl *)> storage_;
  FunctionDataFlow *data_;
  BodyId body_;
  Candidates candidates_;
  unsigned nextVariable_, variableId_ = 0;
  std::map<const clang::VarDecl *, unsigned> parameters_;
  std::map<const clang::VarDecl *, CFGPosition> declarations_;
  std::vector<std::vector<unsigned>> predecessors_;
  const clang::VarDecl *decl_ = nullptr;
  bool parameter_ = false, pointer_ = false;
  std::optional<Node> entryValue_;
  std::vector<Definition> definitions_;
  std::vector<Event> events_;
  std::map<unsigned, Join> joins_;
  std::vector<unsigned> pending_;
  std::map<unsigned, Node> last_;
  std::vector<std::pair<const clang::DeclRefExpr *, Token>> uses_;
  std::map<Token, Node> materialized_;
  std::vector<Token> remaining_;
};

LocalDataFlow::LocalDataFlow(
    const clang::FunctionDecl &function, clang::CFG *cfg, Andersen &pta,
    llvm::function_ref<Node(const clang::ValueDecl *)> storage,
    FunctionDataFlow *data, BodyId body)
    : parents_(std::make_unique<clang::ParentMap>(function.getBody())) {
  Builder(*this, function, cfg, pta, storage, data, body).run();
}

LocalDataFlow::~LocalDataFlow() = default;

std::optional<CFGPosition>
LocalDataFlow::position(const clang::Stmt *stmt) const {
  for (; stmt; stmt = parents_->getParent(stmt))
    if (const auto found = positions_.find(stmt); found != positions_.end())
      return found->second;
  return std::nullopt;
}

LocalDataFlow::Definition
LocalDataFlow::read(const clang::DeclRefExpr *expr) const {
  const auto found = reads_.find(expr);
  return found == reads_.end() ? Definition{} : found->second;
}

LocalDataFlow::Definition
LocalDataFlow::write(const clang::DeclRefExpr *expr) const {
  const auto found = writes_.find(expr);
  return found == writes_.end() ? Definition{} : found->second;
}

LocalDataFlow::Definition
LocalDataFlow::initialize(const clang::VarDecl *decl) const {
  const auto found = initializers_.find(decl);
  return found == initializers_.end() ? Definition{} : found->second;
}

std::size_t LocalDataFlow::variables() const {
  return refined_;
}

std::size_t LocalDataFlow::fallbacks() const {
  return fallback_;
}

} // namespace wpgkit
