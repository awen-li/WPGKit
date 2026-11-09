#include "CallGraphPTA.h"
#include "Profiling.h"
#include "Andersen.h"
#include "ExternalEffects.h"
#include "ClangDataFlow.h"
#include "DataFlowBuilder.h"
#include "InterCFG.h"
#include "ClangCFGLifter.h"
#include "clang/Analysis/CFG.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/ODRHash.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace wpgkit {
namespace {

void disambiguateAnonymousType(const clang::Decl *decl, std::string &id) {
  // Clang's USR encodes unnamed records as @Sa without distinguishing the
  // lambdas in an instantiated template. This also affects specializations
  // taking those closure types as arguments. Use the ABI identity as a suffix;
  // ordinary declarations need no additional mangling or retained metadata.
  if (id.find("@Sa") == std::string::npos)
    return;
  for (const auto *scope = decl; scope;) {
    const auto *context = scope->getDeclContext();
    if (context && !context->isDependentContext()) {
      auto mangler = std::unique_ptr<clang::MangleContext>(
          scope->getASTContext().createMangleContext());
      std::string suffix;
      llvm::raw_string_ostream stream(suffix);
      if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(scope)) {
        if (function->getType()->isDependentType())
          return;
        if (const auto *ctor =
                llvm::dyn_cast<clang::CXXConstructorDecl>(function))
          mangler->mangleName(clang::GlobalDecl(ctor, clang::Ctor_Complete),
                              stream);
        else if (const auto *dtor =
                     llvm::dyn_cast<clang::CXXDestructorDecl>(function))
          mangler->mangleName(clang::GlobalDecl(dtor, clang::Dtor_Complete),
                              stream);
        else
          mangler->mangleName(clang::GlobalDecl(function), stream);
      } else if (const auto *record =
                     llvm::dyn_cast<clang::RecordDecl>(scope)) {
        const auto type = scope->getASTContext().getRecordType(record);
        if (type->isDependentType())
          return;
        mangler->mangleCanonicalTypeName(type, stream);
      } else if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(scope);
                 variable && variable->hasGlobalStorage()) {
        if (variable->getType()->isDependentType())
          return;
        mangler->mangleName(clang::GlobalDecl(variable), stream);
      }
      if (!suffix.empty()) {
        id += "|abi:" + suffix;
        return;
      }
    }
    scope = context ? clang::Decl::castFromDeclContext(context) : nullptr;
  }
}

std::string usr(const clang::Decl *decl, SourceUnitId unit) {
  llvm::SmallVector<char, 128> buffer;
  // Clang's location policy can differ between a user declaration and a
  // system-header redeclaration. Always identify the same canonical decl.
  if (clang::index::generateUSRForDecl(decl->getCanonicalDecl(), buffer))
    return {};
  std::string id(buffer.begin(), buffer.end());
  disambiguateAnonymousType(decl->getCanonicalDecl(), id);
  // A header's static/anonymous-namespace declarations are different entities
  // in each translation unit, even when Clang gives them the same USR.
  for (const auto *scope = decl; scope;) {
    if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(scope)) {
      const auto linkage = named->getLinkageInternal();
      if (linkage == clang::Linkage::Internal ||
          linkage == clang::Linkage::UniqueExternal)
        return "unit:" + std::to_string(unit.value()) + ":" + id;
    }
    const auto *context = scope->getDeclContext();
    scope = context ? clang::Decl::castFromDeclContext(context) : nullptr;
  }
  return id;
}

// A compact conflict diagnostic, not a proof of ODR equivalence. The linked
// program must obey the one-definition rule. Hash the instantiated body
// explicitly: ODRHash::AddFunctionDecl skips many template specializations.
class DefinitionHash final : public clang::RecursiveASTVisitor<DefinitionHash> {
public:
  explicit DefinitionHash(SourceUnitId unit) : unit_(unit) {}

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(expr->getDecl());
        variable && expr->isNonOdrUse() == clang::NOUR_Constant &&
        expr->getType()->isIntegralOrEnumerationType()) {
      clang::Expr::EvalResult value;
      if (expr->EvaluateAsInt(value, variable->getASTContext())) {
        // Non-ODR-used integral constants may denote distinct objects in each
        // unit. Their value, not storage identity, determines this expression.
        references_.AddString(usr(variable, SourceUnitId{}));
        value.Val.getInt().Profile(references_);
        return true;
      }
    }
    return reference(expr->getDecl());
  }

  bool VisitMemberExpr(clang::MemberExpr *expr) {
    return reference(expr->getMemberDecl());
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
    return reference(expr->getConstructor());
  }

  std::pair<unsigned, unsigned> compute(const clang::FunctionDecl &definition) {
    clang::ODRHash syntax;
    syntax.AddQualType(definition.getType().getCanonicalType());
    const auto add = [&](clang::Stmt *stmt) {
      syntax.AddStmt(stmt);
      TraverseStmt(stmt);
    };
    if (const auto *ctor =
            llvm::dyn_cast<clang::CXXConstructorDecl>(&definition))
      for (const auto *init : ctor->inits())
        add(init->getInit());
    add(definition.getBody());
    return {syntax.CalculateHash(), references_.ComputeHash()};
  }

private:
  bool reference(const clang::NamedDecl *decl) {
    // Parameter/local USRs contain source coordinates and are not ODR identity.
    if (!decl->getDeclContext()->isFunctionOrMethod())
      references_.AddString(usr(decl, unit_));
    return true;
  }

  SourceUnitId unit_;
  llvm::FoldingSetNodeID references_;
};

SourceLocation sourceLocation(const clang::SourceManager &sources,
                              clang::SourceLocation location,
                              GraphMetadata &metadata) {
  if (!location.isValid())
    return {};
  const auto file = sources.getFilename(location);
  return {metadata.intern(std::string_view(file.data(), file.size())),
          sources.getFileOffset(location)};
}

SourceLocation definitionLocation(const clang::FunctionDecl *decl,
                                  GraphMetadata &metadata) {
  const auto &sources = decl->getASTContext().getSourceManager();
  return sourceLocation(sources, sources.getSpellingLoc(decl->getBeginLoc()),
                        metadata);
}

CallLocation callLocation(const clang::SourceManager &sources,
                          clang::SourceLocation location,
                          GraphMetadata &metadata) {
  return {sourceLocation(sources, sources.getExpansionLoc(location), metadata),
          sourceLocation(sources, sources.getSpellingLoc(location), metadata)};
}

ParamKind paramKind(clang::QualType type) {
  if (type->isPointerType())
    return ParamKind::Pointer;
  if (type->isReferenceType() && type->getPointeeType()->isPointerType())
    return ParamKind::ReferenceToPointer;
  if (type->isReferenceType())
    return ParamKind::Reference;
  return ParamKind::Other;
}

using Param = CallArgument;

bool invokesFunction(const clang::Expr *expression) {
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expression);
      call && llvm::isa<clang::CXXPseudoDestructorExpr>(
                  call->getCallee()->IgnoreParenImpCasts()))
    return false;
  return llvm::isa<clang::CallExpr, clang::CXXConstructExpr, clang::CXXNewExpr,
                   clang::CXXDeleteExpr>(expression);
}

struct CallTarget final {
  const clang::FunctionDecl *function;
  bool virtualDispatch = false;
};

CallTarget resolveCallTarget(const clang::Expr *expr,
                             const clang::FunctionDecl *callee) {
  const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
  if (!method || !method->isVirtual())
    return {callee};
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
            call->getCallee()->IgnoreParenImpCasts());
        member && member->hasQualifier())
      return {callee}; // Explicit qualification suppresses virtual dispatch.
  const clang::Expr *object = nullptr;
  if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr))
    object = call->getImplicitObjectArgument();
  else if (const auto *call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr);
           call && call->getNumArgs())
    object = call->getArg(0);
  // Clang accounts for final overriders, concrete objects and Apple kext.
  // This query uses only the current AST; the returned identity is owned below.
  if (const auto *target = method->getDevirtualizedMethod(
          object, method->getASTContext().getLangOpts().AppleKext))
    return {target};
  return {callee, true};
}

struct BodyFacts final {
  std::vector<Andersen::Fact> constraints;
  std::unique_ptr<FunctionDataFlow> data;
  std::size_t firstSite = 0;
  std::size_t endSite = 0;
  std::set<TextId> limitations;
  bool unknownMemoryEffects = false;
  BodyId cfg;
};

struct Function final {
  FunctionId id;
  TextId usr;
  TextId name;
  std::optional<std::pair<unsigned, unsigned>> definitionHash;
  bool conflictingDefinitions = false;
  bool pureVirtual = false;
  ExternalModel model = ExternalModel::Unknown;
  std::set<TextId> linkage;
  std::vector<Param> params;
  std::optional<Andersen::Node> receiver;
  std::unique_ptr<BodyFacts> body;
  std::vector<std::size_t> callers;
  std::size_t connectedCallers = 0;
  bool bodyActivated = false;
  bool queued = false;
  bool root = false;
  bool bodyDirty = false;

  bool usesModel() const {
    return model != ExternalModel::Unknown &&
           (!body || ExternalModules::isIntrinsic(model));
  }
};

std::set<TextId> linkageNames(const clang::FunctionDecl *decl,
                              GraphMetadata &metadata) {
  std::set<TextId> names;
  auto context = std::unique_ptr<clang::MangleContext>(
      decl->getASTContext().createMangleContext());
  if (!context->shouldMangleDeclName(decl)) {
    names.insert(metadata.intern(decl->getNameAsString()));
    return names;
  }
  const auto add = [&](clang::GlobalDecl global) {
    std::string name;
    llvm::raw_string_ostream stream(name);
    context->mangleName(global, stream);
    names.insert(metadata.intern(name));
  };
  if (const auto *constructor =
          llvm::dyn_cast<clang::CXXConstructorDecl>(decl)) {
    add(clang::GlobalDecl(constructor, clang::Ctor_Complete));
    add(clang::GlobalDecl(constructor, clang::Ctor_Base));
  } else if (const auto *destructor =
                 llvm::dyn_cast<clang::CXXDestructorDecl>(decl)) {
    add(clang::GlobalDecl(destructor, clang::Dtor_Deleting));
    add(clang::GlobalDecl(destructor, clang::Dtor_Complete));
    add(clang::GlobalDecl(destructor, clang::Dtor_Base));
  } else {
    add(clang::GlobalDecl(decl));
  }
  return names;
}

struct Site {
  ExternalCall externalCall() {
    return {args,    result,     receiver,        returnsPointer,
            formats, unresolved, externalEffects, managerOperation};
  }

  bool pointerArgument(std::size_t index) const {
    return index < args.size() && args[index].kind != ParamKind::Other;
  }

  std::vector<Param> args;
  FunctionId caller;
  CallLocation location;
  BodyId body;
  Andersen::Node callee; // Only indirect sites use a PTA callee node.
  Andersen::Node result;
  FunctionId directTarget;
  bool virtualDispatch = false;
  bool returnsPointer = false;
  std::uint8_t formats = 0; // Two FormatEffect bits for each of positions 0..2.
  FunctionManagerOperation managerOperation = FunctionManagerOperation::Unknown;
  std::set<FunctionId> targets;
  bool direct = false;
  bool unresolved = false;
  bool active = false;
  bool fallbackApplied = false;
  bool fallbackQueued = false;
  bool localConstruction = false;
  ExternalSummary externalEffects;
  std::optional<Andersen::Node> receiver;
};

// A virtual site watches every known method in its override subtree. A new
// override therefore wakes only sites that can dispatch through its base.
class VirtualDispatch final {
public:
  struct Target {
    CallSiteId site;
    FunctionId function;
  };

  void addOverride(FunctionId base, FunctionId derived) {
    if (!overrides_[base].insert(derived).second)
      return;
    if (const auto found = users_.find(base); found != users_.end())
      for (const auto site : found->second)
        watch(site, derived);
  }

  void addCall(CallSiteId site, FunctionId base) {
    watch(site, base);
  }

  std::vector<Target> takeTargets() {
    std::vector<Target> targets;
    while (!pending_.empty()) {
      const auto target = pending_.front();
      pending_.pop_front();
      targets.push_back(target);
      if (const auto found = overrides_.find(target.function);
          found != overrides_.end())
        for (const auto derived : found->second)
          watch(target.site, derived);
    }
    return targets;
  }

private:
  void watch(CallSiteId site, FunctionId function) {
    if (users_[function].insert(site).second)
      pending_.push_back({site, function});
  }

  std::map<FunctionId, std::set<FunctionId>> overrides_;
  std::map<FunctionId, std::set<CallSiteId>> users_;
  std::deque<Target> pending_;
};

class Engine final {
public:
  explicit Engine(Profiler *profiler, FunctionFactObserver observe,
                  ExternalModules models)
      : models_(models), profiler_(profiler), observe_(std::move(observe)),
        interCFG_(std::make_shared<InterCFG>()) {
    if (!models_.isEmpty())
      modelEffects_.emplace(pta_);
  }

  std::size_t cfgCount() const {
    return cfgs_.size() - failedCFGs_;
  }

  std::size_t failedCFGCount() const {
    return failedCFGs_;
  }

  void extract(clang::ASTContext &unit);
  llvm::Expected<CallGraphResult> run(const std::vector<std::string> &roots,
                                      bool finalize = false,
                                      bool consume = false);

private:
  class Inventory final : public clang::RecursiveASTVisitor<Inventory> {
  public:
    explicit Inventory(Engine &engine) : engine_(engine) {}

    bool shouldVisitTemplateInstantiations() const {
      return true;
    }

    bool shouldVisitImplicitCode() const {
      return true;
    }

    bool VisitFunctionDecl(clang::FunctionDecl *decl);
    bool VisitVarDecl(clang::VarDecl *decl);

  private:
    Engine &engine_;
  };

  class Body final : public clang::RecursiveASTVisitor<Body> {
  public:
    Body(Engine &engine, FunctionId function)
        : engine_(engine), function_(function) {}

    bool VisitStmt(clang::Stmt *stmt) {
      if (engine_.dataCapture_)
        engine_.dataCapture_->statement(stmt);
      return true;
    }

    bool VisitVarDecl(clang::VarDecl *decl);
    bool VisitBinaryOperator(clang::BinaryOperator *expr);
    bool VisitUnaryOperator(clang::UnaryOperator *expr);
    bool VisitReturnStmt(clang::ReturnStmt *stmt);
    bool VisitCallExpr(clang::CallExpr *expr);
    bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr);
    bool VisitMaterializeTemporaryExpr(clang::MaterializeTemporaryExpr *expr);
    bool VisitCXXNewExpr(clang::CXXNewExpr *expr);
    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr);
    bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr *expr);
    bool VisitCXXDefaultArgExpr(clang::CXXDefaultArgExpr *expr);
    bool VisitCXXDefaultInitExpr(clang::CXXDefaultInitExpr *expr);
    bool VisitCXXThrowExpr(clang::CXXThrowExpr *expr);
    bool VisitGCCAsmStmt(clang::GCCAsmStmt *stmt);
    bool VisitMSAsmStmt(clang::MSAsmStmt *stmt);
    bool VisitAtomicExpr(clang::AtomicExpr *expr);
    bool VisitCoroutineBodyStmt(clang::CoroutineBodyStmt *stmt);
    bool VisitDeclRefExpr(clang::DeclRefExpr *expr);
    bool TraverseLambdaExpr(clang::LambdaExpr *expr);
    bool TraverseTypeLoc(clang::TypeLoc type);
    bool
    TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr);

    bool TraverseCXXTypeidExpr(clang::CXXTypeidExpr *expr) {
      if (expr->isTypeOperand())
        return true;
      return expr->isPotentiallyEvaluated()
                 ? TraverseStmt(expr->getExprOperand())
                 : skip(expr->getExprOperand());
    }

    bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr *expr) {
      return skip(expr->getOperand());
    }

    bool TraverseRequiresExpr(clang::RequiresExpr *expr) {
      return skip(expr);
    }

    bool TraverseTypeTraitExpr(clang::TypeTraitExpr *) {
      return true;
    }

    bool TraverseExpressionTraitExpr(clang::ExpressionTraitExpr *) {
      return true;
    }

    bool TraverseCallExpr(clang::CallExpr *expr) {
      if (expr->isUnevaluatedBuiltinCall(
              engine_.activeDefinition_->getASTContext())) {
        // Retain the intrinsic's CFG occurrence, but never evaluate its
        // operands while extracting actual arguments or walking child
        // expressions.
        engine_.site(expr, expr->getDirectCallee(), {}, function_);
        for (auto *arg : expr->arguments())
          skip(arg);
        return true;
      }
      return clang::RecursiveASTVisitor<Body>::TraverseCallExpr(expr);
    }

    bool TraverseChooseExpr(clang::ChooseExpr *expr) {
      skip(expr->getCond());
      if (expr->isConditionDependent())
        return skip(expr);
      skip(expr->isConditionTrue() ? expr->getRHS() : expr->getLHS());
      return TraverseStmt(expr->getChosenSubExpr());
    }

    bool TraverseGenericSelectionExpr(clang::GenericSelectionExpr *expr) {
      if (expr->isResultDependent())
        return skip(expr);
      for (auto *child : expr->children())
        if (child != expr->getResultExpr())
          skip(child);
      return TraverseStmt(expr->getResultExpr());
    }

  private:
    bool skip(clang::Stmt *statement);
    std::size_t constructionSite(clang::CXXConstructExpr *expr);
    Engine &engine_;
    FunctionId function_;
  };

  Andersen::Node nodeFor(const clang::ValueDecl *decl);

  std::string usr(const clang::Decl *decl) const {
    return wpgkit::usr(decl, activeUnitId_);
  }

  void rememberDefinition(const clang::FunctionDecl *decl);
  Andersen::Node functionObject(const clang::FunctionDecl *decl);
  void markExternalInputs(const Function &function);
  Andersen::Node returnNode(FunctionId function);
  Andersen::Node unknownValue(const char *reason);
  Andersen::Node fieldNode(const clang::MemberExpr *member, FunctionId caller);
  Andersen::Node arrayIndex(std::uint64_t index);
  Andersen::Node fieldAddress(const clang::MemberExpr *member,
                              FunctionId caller);
  Andersen::Node receiverNode(FunctionId function);
  Andersen::Node captureAddress(const clang::ValueDecl *, FunctionId);
  void initializeLambda(const clang::VarDecl &, const clang::LambdaExpr &,
                        FunctionId);
  Andersen::Node addressOf(const clang::Expr *expr, FunctionId caller);
  Andersen::Node value(const clang::Expr *expr, FunctionId caller);
  Andersen::Node extractValue(const clang::Expr *expr, FunctionId caller);
  Andersen::Node expressionValue(const clang::Expr *expr, FunctionId caller);
  void captureArguments(std::size_t index, const clang::Expr *expr,
                        const clang::FunctionDecl *callee,
                        std::span<const clang::Expr *const> args,
                        FunctionId caller, ExternalModel model);
  void extractFunctionFacts(const clang::FunctionDecl &definition,
                            clang::CFG *native, FunctionId function,
                            BodyFacts &body);
  llvm::Error beginRun(const std::vector<std::string> &roots, bool finalize);
  llvm::Error solve();
  CallGraphResult snapshot(bool consume);
  void buildDataFlow(CallGraphResult &result, bool consume);
  std::size_t site(const clang::CallExpr *expr, FunctionId caller);
  std::size_t site(const clang::CXXNewExpr *expr, FunctionId caller);
  std::size_t site(const clang::Expr *expr, const clang::FunctionDecl *callee,
                   std::vector<const clang::Expr *> args, FunctionId caller);
  std::size_t site(const clang::VarDecl *decl,
                   const clang::CXXDestructorDecl *destructor,
                   FunctionId caller);
  Function &ensureFunction(const std::string &usr);
  FunctionId findFunction(std::string_view usr) const;

  Function &functionAt(FunctionId id) {
    return functions_.at(id.value() - 1);
  }

  bool edge(std::size_t siteIndex, FunctionId callee);
  void enqueueFunction(FunctionId callee);
  void activate(FunctionId callee);
  void activateSite(std::size_t index);
  InterCFG &writableInterCFG();
  void attachCFG(Function &function);
  void updateSummary(std::size_t index, bool unresolved);
  bool readOnlyFunction(FunctionId function);
  bool readOnlyCall(const Site &call);
  void checkRuntimeContracts(Site &site);
  void recordModel(const clang::FunctionDecl &declaration, Function &function,
                   ExternalModel model);
  bool knownResult(const Site &site);
  bool provesProperty(FunctionId function, RuntimeProperty property);
  void enqueueFallback(std::size_t index);
  bool applyUnknownCalls();
  void markBodyDirty(FunctionId function);
  void refreshBodyStates();
  bool unresolved(const Site &site, bool unknownEffects);
  void assign(const clang::Expr *left, const clang::Expr *right,
              FunctionId caller);
  bool writePointer(const clang::Expr *left, Andersen::Node source,
                    FunctionId caller, bool refineLocal);
  void copyAggregate(Andersen::Node destination, Andersen::Node source,
                     const clang::CXXRecordDecl &, const clang::Expr *);
  void initializeFields(Andersen::Node, const clang::CXXRecordDecl &,
                        const clang::InitListExpr &, FunctionId);
  void initializeAggregate(const clang::VarDecl *decl, FunctionId caller);
  void invalidatePointer(const clang::Expr *left, const char *reason,
                         FunctionId caller);
  void finishCFGCapture(llvm::Expected<FunctionCFG> graph,
                        std::shared_ptr<CapturedCFG> captured, BodyFacts &body,
                        std::vector<bool> matched);
  void captureCFG(const clang::FunctionDecl &definition, FunctionId function,
                  BodyFacts &body);
  void extractBody(const clang::FunctionDecl &definition, FunctionId function);
  void observeElement(const clang::CFGElement &element, CFGPosition position,
                      FunctionId function);

  const ExternalModules models_;
  Andersen pta_;
  std::optional<ExternalEffects> modelEffects_;
  // Multiple targets can share an identical numeric contract. Apply it once
  // per indirect/virtual site; ordinary direct calls need no extra index.
  std::set<std::pair<std::size_t, ExternalModel>> appliedModelEffects_;
  Profiler *profiler_ = nullptr;
  FunctionFactObserver observe_;
  std::optional<LocalDataFlow> localFlow_; // Current body only.
  std::optional<ClangDataFlow> dataCapture_;
  CFGFactObserver captureFacts_; // Borrowed native storage; current body only.
  std::shared_ptr<const DataFlowGraph> dataFlow_;
  std::map<const clang::Expr *, Andersen::Node> pointerValues_;
  std::map<std::uint64_t, Andersen::Node> arrayIndices_;
  std::set<const clang::LambdaExpr *> initializedLambdas_;
  std::size_t cfgLocalVariables_ = 0, cfgLocalFallbacks_ = 0;
  std::shared_ptr<InterCFG> interCFG_;
  bool overlayMayChange_ = true;
  std::map<CallSiteId, std::vector<CFGPosition>> callPositions_;
  GraphMetadata metadata_;
  std::map<TextId, FunctionId> functionIds_;
  // A deque preserves references when extraction discovers another function.
  std::deque<Function> functions_;
  std::map<TextId, std::set<FunctionId>> rootDefinitions_;
  VirtualDispatch virtualDispatch_;
  std::map<TextId, Andersen::Node> declarationNodes_;
  std::map<const clang::ValueDecl *, Andersen::Node> localNodes_;
  std::set<Andersen::Node>
      externalStorage_; // Globals and merged field storage.
  bool finalized_ = false;
  bool globalFallbackApplied_ = false;
  bool globalUnknownObserved_ = false;
  std::vector<std::size_t> pendingFallback_;
  std::map<FunctionId, Andersen::Node> functionObjects_;
  std::map<Andersen::Node, FunctionId> objectFunctions_;
  std::map<FunctionId, Andersen::Node> returns_;
  // Clang may share non-dependent expressions between template instantiations.
  // Their effects and call sites still belong to distinct function bodies.
  std::map<std::tuple<const clang::Expr *, FunctionId, FunctionId>, std::size_t>
      siteByExpr_;
  std::map<std::pair<const clang::VarDecl *, FunctionId>, std::size_t>
      siteByDecl_;
  std::set<const clang::Stmt *>
      unevaluated_; // Current body only; discarded after CFG lift.
  std::vector<Site> sites_;
  std::vector<std::shared_ptr<const CapturedCFG>> cfgs_;
  std::size_t failedCFGs_ = 0;
  std::vector<const clang::VarDecl *> globals_;
  std::vector<const clang::FunctionDecl *> currentDefinitions_;
  std::set<const clang::FunctionDecl *> currentSeenDefinitions_;
  std::vector<Andersen::Fact> globalFacts_;
  std::set<TextId> inventoryLimitations_;
  bool inventoryUnknownMemoryEffects_ = false;
  const clang::FunctionDecl *activeDefinition_ = nullptr;
  std::deque<FunctionId> pending_;
  std::set<FunctionId> reached_;
  std::deque<FunctionId> dirtyBodies_;
  std::set<FunctionId> incompleteFunctions_;
  std::set<TextId> limitations_;
  bool unknownMemoryEffects_ = false;
  std::set<TextId> analysisLimitations_;
  bool analysisUnknownMemoryEffects_ = false;
  std::set<TextId> roots_;
  std::map<Andersen::Node, std::vector<std::size_t>> indirectUsers_;
  std::map<Andersen::Node, std::vector<std::size_t>> callbackUsers_;
  std::map<FunctionId, bool> readOnlyCallbacks_;
  std::map<FunctionId, FunctionId> runtimeDelegates_;
  std::set<std::size_t> pendingIndirect_;
  CallGraphResult::Statistics statistics_;
  const clang::SourceManager *activeSources_ = nullptr;
  std::size_t nextUnitId_ = 0;
  SourceUnitId activeUnitId_;
};

Function &Engine::ensureFunction(const std::string &usr) {
  const auto text = metadata_.intern(usr);
  auto [found, inserted] = functionIds_.try_emplace(text);
  if (inserted) {
    auto &function = functions_.emplace_back();
    function.id = FunctionId(functions_.size());
    function.usr = text;
    found->second = function.id;
  }
  return functionAt(found->second);
}

FunctionId Engine::findFunction(std::string_view usr) const {
  const auto found = functionIds_.find(metadata_.find(usr));
  return found == functionIds_.end() ? FunctionId{} : found->second;
}

bool Engine::Inventory::VisitFunctionDecl(clang::FunctionDecl *decl) {
  if (decl->isDependentContext())
    return true;
  if (decl->isImplicit() && !llvm::isa<clang::CXXConversionDecl>(decl) &&
      !(llvm::isa<clang::CXXMethodDecl>(decl) &&
        llvm::cast<clang::CXXMethodDecl>(decl)->getParent()->isLambda()))
    return true;
  auto id = engine_.usr(decl);
  if (id.empty()) {
    engine_.limitations_.insert(
        engine_.metadata_.intern("function without Clang USR"));
    return true;
  }
  auto &function = engine_.ensureFunction(id);
  function.name = engine_.metadata_.intern(decl->getQualifiedNameAsString());
  engine_.recordModel(*decl, function, engine_.models_.query(decl).model);
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
    function.pureVirtual = method->isPureVirtual();
    for (const auto *base : method->overridden_methods()) {
      if (function.linkage.empty())
        function.linkage = linkageNames(decl, engine_.metadata_);
      const auto baseId = engine_.usr(base);
      if (baseId.empty())
        engine_.limitations_.insert(
            engine_.metadata_.intern("virtual override without Clang USR"));
      else
        engine_.virtualDispatch_.addOverride(engine_.ensureFunction(baseId).id,
                                             function.id);
    }
  }
  if (decl->isThisDeclarationADefinition()) {
    engine_.rememberDefinition(decl);
  }
  return true;
}

void Engine::rememberDefinition(const clang::FunctionDecl *decl) {
  const auto *definition = decl->getDefinition();
  if (!definition || definition->isDependentContext() || !definition->getBody())
    return;
  if (!currentSeenDefinitions_.insert(definition).second)
    return;
  const auto id = usr(definition);
  if (id.empty()) {
    limitations_.insert(
        metadata_.intern("function definition without Clang USR"));
    return;
  }
  auto &function = ensureFunction(id);
  function.name = metadata_.intern(definition->getQualifiedNameAsString());
  const auto hash = DefinitionHash(activeUnitId_).compute(*definition);
  if (!function.definitionHash) {
    function.definitionHash = hash;
    markBodyDirty(function.id);
  } else if (*function.definitionHash != hash) {
    function.conflictingDefinitions = true;
  }
  currentDefinitions_.push_back(definition);
}

bool Engine::Inventory::VisitVarDecl(clang::VarDecl *decl) {
  if (decl->getDeclContext()->isDependentContext())
    return true;
  if (decl->isFileVarDecl() && decl->hasInit()) {
    if (const auto *record = decl->getType()->getAsCXXRecordDecl();
        record && !record->hasTrivialDestructor()) {
      engine_.limitations_.insert(
          engine_.metadata_.intern("global destructor effects not indexed"));
      engine_.unknownMemoryEffects_ = true;
    }
    engine_.globals_.push_back(decl);
    if (!decl->hasConstantInitialization()) {
      engine_.limitations_.insert(engine_.metadata_.intern(
          "dynamic global initializer effects not modeled"));
      engine_.unknownMemoryEffects_ = true;
    }
  }
  return true;
}

Andersen::Node Engine::nodeFor(const clang::ValueDecl *decl) {
  const auto create = [&] {
    const auto node = pta_.addNode();
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
    if (llvm::isa<clang::FieldDecl>(decl) ||
        (variable && variable->hasGlobalStorage())) {
      externalStorage_.insert(node);
      pta_.markExternallyAffected(node);
    }
    if (modelEffects_ && llvm::isa<clang::FieldDecl>(decl))
      modelEffects_->addField(node);
    return node;
  };
  const auto id = metadata_.intern(usr(decl));
  if (id.isValid()) {
    if (const auto found = declarationNodes_.find(id);
        found != declarationNodes_.end())
      return found->second;
    return declarationNodes_.emplace(id, create()).first->second;
  }
  if (const auto found = localNodes_.find(decl); found != localNodes_.end())
    return found->second;
  return localNodes_.emplace(decl, create()).first->second;
}

Andersen::Node Engine::functionObject(const clang::FunctionDecl *decl) {
  const auto id = usr(decl);
  if (id.empty())
    return unknownValue("function address without Clang USR");
  auto &function = ensureFunction(id);
  function.name = metadata_.intern(decl->getQualifiedNameAsString());
  recordModel(*decl, function, models_.query(decl).model);
  if (function.linkage.empty())
    function.linkage = linkageNames(decl, metadata_);
  rememberDefinition(decl);
  const auto found = functionObjects_.find(function.id);
  if (found != functionObjects_.end())
    return found->second;
  const auto node = pta_.addNode();
  functionObjects_.emplace(function.id, node);
  objectFunctions_.emplace(node, function.id);
  markExternalInputs(function);
  return node;
}

void Engine::markExternalInputs(const Function &function) {
  // Unknown code can invoke an exported/address-taken function with new inputs.
  // No synthetic calls or body activation: only its existing formals widen.
  for (const auto &param : function.params)
    if (param.kind != ParamKind::Other)
      pta_.markExternallyAffected(param.node);
}

Andersen::Node Engine::returnNode(FunctionId function) {
  const auto found = returns_.find(function);
  if (found != returns_.end())
    return found->second;
  const auto node = pta_.addNode();
  returns_.emplace(function, node);
  return node;
}

Andersen::Node Engine::unknownValue(const char *reason) {
  limitations_.insert(metadata_.intern(reason));
  const auto node = pta_.addNode();
  pta_.markUnknown(node);
  return node;
}

Andersen::Node Engine::receiverNode(FunctionId id) {
  auto &function = functionAt(id);
  if (!function.receiver)
    function.receiver = pta_.addNode();
  return *function.receiver;
}

Andersen::Node Engine::captureAddress(const clang::ValueDecl *variable,
                                      FunctionId caller) {
  const auto *method =
      llvm::dyn_cast_or_null<clang::CXXMethodDecl>(activeDefinition_);
  if (method && method->getParent()->isLambda()) {
    llvm::DenseMap<const clang::ValueDecl *, clang::FieldDecl *> fields;
    clang::FieldDecl *thisField = nullptr;
    method->getParent()->getCaptureFields(fields, thisField);
    if (const auto found = fields.find(variable); found != fields.end())
      return pta_.project(receiverNode(caller), nodeFor(found->second));
  }
  return unknownValue("capture field unavailable");
}

Andersen::Node Engine::arrayIndex(std::uint64_t index) {
  auto [found, added] = arrayIndices_.try_emplace(index);
  if (added)
    found->second = pta_.addNode();
  return found->second;
}

Andersen::Node Engine::fieldAddress(const clang::MemberExpr *member,
                                    FunctionId caller) {
  const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
  if (!field)
    return unknownValue("unsupported member expression");
  if (field->getParent()->isUnion())
    field = *field->getParent()->field_begin();
  const auto *base = member->getBase()->IgnoreParenImpCasts();
  return pta_.project(member->isArrow() ? value(base, caller)
                                        : addressOf(base, caller),
                      nodeFor(field));
}

Andersen::Node Engine::fieldNode(const clang::MemberExpr *member,
                                 FunctionId caller) {
  const auto result = pta_.addNode();
  pta_.load(result, fieldAddress(member, caller));
  return result;
}

Andersen::Node Engine::addressOf(const clang::Expr *expr, FunctionId caller) {
  expr = expr->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (ref->getDecl()->getType()->isReferenceType())
      return nodeFor(ref->getDecl());
    const auto result = pta_.addNode();
    if (const auto *function =
            llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()))
      pta_.address(result, functionObject(function));
    else
      pta_.address(result, nodeFor(ref->getDecl()));
    return result;
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl())) {
      if (field->getType()->isReferenceType())
        return fieldNode(member, caller);
      return fieldAddress(member, caller);
    }
  }
  if (const auto *array = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
    const auto *base = array->getBase()->IgnoreParenImpCasts();
    clang::Expr::EvalResult index;
    const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(base);
    if (ref && base->getType()->isArrayType() &&
        array->getIdx()->EvaluateAsInt(index,
                                       ref->getDecl()->getASTContext()) &&
        !index.HasSideEffects && !index.HasUndefinedBehavior &&
        index.Val.getInt().isNonNegative() &&
        index.Val.getInt().getActiveBits() <= 64)
      return pta_.project(addressOf(base, caller),
                          arrayIndex(index.Val.getInt().getZExtValue()));
    return unknownValue("unknown array subobject");
  }
  if (const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(expr);
      deref && deref->getOpcode() == clang::UO_Deref)
    return value(deref->getSubExpr(), caller);
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
    if (const auto *callee = call->getDirectCallee();
        callee && callee->getReturnType()->isReferenceType())
      return sites_.at(site(call, caller)).result;
  return unknownValue("unsupported pointer reference binding");
}

Andersen::Node Engine::value(const clang::Expr *expr, FunctionId caller) {
  if (const auto found = pointerValues_.find(expr);
      found != pointerValues_.end())
    return found->second;
  const auto result = extractValue(expr, caller);
  pointerValues_.emplace(expr, result);
  return result;
}

Andersen::Node Engine::extractValue(const clang::Expr *expr,
                                    FunctionId caller) {
  if (!expr)
    return unknownValue("missing pointer expression");
  while (true) {
    if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
      expr = paren->getSubExpr();
    } else if (const auto *cast =
                   llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
      switch (cast->getCastKind()) {
      case clang::CK_LValueToRValue:
      case clang::CK_FunctionToPointerDecay:
      case clang::CK_NoOp:
      case clang::CK_NullToPointer:
      case clang::CK_BitCast:
        expr = cast->getSubExpr();
        break;
      case clang::CK_ArrayToPointerDecay:
        return pta_.project(addressOf(cast->getSubExpr(), caller),
                            arrayIndex(0));
      default:
        return unknownValue("unsupported pointer cast");
      }
    } else if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expr);
               cast &&
               (cast->getCastKind() == clang::CK_NoOp ||
                cast->getCastKind() == clang::CK_BitCast) &&
               cast->getType()->isPointerType() &&
               cast->getSubExpr()->getType()->isPointerType()) {
      expr = cast->getSubExpr();
    } else {
      break;
    }
  }
  return expressionValue(expr, caller);
}

Andersen::Node Engine::expressionValue(const clang::Expr *expr,
                                       FunctionId caller) {
  if (llvm::isa<clang::CXXThisExpr>(expr) && caller.isValid())
    return receiverNode(caller);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (const auto *function =
            llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl())) {
      const auto result = pta_.addNode();
      pta_.address(result, functionObject(function));
      return result;
    }
    if (ref->getDecl()->getType()->isReferenceType() &&
        ref->getType()->isPointerType()) {
      const auto result = pta_.addNode();
      pta_.load(result, nodeFor(ref->getDecl()));
      return result;
    }
    if (localFlow_)
      if (const auto node = localFlow_->read(ref).pointer)
        return *node;
    const auto node = nodeFor(ref->getDecl());
    pta_.markExternallyAffected(
        node); // Shared fallback, not a local SSA value.
    return node;
  }
  if (llvm::isa<clang::ArraySubscriptExpr>(expr)) {
    const auto result = pta_.addNode();
    pta_.load(result, addressOf(expr, caller));
    return result;
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl())) {
      // Field cells are relative to each base object; pointer contents remain
      // MAY sets.
      const auto node = fieldNode(member, caller);
      if (field->getType()->isReferenceType() &&
          member->getType()->isPointerType()) {
        const auto result = pta_.addNode();
        pta_.load(result, node);
        return result;
      }
      return node;
    }
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unary->getOpcode() == clang::UO_AddrOf) {
      const auto *operand = unary->getSubExpr()->IgnoreParenImpCasts();
      if (llvm::isa<clang::DeclRefExpr, clang::ArraySubscriptExpr>(operand))
        return addressOf(operand, caller);
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(operand);
          member && llvm::isa<clang::FieldDecl>(member->getMemberDecl()))
        return addressOf(member, caller);
      if (llvm::isa<clang::CallExpr>(operand))
        return addressOf(operand, caller); // Address of a returned reference.
      return unknownValue("unsupported address expression");
    }
    if (unary->getOpcode() == clang::UO_Deref) {
      const auto pointer = value(unary->getSubExpr(), caller);
      if (expr->getType()->isFunctionType())
        return pointer;
      const auto result = pta_.addNode();
      pta_.load(result, pointer);
      return result;
    }
  }
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
    if (!caller.isValid())
      return unknownValue("global initializer call not modeled");
    const auto result = sites_.at(site(call, caller)).result;
    if (const auto *callee = call->getDirectCallee();
        callee && callee->getReturnType()->isReferenceType() &&
        call->getType()->isPointerType()) {
      const auto loaded = pta_.addNode();
      pta_.load(loaded, result);
      return loaded;
    }
    return result;
  }
  if (llvm::isa<clang::CXXNullPtrLiteralExpr>(expr) ||
      llvm::isa<clang::GNUNullExpr>(expr) ||
      (llvm::isa<clang::IntegerLiteral>(expr) &&
       llvm::cast<clang::IntegerLiteral>(expr)->getValue() == 0))
    return pta_.addNode();
  if (const auto *conditional =
          llvm::dyn_cast<clang::AbstractConditionalOperator>(expr)) {
    const auto result = pta_.addNode();
    pta_.copy(result, value(conditional->getTrueExpr(), caller));
    pta_.copy(result, value(conditional->getFalseExpr(), caller));
    return result;
  }
  if (const auto *allocation = llvm::dyn_cast<clang::CXXNewExpr>(expr)) {
    // The selected allocator's body, model or conservative fallback supplies
    // the storage. Placement arguments need not be aliases of its result.
    return sites_.at(site(allocation, caller)).result;
  }
  return unknownValue("unsupported pointer expression");
}

void Engine::assign(const clang::Expr *left, const clang::Expr *right,
                    FunctionId caller) {
  if (!left->getType()->isPointerType())
    return;
  if (!writePointer(left, value(right, caller), caller, /*refineLocal=*/true))
    limitations_.insert(
        metadata_.intern("unsupported pointer assignment destination"));
}

bool Engine::writePointer(const clang::Expr *left, Andersen::Node source,
                          FunctionId caller, bool refineLocal) {
  left = left->IgnoreParenImpCasts();
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(left)) {
    if (ref->getDecl()->getType()->isReferenceType())
      pta_.store(nodeFor(ref->getDecl()), source);
    else {
      const auto definition =
          refineLocal ? localFlow_->write(ref).pointer : std::nullopt;
      pta_.copy(definition ? *definition : nodeFor(ref->getDecl()), source);
    }
  } else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(left);
             member && llvm::isa<clang::FieldDecl>(member->getMemberDecl())) {
    const auto node = member->getMemberDecl()->getType()->isReferenceType()
                          ? fieldNode(member, caller)
                          : fieldAddress(member, caller);
    pta_.store(node, source);
  } else if (const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(left);
             deref && deref->getOpcode() == clang::UO_Deref) {
    pta_.store(value(deref->getSubExpr(), caller), source);
  } else if (llvm::isa<clang::ArraySubscriptExpr>(left)) {
    pta_.store(addressOf(left, caller), source);
  } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(left);
             call && call->isGLValue()) {
    pta_.store(addressOf(call, caller), source);
  } else {
    unknownMemoryEffects_ = true;
    return false;
  }
  return true;
}

void Engine::invalidatePointer(const clang::Expr *left, const char *reason,
                               FunctionId caller) {
  // Unsupported mutations affect shared storage, not a new local definition.
  writePointer(left, unknownValue(reason), caller, /*refineLocal=*/false);
}

void Engine::copyAggregate(Andersen::Node destination, Andersen::Node source,
                           const clang::CXXRecordDecl &record,
                           const clang::Expr *expression) {
  if (record.isUnion() || record.getNumBases()) {
    pta_.havoc(destination);
    return;
  }
  for (const auto *field : record.fields()) {
    const auto to = pta_.project(destination, nodeFor(field));
    const auto from = pta_.project(source, nodeFor(field));
    if (dataCapture_)
      dataCapture_->copyField(from, to, expression);
    if (const auto *nested = field->getType()->getAsCXXRecordDecl()) {
      copyAggregate(to, from, *nested, expression);
      continue;
    }
    if (field->getType()->isPointerType() ||
        field->getType()->isReferenceType()) {
      const auto contents = pta_.addNode();
      pta_.load(contents, from);
      pta_.store(to, contents);
    }
  }
}

void Engine::initializeFields(Andersen::Node base,
                              const clang::CXXRecordDecl &record,
                              const clang::InitListExpr &list,
                              FunctionId caller) {
  if (record.isUnion() || record.getNumBases()) {
    pta_.havoc(base);
    return;
  }
  unsigned index = 0;
  for (const auto *field : record.fields()) {
    if (index >= list.getNumInits())
      break;
    const auto *initial = list.getInit(index++);
    const auto address = pta_.project(base, nodeFor(field));
    if (const auto *nested = llvm::dyn_cast<clang::InitListExpr>(initial)) {
      if (const auto *type = field->getType()->getAsCXXRecordDecl()) {
        initializeFields(address, *type, *nested, caller);
        continue;
      }
    }
    if (dataCapture_)
      dataCapture_->field(address, initial);
    if (field->getType()->isPointerType())
      pta_.store(address, value(initial, caller));
    else if (field->getType()->isReferenceType())
      pta_.store(address, addressOf(initial, caller));
  }
}

void Engine::initializeLambda(const clang::VarDecl &variable,
                              const clang::LambdaExpr &lambda,
                              FunctionId caller) {
  llvm::DenseMap<const clang::ValueDecl *, clang::FieldDecl *> fields;
  clang::FieldDecl *thisField = nullptr;
  lambda.getLambdaClass()->getCaptureFields(fields, thisField);
  const auto base = pta_.addNode();
  pta_.address(base, nodeFor(&variable));
  auto initial = lambda.capture_init_begin();
  for (const auto &capture : lambda.captures()) {
    const auto *valueExpr = *initial++;
    const auto *field = capture.capturesVariable()
                            ? fields.lookup(capture.getCapturedVar())
                            : thisField;
    if (!field || !valueExpr) {
      unknownMemoryEffects_ = true;
      continue;
    }
    const auto address = pta_.project(base, nodeFor(field));
    if (dataCapture_)
      dataCapture_->field(address, valueExpr);
    if (field->getType()->isReferenceType())
      pta_.store(address, addressOf(valueExpr, caller));
    else if (field->getType()->isPointerType())
      pta_.store(address, value(valueExpr, caller));
    else if (field->getType()->isRecordType())
      pta_.havoc(address);
  }
  initializedLambdas_.insert(&lambda);
}

void Engine::initializeAggregate(const clang::VarDecl *decl,
                                 FunctionId caller) {
  if (const auto *lambda =
          llvm::dyn_cast_or_null<clang::LambdaExpr>(decl->getInit())) {
    initializeLambda(*decl, *lambda, caller);
    return;
  }
  const auto *record = decl->getType()->getAsCXXRecordDecl();
  const auto *list = decl->getInit()
                         ? llvm::dyn_cast<clang::InitListExpr>(
                               decl->getInit()->IgnoreParenImpCasts())
                         : nullptr;
  if (list && decl->getType()->isArrayType()) {
    const auto base = pta_.addNode();
    pta_.address(base, nodeFor(decl));
    for (unsigned index = 0; index < list->getNumInits(); ++index) {
      const auto address = pta_.project(base, arrayIndex(index));
      const auto *initial = list->getInit(index);
      if (dataCapture_)
        dataCapture_->field(address, initial);
      if (initial->getType()->isPointerType())
        pta_.store(address, value(initial, caller));
    }
    return;
  }
  if (!record)
    return;
  const auto *copy =
      llvm::dyn_cast_or_null<clang::CXXConstructExpr>(decl->getInit());
  if (copy && copy->getConstructor()->isTrivial() &&
      copy->getConstructor()->isCopyOrMoveConstructor() &&
      copy->getNumArgs() == 1) {
    const auto base = pta_.addNode();
    pta_.address(base, nodeFor(decl));
    copyAggregate(base, addressOf(copy->getArg(0), caller), *record, copy);
    return;
  }
  if (!list) {
    if (!caller.isValid() && decl->hasInit())
      for (const auto *field : record->fields())
        if (field->getType()->isPointerType()) {
          limitations_.insert(metadata_.intern(
              "constant global record constructor not modeled"));
          pta_.markUnknown(nodeFor(field));
        }
    return;
  }
  const auto base = pta_.addNode();
  pta_.address(base, nodeFor(decl));
  initializeFields(base, *record, *list, caller);
}

std::size_t Engine::site(const clang::CallExpr *expr, FunctionId caller) {
  std::vector<const clang::Expr *> args;
  unsigned offset = 0;
  if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr);
      op && llvm::isa_and_nonnull<clang::CXXMethodDecl>(op->getDirectCallee()))
    offset = 1; // The first AST argument is the implicit object, not a formal.
  for (unsigned i = offset; i < expr->getNumArgs(); ++i)
    args.push_back(expr->getArg(i));
  return site(expr, ExternalModules::resolveCallee(*expr), std::move(args),
              caller);
}

std::size_t Engine::site(const clang::CXXNewExpr *expr, FunctionId caller) {
  // Size and optional alignment are implicit scalar arguments. Keep their
  // positions so explicit placement pointers bind to the selected formals.
  std::vector<const clang::Expr *> args(1 + expr->passAlignment(), nullptr);
  for (const auto *arg : expr->placement_arguments())
    args.push_back(arg);
  return site(expr, expr->getOperatorNew(), std::move(args), caller);
}

void Engine::captureArguments(std::size_t index, const clang::Expr *expr,
                              const clang::FunctionDecl *callee,
                              std::span<const clang::Expr *const> args,
                              FunctionId caller, ExternalModel model) {
  const clang::FunctionProtoType *prototype = nullptr;
  if (!callee)
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
      auto type = call->getCallee()->getType();
      if (type->isPointerType() || type->isReferenceType())
        type = type->getPointeeType();
      prototype = type->getAs<clang::FunctionProtoType>();
    }
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto kind = ParamKind::Other;
    if (callee && i < callee->getNumParams())
      kind = paramKind(callee->getParamDecl(i)->getType());
    else if (prototype && i < prototype->getNumParams())
      kind = paramKind(prototype->getParamType(i));
    else if (args[i])
      kind = paramKind(args[i]->getType()); // Variadic arguments.
    if (callee && args[i] &&
        ExternalModules::referenceArgument(model) == static_cast<int>(i))
      kind = args[i]->getType()->isPointerType() ? ParamKind::ReferenceToPointer
                                                 : ParamKind::Reference;
    Andersen::Node node = 0;
    if (args[i] && kind == ParamKind::Pointer)
      node = value(args[i], caller);
    else if (args[i] && (kind == ParamKind::ReferenceToPointer ||
                         kind == ParamKind::Reference))
      node = addressOf(args[i], caller);
    sites_[index].args.push_back({node, kind});
  }
}

std::size_t Engine::site(const clang::Expr *expr,
                         const clang::FunctionDecl *callee,
                         std::vector<const clang::Expr *> args,
                         FunctionId caller) {
  const auto resolved = resolveCallTarget(expr, callee);
  callee = resolved.function;
  const auto calleeUsr = callee ? usr(callee) : std::string();
  const auto calleeId =
      calleeUsr.empty() ? FunctionId{} : ensureFunction(calleeUsr).id;
  const auto key = std::make_tuple(expr, caller, calleeId);
  if (const auto found = siteByExpr_.find(key); found != siteByExpr_.end())
    return found->second;
  const auto model = models_.query(callee).model;
  const auto index = sites_.size();
  const auto &sources = *activeSources_;
  Site siteFact{};
  siteFact.caller = caller;
  siteFact.location = callLocation(sources, expr->getExprLoc(), metadata_);
  siteFact.result = pta_.addNode();
  siteFact.returnsPointer =
      expr->getType()->isPointerType() ||
      (callee && callee->getReturnType()->isReferenceType());
  // Capture format candidates before indirect targets are known. Six bits
  // distinguish output-only, count writes, dynamic and unsupported formats.
  for (std::size_t i = 0; i < std::min<std::size_t>(args.size(), 3); ++i)
    if (!models_.isEmpty() && args[i] && activeDefinition_)
      siteFact.formats |= static_cast<unsigned>(ExternalModules::formatEffect(
                              *args[i], activeDefinition_->getASTContext()))
                          << (2 * i);
  if (!models_.isEmpty() && args.size() == 3 && args[2] && activeDefinition_)
    siteFact.managerOperation = ExternalModules::managerOperation(
        *args[2], activeDefinition_->getASTContext());
  sites_.push_back(std::move(siteFact));
  siteByExpr_.emplace(key, index);
  captureArguments(index, expr, callee, args, caller, model);
  if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr))
    if (const auto *object = call->getImplicitObjectArgument())
      sites_[index].receiver = object->getType()->isPointerType()
                                   ? value(object, caller)
                                   : addressOf(object, caller);
  if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr))
    if (const auto *method =
            llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
        method && !method->isStatic() && op->getNumArgs())
      sites_[index].receiver = addressOf(op->getArg(0), caller);
  if (const auto *direct = callee) {
    sites_[index].direct = true;
    sites_[index].unresolved |= resolved.virtualDispatch;
    sites_[index].virtualDispatch = resolved.virtualDispatch;
    if (!calleeId.isValid()) {
      sites_[index].unresolved = true;
      limitations_.insert(metadata_.intern("direct call without Clang USR"));
    } else {
      auto &function = functionAt(calleeId);
      recordModel(*direct, function, model);
      if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(direct))
        function.pureVirtual = method->isPureVirtual();
      function.name = metadata_.intern(direct->getQualifiedNameAsString());
      if (function.linkage.empty())
        function.linkage = linkageNames(direct, metadata_);
      sites_[index].directTarget = function.id;
      rememberDefinition(direct);
    }
  } else {
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
      sites_[index].callee = value(call->getCallee(), caller);
    else {
      sites_[index].callee = pta_.addNode();
      sites_[index].unresolved = true;
      pta_.markUnknown(sites_[index].result);
    }
  }
  if (dataCapture_) {
    std::vector<std::uint32_t> pointers;
    for (const auto &arg : sites_[index].args)
      pointers.push_back(arg.kind == ParamKind::Other ? NoDataIndex : arg.node);
    dataCapture_->call(expr, CallSiteId{index + 1}, args, std::move(pointers),
                       sites_[index].receiver.value_or(NoDataIndex), callee);
  }
  if (const auto *assignment = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr);
      assignment && assignment->getOperator() == clang::OO_Equal &&
      assignment->getNumArgs() == 2) {
    const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
    if (method && method->isTrivial() &&
        (method->isCopyAssignmentOperator() ||
         method->isMoveAssignmentOperator()))
      copyAggregate(addressOf(assignment->getArg(0), caller),
                    addressOf(assignment->getArg(1), caller),
                    *method->getParent(), expr);
  }
  return index;
}

std::size_t Engine::site(const clang::VarDecl *decl,
                         const clang::CXXDestructorDecl *destructor,
                         FunctionId caller) {
  const auto key = std::make_pair(decl, caller);
  if (const auto found = siteByDecl_.find(key); found != siteByDecl_.end())
    return found->second;
  const auto &sources = *activeSources_;
  const auto index = sites_.size();
  Site siteFact{};
  siteFact.caller = caller;
  siteFact.location = callLocation(sources, decl->getLocation(), metadata_);
  siteFact.result = pta_.addNode();
  siteFact.direct = true;
  sites_.push_back(std::move(siteFact));
  siteByDecl_.emplace(key, index);
  if (destructor) {
    const auto id = usr(destructor);
    if (!id.empty()) {
      auto &function = ensureFunction(id);
      function.model = models_.query(destructor).model;
      function.name = metadata_.intern(destructor->getQualifiedNameAsString());
      if (function.linkage.empty())
        function.linkage = linkageNames(destructor, metadata_);
      sites_[index].directTarget = function.id;
      rememberDefinition(destructor);
    } else
      sites_[index].unresolved = true;
  } else
    sites_[index].unresolved = true;
  return index;
}

bool Engine::edge(std::size_t siteIndex, FunctionId callee) {
  if (!sites_.at(siteIndex).targets.insert(callee).second)
    return false;
  enqueueFallback(siteIndex);
  auto &function = functionAt(callee);
  const bool firstCaller = function.callers.empty();
  function.callers.push_back(siteIndex);
  if (firstCaller)
    markBodyDirty(callee);
  if (function.body ||
      (function.model != ExternalModel::Unknown &&
       (finalized_ || ExternalModules::isIntrinsic(function.model)))) {
    if (function.body && !function.usesModel())
      reached_.insert(callee);
    enqueueFunction(callee);
  }
  return true;
}

void Engine::enqueueFunction(FunctionId callee) {
  auto &function = functionAt(callee);
  if (function.queued)
    return;
  function.queued = true;
  pending_.push_back(callee);
}

void Engine::markBodyDirty(FunctionId id) {
  auto &function = functionAt(id);
  if (function.bodyDirty || (!function.root && function.callers.empty()))
    return;
  function.bodyDirty = true;
  dirtyBodies_.push_back(id);
}

void Engine::refreshBodyStates() {
  while (!dirtyBodies_.empty()) {
    const auto id = dirtyBodies_.front();
    dirtyBodies_.pop_front();
    auto &function = functionAt(id);
    function.bodyDirty = false;
    ++statistics_.refreshedBodyStates;
    const bool complete =
        function.body ||
        (function.model != ExternalModel::Unknown &&
         (finalized_ || ExternalModules::isIntrinsic(function.model)) &&
         (ExternalModules::summary(function.model).complete() ||
          ExternalModules::callbackArgument(function.model) >= 0));
    if (complete)
      incompleteFunctions_.erase(id);
    else
      incompleteFunctions_.insert(id);
  }
}

InterCFG &Engine::writableInterCFG() {
  if (!interCFG_.unique())
    interCFG_ = std::make_shared<InterCFG>(*interCFG_);
  return *interCFG_;
}

void Engine::attachCFG(Function &function) {
  if (!function.body || function.usesModel() || function.bodyActivated)
    return;
  const auto id = function.body->cfg;
  const auto &body = cfgs_.at(id.value() - 1);
  for (const auto &[site, position] : writableInterCFG().addBody(id, body))
    callPositions_[site].push_back(position);
}

void Engine::updateSummary(std::size_t index, bool unresolved) {
  const auto &site = sites_[index];
  const CallSiteId id{index + 1};
  const auto positions = callPositions_.find(id);
  if (positions == callPositions_.end())
    return;
  auto effects = site.externalEffects;
  bool summary = effects.effects != 0;
  for (const auto target : site.targets) {
    const auto &function = functionAt(target);
    if (function.usesModel()) {
      summary = true;
      // Applied summaries include argument-specific formatting/callback facts.
      // Re-adding a declaration's partial contract would discard refinement.
    } else if (!function.body || !interCFG_->cfg(function.body->cfg))
      unresolved = true;
  }
  if (unresolved)
    effects.effects |= ExternalModules::query().summary.effects;
  const auto desired =
      summary || unresolved ? std::optional{effects} : std::nullopt;
  for (const auto source : positions->second)
    if (interCFG_->summary(source, id) != desired)
      writableInterCFG().setSummary(source, id, desired);
}

void Engine::activate(FunctionId callee) {
  auto &function = functionAt(callee);
  ++statistics_.updatedFunctions;
  function.queued = false;
  attachCFG(function);
  while (function.connectedCallers < function.callers.size()) {
    const auto index = function.callers[function.connectedCallers++];
    auto &site = sites_[index];

    ++statistics_.connectedCalls;
    if (function.usesModel()) {
      ++statistics_.modeledCalls;
      if ((!site.direct || site.virtualDispatch) &&
          !appliedModelEffects_.emplace(index, function.model).second)
        continue;
      ++statistics_.appliedModelSummaries;
      modelEffects_->apply(site.externalCall(), function.model);
      const auto callback = ExternalModules::callbackArgument(function.model);
      if (callback >= 0 && site.pointerArgument(callback))
        callbackUsers_[site.args[callback].node].push_back(index);
      continue;
    }
    if (function.body && interCFG_->cfg(function.body->cfg)) {
      if (const auto found = callPositions_.find(CallSiteId{index + 1});
          found != callPositions_.end())
        for (const auto position : found->second)
          writableInterCFG().addConnection(position, CallSiteId{index + 1},
                                           function.body->cfg);
    }
    // The new connection drives these sparse actual/formal and return links.
    if (function.receiver) {
      if (site.receiver)
        pta_.copy(*function.receiver, *site.receiver);
      else
        pta_.markUnknown(*function.receiver);
    }
    const auto count = std::min(site.args.size(), function.params.size());
    for (std::size_t i = 0; i < count; ++i) {
      const auto &actual = site.args[i];
      const auto &formal = function.params[i];
      if (formal.kind != ParamKind::Other && actual.kind == formal.kind)
        pta_.copy(formal.node, actual.node);
      else if (formal.kind != ParamKind::Other) {
        site.unresolved = true;
        analysisLimitations_.insert(
            metadata_.intern("call argument signature mismatch"));
      }
    }
    if (site.returnsPointer)
      pta_.copy(site.result, returnNode(callee));
  }

  if (function.body && !function.usesModel() && !function.bodyActivated) {
    function.bodyActivated = true;
    auto &body = *function.body;
    ++statistics_.activatedBodies;
    pta_.replay(std::exchange(body.constraints, {}));
    analysisLimitations_.merge(body.limitations);
    body.limitations.clear();
    analysisUnknownMemoryEffects_ |= body.unknownMemoryEffects;
    for (std::size_t i = body.firstSite; i < body.endSite; ++i)
      activateSite(i);
  }
}

void Engine::activateSite(std::size_t index) {
  auto &site = sites_[index];
  site.active = true;
  enqueueFallback(index);
  if (!site.direct) {
    indirectUsers_[site.callee].push_back(index);
    pendingIndirect_.insert(index);
  } else if (site.virtualDispatch) {
    if (site.directTarget.isValid())
      virtualDispatch_.addCall(CallSiteId(index + 1), site.directTarget);
    analysisLimitations_.insert(metadata_.intern(
        "virtual dispatch may have targets outside supplied units"));
    analysisUnknownMemoryEffects_ = true;
  } else if (site.directTarget.isValid())
    edge(index, site.directTarget);
}

void Engine::recordModel(const clang::FunctionDecl &decl, Function &function,
                         ExternalModel model) {
  function.model = model;
  const auto *delegate = ExternalModules::delegate(decl, model);
  if (!delegate)
    return;
  const auto identity = usr(delegate);
  if (identity.empty())
    return;
  auto &target = ensureFunction(identity);
  recordModel(*delegate, target, models_.query(delegate).model);
  target.name = metadata_.intern(delegate->getQualifiedNameAsString());
  runtimeDelegates_[function.id] = target.id;
}

bool Engine::knownResult(const Site &site) {
  if (!site.direct || site.targets.size() != 1 || site.unresolved)
    return false;
  const auto &function = functionAt(*site.targets.begin());
  const auto property = ExternalModules::contract(function.model).guaranteed;
  return function.usesModel() && !function.conflictingDefinitions &&
         (provesProperty(function.id, RuntimeProperty::FreshResult) ||
          (property == RuntimeProperty::ReceiverResult && site.receiver) ||
          (property == RuntimeProperty::Argument0Result &&
           site.pointerArgument(0)));
}

bool Engine::provesProperty(FunctionId id, RuntimeProperty property) {
  // Catalog delegates form finite chains. Supplied bodies need their own
  // proof, and missing delegates cannot establish the requested property.
  for (;;) {
    const auto &function = functionAt(id);
    if (function.conflictingDefinitions)
      return false;
    if (!function.usesModel())
      return property == RuntimeProperty::BoundedDeletion &&
             readOnlyFunction(id);
    const auto contract = ExternalModules::contract(function.model);
    if (contract.guaranteed == property)
      return true;
    if (contract.delegated != property)
      return property == RuntimeProperty::BoundedDeletion &&
             readOnlyFunction(id);
    const auto found = runtimeDelegates_.find(id);
    if (found == runtimeDelegates_.end())
      return false;
    id = found->second;
  }
}

void Engine::enqueueFallback(std::size_t index) {
  auto &site = sites_[index];
  if (!finalized_ || site.fallbackQueued || site.fallbackApplied)
    return;
  site.fallbackQueued = true;
  pendingFallback_.push_back(index);
}

bool Engine::readOnlyFunction(FunctionId id) {
  if (const auto found = readOnlyCallbacks_.find(id);
      found != readOnlyCallbacks_.end())
    return found->second;
  // Imports have ended. Cache only queried callbacks/helpers; reject recursion
  // until there is a proof for it. No AST, new graph or body activation.
  auto &known = readOnlyCallbacks_[id];
  const auto &function = functionAt(id);
  if (function.conflictingDefinitions)
    return false;
  if (function.usesModel()) {
    constexpr auto reads =
        static_cast<std::uint32_t>(CallEffect::ReadArguments) |
        static_cast<std::uint32_t>(CallEffect::ReadGlobals) |
        static_cast<std::uint32_t>(CallEffect::InputToReturn);
    return known =
               (ExternalModules::summary(function.model).effects & ~reads) == 0;
  }
  const auto *body = function.body.get();
  if (!body || body->unknownMemoryEffects || !body->data ||
      !body->data->gaps.empty() || !body->cfg.isValid() ||
      !cfgs_.at(body->cfg.value() - 1)->graph ||
      std::any_of(body->data->memory.begin(), body->data->memory.end(),
                  [&](const auto &access) {
                    // Entry bindings initialize local parameter storage. A
                    // later store through a reference has a separate node.
                    return access.write &&
                           (access.indirect ||
                            body->data->nodes.at(access.node).kind !=
                                DataNodeKind::Parameter);
                  }))
    return false;
  // Ending a scalar lifetime is an observable callback effect even though it
  // neither invokes a function nor stores a different pointer value.
  for (const auto &block : cfgs_.at(body->cfg.value() - 1)->graph->blocks())
    for (const auto &event : block.events)
      if (event.kind == CFGEventKind::LifetimeEnds)
        return false;
  for (std::size_t i = body->firstSite; i < body->endSite; ++i) {
    if (!readOnlyCall(sites_[i]))
      return false;
  }
  return known = true;
}

bool Engine::readOnlyCall(const Site &call) {
  if (!call.direct || call.virtualDispatch || call.unresolved ||
      !call.directTarget.isValid())
    return false;
  const auto &target = functionAt(call.directTarget);
  if (call.localConstruction && call.args.empty() && target.usesModel() &&
      !target.conflictingDefinitions) {
    // Only the fresh, full-expression temporary is written. No pointer inputs
    // can expose caller-owned storage; placement construction never qualifies.
    constexpr auto local =
        static_cast<std::uint32_t>(CallEffect::BeginLifetime) |
        static_cast<std::uint32_t>(CallEffect::WriteArguments) |
        static_cast<std::uint32_t>(CallEffect::ReadArguments) |
        static_cast<std::uint32_t>(CallEffect::ReadGlobals);
    if ((ExternalModules::summary(target.model).effects & ~local) == 0)
      return true;
  }
  return readOnlyFunction(call.directTarget);
}

void Engine::checkRuntimeContracts(Site &site) {
  for (const auto target : site.targets) {
    const auto &function = functionAt(target);
    if (function.usesModel() &&
        ExternalModules::contract(function.model).delegated ==
            RuntimeProperty::BoundedDeletion) {
      site.unresolved |=
          !provesProperty(target, RuntimeProperty::BoundedDeletion);
    }

    const auto callback = ExternalModules::callbackArgument(function.model);
    if (!function.usesModel() || callback < 0)
      continue;
    bool readOnly = site.pointerArgument(callback);
    if (readOnly && pta_.isExternallyAffected(site.args[callback].node) &&
        (inventoryUnknownMemoryEffects_ || analysisUnknownMemoryEffects_ ||
         pta_.hasUnknownStore()))
      readOnly = false;
    if (readOnly) {
      const auto pointer = site.args[callback].node;
      readOnly = !pta_.isUnknown(pointer) && !pta_.pointsTo(pointer).empty();
      for (const auto object : pta_.pointsTo(pointer)) {
        const auto found = objectFunctions_.find(object);
        readOnly &=
            found != objectFunctions_.end() && readOnlyFunction(found->second);
      }
    }
    site.externalEffects.effects |=
        ExternalModules::query(function.model, site.formats, readOnly)
            .summary.effects;
    site.unresolved |= !readOnly;
  }
}

bool Engine::applyUnknownCalls() {
  bool changed = false;
  refreshBodyStates();
  // New global uncertainty revisits memory-dependent calls. Provenance-only
  // growth subsequently wakes affected users through the ordinary PTA queue.
  if (!globalUnknownObserved_ &&
      (inventoryUnknownMemoryEffects_ || analysisUnknownMemoryEffects_ ||
       pta_.hasUnknownStore())) {
    globalUnknownObserved_ = true;
    for (const auto &[node, users] : indirectUsers_)
      if (pta_.isExternallyAffected(node))
        for (const auto index : users)
          enqueueFallback(index);
    for (const auto &[node, users] : callbackUsers_)
      if (pta_.isExternallyAffected(node))
        for (const auto index : users)
          enqueueFallback(index);
  }
  for (const auto index : std::exchange(pendingFallback_, {})) {
    auto &site = sites_[index];
    site.fallbackQueued = false;
    if (!site.active || site.fallbackApplied)
      continue;
    checkRuntimeContracts(site);
    bool unknown = site.unresolved || site.targets.empty() ||
                   !site.externalEffects.complete() ||
                   (!site.direct && pta_.isExternallyAffected(site.callee) &&
                    (inventoryUnknownMemoryEffects_ ||
                     analysisUnknownMemoryEffects_ || pta_.hasUnknownStore()));
    for (const auto target : site.targets)
      unknown |= incompleteFunctions_.contains(target) ||
                 functionAt(target).conflictingDefinitions;
    if (!unknown)
      continue;

    site.fallbackApplied = true;
    site.externalEffects.effects |= ExternalModules::query().summary.effects;
    ++statistics_.unknownCalls;
    changed = true;
    const bool unknownResult = site.returnsPointer && !knownResult(site);
    const auto expose = [&](Andersen::Node pointer) {
      pta_.havoc(pointer);
      if (unknownResult) {
        pta_.copy(site.result, pointer);
        pta_.load(site.result, pointer);
      }
    };
    for (const auto &arg : site.args)
      if (arg.kind != ParamKind::Other)
        expose(arg.node);
    if (site.receiver)
      expose(*site.receiver);
    if (unknownResult)
      pta_.markUnknown(site.result);
    // A symbolic unknown callback/global effect is intentionally not a closed
    // set of callees. Do not fabricate ordinary edges to every program
    // function.
    analysisUnknownMemoryEffects_ = true;
  }

  if (changed && !globalFallbackApplied_) {
    globalFallbackApplied_ = true;
    if (modelEffects_)
      modelEffects_->unknownMemory();
    for (const auto node : externalStorage_) {
      pta_.markUnknown(node);
      pta_.havoc(node);
    }
    externalStorage_
        .clear(); // Imports have ended; the installed watchers persist.
    analysisLimitations_.insert(metadata_.intern(
        "conservative unknown-call memory and callback effects"));
  }
  return changed;
}

bool Engine::Body::TraverseTypeLoc(clang::TypeLoc type) {
  // Type syntax (decltype, template operands, noexcept specifications) is not
  // executable. Explicit VLA bounds are the exception; walk written bounds,
  // not canonical types, to avoid evaluating a reused typedef's bound twice.
  for (auto loc = type; !loc.isNull(); loc = loc.getNextTypeLoc()) {
    if (const auto decltypeLoc = loc.getAs<clang::DecltypeTypeLoc>();
        !decltypeLoc.isNull())
      skip(decltypeLoc.getUnderlyingExpr());
    if (const auto typeofLoc = loc.getAs<clang::TypeOfExprTypeLoc>();
        !typeofLoc.isNull()) {
      auto *operand = typeofLoc.getUnderlyingExpr();
      // GNU typeof evaluates variably modified operands, unlike decltype.
      if (operand->getType()->isVariablyModifiedType()) {
        if (!TraverseStmt(operand))
          return false;
      } else
        skip(operand);
    }
    if (const auto array = loc.getAs<clang::VariableArrayTypeLoc>();
        !array.isNull())
      if (!TraverseStmt(array.getSizeExpr()))
        return false;
  }
  return true;
}

bool Engine::Body::skip(clang::Stmt *statement) {
  std::vector<clang::Stmt *> pending{statement};
  while (!pending.empty()) {
    auto *node = pending.back();
    pending.pop_back();
    if (!node || !engine_.unevaluated_.insert(node).second)
      continue;
    for (auto *child : node->children())
      pending.push_back(child);
  }
  return true;
}

bool Engine::Body::TraverseUnaryExprOrTypeTraitExpr(
    clang::UnaryExprOrTypeTraitExpr *expr) {
  if (expr->getKind() != clang::UETT_SizeOf ||
      !expr->getTypeOfArgument()->isVariableArrayType())
    return expr->isArgumentType() || skip(expr->getArgumentExpr());
  return expr->isArgumentType()
             ? TraverseTypeLoc(expr->getArgumentTypeInfo()->getTypeLoc())
             : TraverseStmt(expr->getArgumentExpr());
}

bool Engine::Body::VisitVarDecl(clang::VarDecl *decl) {
  clang::Expr *initial = decl->getInit();
  if (auto *cleanup = llvm::dyn_cast_or_null<clang::ExprWithCleanups>(initial))
    initial = cleanup->getSubExpr();
  if (auto *construct =
          llvm::dyn_cast_or_null<clang::CXXConstructExpr>(initial)) {
    const auto site = constructionSite(construct);
    const auto address = engine_.pta_.addNode();
    engine_.pta_.address(address, engine_.nodeFor(decl));
    engine_.sites_[site].receiver = address;
    engine_.sites_[site].localConstruction = true;
  }
  if (engine_.dataCapture_)
    engine_.dataCapture_->variable(decl);
  if (const auto *record = decl->getType()->getAsCXXRecordDecl();
      record && !record->hasTrivialDestructor()) {
    engine_.site(decl, record->getDestructor(), function_);
  }
  if (decl->getType()->isReferenceType() && decl->hasInit())
    engine_.pta_.copy(engine_.nodeFor(decl),
                      engine_.addressOf(decl->getInit(), function_));
  if (decl->getType()->isPointerType() && decl->hasInit()) {
    const auto definition = engine_.localFlow_->initialize(decl).pointer;
    engine_.pta_.copy(definition ? *definition : engine_.nodeFor(decl),
                      engine_.value(decl->getInit(), function_));
  }
  engine_.initializeAggregate(decl, function_);
  return true;
}

bool Engine::Body::VisitBinaryOperator(clang::BinaryOperator *expr) {
  if (engine_.dataCapture_)
    engine_.dataCapture_->assignment(expr);
  if (expr->getOpcode() == clang::BO_Assign) {
    if (expr->getLHS()->getType()->isRecordType()) {
      engine_.limitations_.insert(
          engine_.metadata_.intern("record assignment effects not modeled"));
      engine_.unknownMemoryEffects_ = true;
    } else
      engine_.assign(expr->getLHS(), expr->getRHS(), function_);
  } else if (expr->isCompoundAssignmentOp() &&
             expr->getLHS()->getType()->isPointerType())
    engine_.invalidatePointer(
        expr->getLHS(), "pointer compound assignment not modeled", function_);
  return true;
}

bool Engine::Body::VisitUnaryOperator(clang::UnaryOperator *expr) {
  if (engine_.dataCapture_)
    engine_.dataCapture_->increment(expr);
  if (expr->isIncrementDecrementOp() &&
      expr->getSubExpr()->getType()->isPointerType())
    engine_.invalidatePointer(expr->getSubExpr(),
                              "pointer increment/decrement not modeled",
                              function_);
  return true;
}

bool Engine::Body::VisitReturnStmt(clang::ReturnStmt *stmt) {
  const auto type = engine_.activeDefinition_->getReturnType();
  if (const auto *result = stmt->getRetValue();
      result &&
      (result->getType()->isPointerType() || type->isReferenceType())) {
    engine_.pta_.copy(engine_.returnNode(function_),
                      type->isReferenceType()
                          ? engine_.addressOf(result, function_)
                          : engine_.value(result, function_));
  }
  return true;
}

bool Engine::Body::VisitCallExpr(clang::CallExpr *expr) {
  // Scalar pseudo-destruction ends a lifetime; it has no callee. Traversal
  // still evaluates the base, including any ordinary calls in that operand.
  if (!invokesFunction(expr))
    return true;
  engine_.site(expr, function_);
  return true;
}

std::size_t Engine::Body::constructionSite(clang::CXXConstructExpr *expr) {
  std::vector<const clang::Expr *> args;
  for (const auto *arg : expr->arguments())
    args.push_back(arg);
  return engine_.site(expr, expr->getConstructor(), std::move(args), function_);
}

bool Engine::Body::VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
  constructionSite(expr);
  return true;
}

bool Engine::Body::VisitMaterializeTemporaryExpr(
    clang::MaterializeTemporaryExpr *expr) {
  if (expr->getStorageDuration() != clang::SD_FullExpression)
    return true;
  // Inspect only this materialized object, without building an AST parent map
  // or accepting construction nested in an unrelated expression.
  auto *value = expr->getSubExpr()->IgnoreParenImpCasts();
  if (auto *bound = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(value))
    value = bound->getSubExpr()->IgnoreParenImpCasts();
  if (auto *constructed = llvm::dyn_cast<clang::CXXConstructExpr>(value)) {
    const auto index = constructionSite(constructed);
    engine_.sites_[index].localConstruction = true;
  }
  return true;
}

bool Engine::Body::VisitCXXNewExpr(clang::CXXNewExpr *expr) {
  engine_.site(expr, function_);
  return true;
}

bool Engine::Body::VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
  engine_.site(expr, expr->getOperatorDelete(), {expr->getArgument()},
               function_);
  const auto destroyedType = expr->getDestroyedType();
  if (destroyedType.isNull()) {
    engine_.limitations_.insert(engine_.metadata_.intern(
        "delete expression destroyed type unavailable"));
    engine_.unknownMemoryEffects_ = true;
  } else if (const auto *record = destroyedType->getAsCXXRecordDecl()) {
    if (const auto *destructor = record->getDestructor())
      engine_.site(expr, destructor, {}, function_);
    else {
      engine_.limitations_.insert(
          engine_.metadata_.intern("delete expression destructor unavailable"));
      engine_.unknownMemoryEffects_ = true;
    }
  }
  return true;
}

bool Engine::Body::VisitCXXBindTemporaryExpr(
    clang::CXXBindTemporaryExpr *expr) {
  engine_.site(expr, expr->getTemporary()->getDestructor(), {}, function_);
  return true;
}

// The visitor and current CFG options do not expand these implicit expressions.
// They can call functions or mutate pointers even when their result is scalar.
bool Engine::Body::VisitCXXDefaultArgExpr(clang::CXXDefaultArgExpr *) {
  engine_.limitations_.insert(
      engine_.metadata_.intern("default argument effects not modeled"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitCXXDefaultInitExpr(clang::CXXDefaultInitExpr *) {
  engine_.limitations_.insert(engine_.metadata_.intern(
      "default member initializer effects not modeled"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitCXXThrowExpr(clang::CXXThrowExpr *) {
  engine_.limitations_.insert(
      engine_.metadata_.intern("exception effects not indexed"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitGCCAsmStmt(clang::GCCAsmStmt *) {
  engine_.limitations_.insert(
      engine_.metadata_.intern("inline assembly effects not indexed"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitMSAsmStmt(clang::MSAsmStmt *) {
  engine_.limitations_.insert(
      engine_.metadata_.intern("inline assembly effects not indexed"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitAtomicExpr(clang::AtomicExpr *) {
  engine_.limitations_.insert(
      engine_.metadata_.intern("atomic memory effects not indexed"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitCoroutineBodyStmt(clang::CoroutineBodyStmt *) {
  engine_.limitations_.insert(
      engine_.metadata_.intern("coroutine effects not indexed"));
  engine_.unknownMemoryEffects_ = true;
  return true;
}

bool Engine::Body::VisitDeclRefExpr(clang::DeclRefExpr *expr) {
  if (expr->getType()->isPointerType() &&
      expr->getDecl()->getType().isVolatileQualified()) {
    engine_.limitations_.insert(
        engine_.metadata_.intern("volatile pointer access not modeled"));
    engine_.pta_.markUnknown(engine_.nodeFor(expr->getDecl()));
    engine_.unknownMemoryEffects_ = true;
  }
  return true;
}

bool Engine::Body::TraverseLambdaExpr(clang::LambdaExpr *expr) {
  if (expr->capture_size() && !engine_.initializedLambdas_.contains(expr)) {
    engine_.limitations_.insert(engine_.metadata_.intern(
        "temporary lambda capture storage not modeled"));
    engine_.unknownMemoryEffects_ = true;
  }
  for (auto *initial : expr->capture_inits())
    if (!TraverseStmt(initial))
      return false;
  return true; // The call operator has its own CFG and extraction scope.
}

void Engine::extractBody(const clang::FunctionDecl &definition,
                         FunctionId function) {
  if (const auto *constructor =
          llvm::dyn_cast<clang::CXXConstructorDecl>(&definition)) {
    for (const auto *initializer : constructor->inits()) {
      if (initializer->isMemberInitializer()) {
        const auto *field = initializer->getMember();
        const auto address =
            pta_.project(receiverNode(function), nodeFor(field));
        if (dataCapture_)
          dataCapture_->field(address, initializer->getInit());
        if (field->getType()->isPointerType())
          pta_.store(address, value(initializer->getInit(), function));
        else if (field->getType()->isReferenceType() &&
                 field->getType()->getPointeeType()->isPointerType())
          pta_.store(address, addressOf(initializer->getInit(), function));
      }
      Body(*this, function).TraverseStmt(initializer->getInit());
    }
  }
  for (const auto *param : definition.parameters()) {
    if (const auto *record = param->getType()->getAsCXXRecordDecl();
        record && !record->hasTrivialDestructor()) {
      limitations_.insert(metadata_.intern(
          "nontrivial parameter destructor effects not indexed"));
      unknownMemoryEffects_ = true;
    }
  }
  Body(*this, function).TraverseStmt(definition.getBody());
}

void Engine::extractFunctionFacts(const clang::FunctionDecl &definition,
                                  clang::CFG *native, FunctionId function,
                                  BodyFacts &body) {
  if (native)
    body.data = std::make_unique<FunctionDataFlow>();
  const BodyId id{cfgs_.size() + 1};
  localFlow_.emplace(
      definition, native, pta_,
      [this](const clang::ValueDecl *decl) { return nodeFor(decl); },
      body.data.get(), id);
  if (body.data) {
    dataCapture_.emplace(
        *body.data, *localFlow_, id,
        [this, function](const clang::Expr *expr) {
          return value(expr, function);
        },
        [this](const clang::ValueDecl *decl) { return nodeFor(decl); },
        [this, function](const clang::Expr *expr) {
          return addressOf(expr, function);
        },
        [this, function](const clang::ValueDecl *decl) {
          return captureAddress(decl, function);
        });
    dataCapture_->parameters(definition);
  }
  extractBody(definition, function);
  if (dataCapture_ && unknownMemoryEffects_)
    dataCapture_->unknown();
  if (observe_ && dataCapture_)
    captureFacts_ =
        observe_(definition, *native, id, *dataCapture_, *localFlow_);
}

void Engine::observeElement(const clang::CFGElement &element,
                            CFGPosition position, FunctionId function) {
  if (!captureFacts_)
    return;
  const auto statement = element.getAs<clang::CFGStmt>();
  const auto *expr =
      statement ? llvm::dyn_cast<clang::Expr>(statement->getStmt()) : nullptr;
  if (expr && unevaluated_.contains(expr))
    return;
  CallSiteId site;
  if (expr) {
    const auto found = siteByExpr_.lower_bound({expr, function, FunctionId{}});
    if (found != siteByExpr_.end() && std::get<0>(found->first) == expr &&
        std::get<1>(found->first) == function)
      site = CallSiteId{found->second + 1};
  }
  captureFacts_(element, position, site);
}

void Engine::captureCFG(const clang::FunctionDecl &definition,
                        FunctionId function, BodyFacts &body) {
  auto captured = std::make_shared<CapturedCFG>();
  captured->function = function;
  captured->unit = activeUnitId_;
  captured->definition = definitionLocation(&definition, metadata_);
  std::vector<bool> matched;
  const auto bind = [&](std::size_t index, PointId point) {
    if (index < body.firstSite || index >= sites_.size())
      return false;
    captured->calls.push_back({CallSiteId{index + 1}, point});
    matched[index - body.firstSite] = true;
    return true;
  };
  const auto bindExpression = [&](const clang::Expr *expr, PointId point,
                                  const clang::FunctionDecl *onlyTarget =
                                      nullptr) {
    if (onlyTarget) {
      const auto found =
          siteByExpr_.find({expr, function, findFunction(usr(onlyTarget))});
      return found != siteByExpr_.end() && bind(found->second, point);
    }
    bool foundSite = false;
    auto found = siteByExpr_.lower_bound({expr, function, FunctionId{}});
    while (found != siteByExpr_.end() && std::get<0>(found->first) == expr &&
           std::get<1>(found->first) == function) {
      foundSite |= bind(found->second, point);
      ++found;
    }
    return foundSite;
  };
  const auto capture = [&](const clang::CFGElement &element, PointId point,
                           BlockId block, std::uint64_t offset) {
    observeElement(element, {BodyId{cfgs_.size() + 1}, block, offset},
                   function);
    bool isCall = false, linked = false;
    const auto firstCall = captured->calls.size();
    if (const auto statement = element.getAs<clang::CFGStmt>()) {
      const auto *expr = llvm::dyn_cast<clang::Expr>(statement->getStmt());
      if (!expr || llvm::isa<clang::CXXBindTemporaryExpr>(expr))
        return;
      if (unevaluated_.contains(expr)) {
        captured->unevaluatedPoints.push_back(point);
        return;
      }
      isCall = invokesFunction(expr);
      if (!isCall)
        return;
      if (const auto *deleted = llvm::dyn_cast<clang::CXXDeleteExpr>(expr))
        linked = bindExpression(expr, point, deleted->getOperatorDelete());
      else
        linked = bindExpression(expr, point);
    } else if (const auto automatic =
                   element.getAs<clang::CFGAutomaticObjDtor>()) {
      isCall = true;
      const auto found = siteByDecl_.find({automatic->getVarDecl(), function});
      linked = found != siteByDecl_.end() && bind(found->second, point);
    } else if (const auto temporary =
                   element.getAs<clang::CFGTemporaryDtor>()) {
      if (unevaluated_.contains(temporary->getBindTemporaryExpr())) {
        captured->unevaluatedPoints.push_back(point);
        return;
      }
      isCall = true;
      linked = bindExpression(
          temporary->getBindTemporaryExpr(), point,
          temporary->getDestructorDecl(definition.getASTContext()));
    } else if (const auto deleted = element.getAs<clang::CFGDeleteDtor>()) {
      isCall = true;
      linked = bindExpression(
          deleted->getDeleteExpr(), point,
          deleted->getDestructorDecl(definition.getASTContext()));
    } else if (element.getAs<clang::CFGImplicitDtor>() ||
               element.getAs<clang::CFGCleanupFunction>() ||
               element.getAs<clang::CFGNewAllocator>()) {
      isCall = true;
    }
    if (isCall && !linked)
      captured->unmodeledCalls.push_back(point);
    if (dataCapture_ && element.getAs<clang::CFGImplicitDtor>()) {
      const auto automatic = element.getAs<clang::CFGAutomaticObjDtor>();
      for (auto i = firstCall; i < captured->calls.size(); ++i)
        dataCapture_->destructor(captured->calls[i].call,
                                 {BodyId{cfgs_.size() + 1}, block, offset},
                                 automatic ? automatic->getVarDecl() : nullptr);
    }
  };
  const auto prepareFacts = [&](clang::CFG *native) {
    extractFunctionFacts(definition, native, function, body);
    matched.resize(sites_.size() - body.firstSite);
  };
  auto graph = ClangCFGLifter{}.lift(definition, definition.getASTContext(),
                                     function, capture, prepareFacts);
  finishCFGCapture(std::move(graph), std::move(captured), body,
                   std::move(matched));
}

void Engine::finishCFGCapture(llvm::Expected<FunctionCFG> graph,
                              std::shared_ptr<CapturedCFG> captured,
                              BodyFacts &body, std::vector<bool> matched) {
  captureFacts_ = {};
  dataCapture_.reset();
  if (localFlow_) {
    cfgLocalVariables_ += localFlow_->variables();
    cfgLocalFallbacks_ += localFlow_->fallbacks();
    localFlow_.reset();
  }
  // Call/allocation lookups are needed only through this body's CFG callbacks.
  // Release them before extracting another body, including after a failed lift.
  siteByExpr_.clear();
  siteByDecl_.clear();
  pointerValues_.clear();
  initializedLambdas_.clear();
  unevaluated_.clear();
  if (graph) {
    captured->graph.emplace(std::move(*graph));
  } else {
    ++failedCFGs_;
    limitations_.insert(metadata_.intern("CFG construction failed: " +
                                         llvm::toString(graph.takeError())));
    unknownMemoryEffects_ = true;
    captured->calls.clear();
    captured->unmodeledCalls.clear();
    std::fill(matched.begin(), matched.end(), false);
  }
  const BodyId bodyId{cfgs_.size() + 1};
  body.cfg = bodyId;
  for (std::size_t i = body.firstSite; i < sites_.size(); ++i) {
    sites_[i].body = bodyId;
    if (!matched[i - body.firstSite])
      captured->unmappedCalls.push_back(CallSiteId{i + 1});
  }
  if (!captured->unmodeledCalls.empty()) {
    limitations_.insert(metadata_.intern("CFG call effects not captured"));
    unknownMemoryEffects_ = true;
  }
  if (!captured->unmappedCalls.empty())
    limitations_.insert(metadata_.intern("call facts without CFG occurrences"));
  cfgs_.push_back(std::move(captured));
}

void Engine::extract(clang::ASTContext &unit) {
  if (finalized_)
    throw std::logic_error("cannot import ASTs after CG/PTA finalization");
  overlayMayChange_ = true;
  activeUnitId_ = SourceUnitId{++nextUnitId_};
  limitations_.clear();
  unknownMemoryEffects_ = false;
  Inventory(*this).TraverseDecl(unit.getTranslationUnitDecl());
  pta_.beginCapture();
  for (const auto *global : globals_) {
    activeSources_ = &global->getASTContext().getSourceManager();
    if (global->getType()->isPointerType()) {
      if (global->hasConstantInitialization())
        pta_.copy(nodeFor(global), value(global->getInit(), FunctionId{}));
      else
        pta_.markUnknown(nodeFor(global));
    }
    if (global->hasConstantInitialization())
      initializeAggregate(global, FunctionId{});
  }
  pointerValues_.clear();
  initializedLambdas_.clear();
  auto globalBatch = pta_.endCapture();
  globalFacts_.insert(globalFacts_.end(), globalBatch.begin(),
                      globalBatch.end());
  inventoryLimitations_.insert(limitations_.begin(), limitations_.end());
  inventoryUnknownMemoryEffects_ |= unknownMemoryEffects_;
  limitations_.clear();
  unknownMemoryEffects_ = false;
  for (std::size_t definitionIndex = 0;
       definitionIndex < currentDefinitions_.size(); ++definitionIndex) {
    const auto *definition = currentDefinitions_[definitionIndex];
    const auto id = findFunction(usr(definition));
    auto &function = functionAt(id);
    if (function.body)
      continue;
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(definition);
        method && !method->isStatic())
      receiverNode(id);
    for (const auto *param : definition->parameters()) {
      const auto kind = paramKind(param->getType());
      function.params.push_back(
          {kind == ParamKind::Other ? Andersen::Node{} : nodeFor(param), kind});
    }
    if (definition->isExternallyVisible() ||
        functionObjects_.contains(function.id))
      markExternalInputs(function);
    activeDefinition_ = definition;
    activeSources_ = &definition->getASTContext().getSourceManager();
    BodyFacts body;
    body.firstSite = sites_.size();
    pta_.beginCapture();
    captureCFG(*definition, id, body);
    body.constraints = pta_.endCapture();
    body.endSite = sites_.size();
    body.limitations = std::move(limitations_);
    body.unknownMemoryEffects = unknownMemoryEffects_;
    if (const auto root = rootDefinitions_.find(function.name);
        root != rootDefinitions_.end())
      root->second.insert(function.id);
    function.body = std::make_unique<BodyFacts>(std::move(body));
    markBodyDirty(function.id);
    if (reached_.contains(function.id) || !function.callers.empty()) {
      reached_.insert(function.id);
      enqueueFunction(function.id);
    }
    limitations_.clear();
    unknownMemoryEffects_ = false;
  }
  activeDefinition_ = nullptr;
  activeSources_ = nullptr;
  globals_.clear();
  currentDefinitions_.clear();
  currentSeenDefinitions_.clear();
  localNodes_.clear();
  siteByExpr_.clear();
  siteByDecl_.clear();
  pointerValues_.clear();
  initializedLambdas_.clear();
  activeUnitId_ = {};
}

llvm::Error Engine::beginRun(const std::vector<std::string> &roots,
                             bool finalize) {
  if (roots.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "at least one root is required");
  std::set<TextId> requestedRoots;
  for (const auto &root : roots)
    requestedRoots.insert(metadata_.intern(root));
  if (!roots_.empty() && roots_ != requestedRoots)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "a CG/PTA session requires a fixed root set");
  if (roots_.empty()) {
    rootDefinitions_.clear();
    for (const auto &root : requestedRoots)
      rootDefinitions_.try_emplace(root);
    // Resolve the initial roots once. Later imports update only matching names.
    for (const auto &function : functions_)
      if (function.body)
        if (const auto root = rootDefinitions_.find(function.name);
            root != rootDefinitions_.end())
          root->second.insert(function.id);
  }
  std::vector<FunctionId> rootIds;
  for (const auto &root : roots) {
    const auto found = rootDefinitions_.find(metadata_.find(root));
    if (found->second.empty())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "missing root definition: %s",
                                     root.c_str());
    if (found->second.size() != 1)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "ambiguous root: %s", root.c_str());
    rootIds.push_back(*found->second.begin());
  }
  roots_ = requestedRoots;
  overlayMayChange_ |= finalize && !finalized_;
  if (finalize && !finalized_) {
    finalized_ = true;
    // These indexes serve AST extraction only. The solver uses the numeric
    // records and objectFunctions_ reverse lookup; imports are now forbidden.
    functionIds_.clear();
    declarationNodes_.clear();
    functionObjects_.clear();
    std::vector<const clang::VarDecl *>().swap(globals_);
    std::vector<const clang::FunctionDecl *>().swap(currentDefinitions_);
    for (const auto &function : functions_)
      if (!function.body && function.model != ExternalModel::Unknown &&
          function.connectedCallers < function.callers.size()) {
        markBodyDirty(
            function.id); // A complete runtime contract is now available.
        enqueueFunction(function.id);
      }
    for (std::size_t i = 0; i < sites_.size(); ++i)
      if (sites_[i].active)
        enqueueFallback(i);
  }
  pta_.replay(std::exchange(globalFacts_, {}));
  for (const auto &root : rootIds) {
    auto &function = functionAt(root);
    if (!function.root) {
      function.root = true;
      if (function.receiver)
        pta_.markUnknown(*function.receiver);
      markBodyDirty(root);
      if (reached_.insert(root).second)
        enqueueFunction(root);
      for (const auto &param : function.params)
        if (param.kind != ParamKind::Other)
          pta_.markUnknown(param.node);
    }
  }

  return llvm::Error::success();
}

llvm::Error Engine::solve() {
  for (std::size_t iteration = 0; iteration < 10000; ++iteration) {
    while (!pending_.empty()) {
      const auto id = pending_.front();
      pending_.pop_front();
      activate(id);
    }
    for (const auto target : virtualDispatch_.takeTargets()) {
      ++statistics_.resolvedVirtualTargets;
      // Preserve the hierarchy traversal, but a pure slot is not an executable
      // dynamic target. Qualified direct calls still use edge() normally.
      if (!functionAt(target.function).pureVirtual)
        edge(target.site.value() - 1, target.function);
    }
    bool pointerFixedPoint = false;
    for (std::size_t pass = 0; pass < 10000; ++pass) {
      pointerFixedPoint = pta_.solve(1);
      if (std::getenv("WPGKIT_WPG_PROGRESS"))
        llvm::errs() << "CG/PTA round " << iteration << ", pass " << pass
                     << ": " << pta_.nodeCount() << " nodes; "
                     << pta_.objectCount() << " addressed objects; "
                     << pta_.constraintCount() << " constraints; "
                     << pta_.targetRelations()
                     << " target relations; largest set "
                     << pta_.largestTargetSet() << "; " << pta_.watcherCount()
                     << " watchers; " << statistics_.connectedCalls
                     << " call edges; " << statistics_.unknownCalls
                     << " unknown fallbacks\n";
      if (pointerFixedPoint)
        break;
    }
    if (!pointerFixedPoint)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "Andersen pass budget exhausted");
    bool newEdge = false;
    for (const auto node : pta_.takeChangedNodes()) {
      if (const auto found = indirectUsers_.find(node);
          found != indirectUsers_.end())
        pendingIndirect_.insert(found->second.begin(), found->second.end());
      if (const auto found = callbackUsers_.find(node);
          found != callbackUsers_.end())
        for (const auto index : found->second)
          enqueueFallback(index);
    }
    for (const auto i : std::exchange(pendingIndirect_, {})) {
      ++statistics_.resolvedIndirectCalls;
      enqueueFallback(i);
      if (pta_.isUnknown(sites_[i].callee))
        sites_[i].unresolved = true;
      for (const auto object : pta_.pointsTo(sites_[i].callee)) {
        if (const auto found = objectFunctions_.find(object);
            found != objectFunctions_.end())
          newEdge |= edge(i, found->second);
        else
          sites_[i].unresolved = true;
      }
    }
    if (!newEdge && pending_.empty() && !(finalized_ && applyUnknownCalls()))
      break;
    if (iteration == 9999)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "call graph iteration budget exhausted");
  }
  return llvm::Error::success();
}

bool Engine::unresolved(const Site &site, bool unknownEffects) {
  if (site.unresolved || site.targets.empty() ||
      (!site.direct && pta_.isExternallyAffected(site.callee) &&
       (unknownEffects || pta_.hasUnknownStore())))
    return true;
  return std::any_of(site.targets.begin(), site.targets.end(),
                     [&](FunctionId target) {
                       return incompleteFunctions_.contains(target) ||
                              functionAt(target).conflictingDefinitions;
                     });
}

CallGraphResult Engine::snapshot(bool consume) {
  CallGraphResult result;
  result.finalized = finalized_;
  refreshBodyStates();
  result.limitations = inventoryLimitations_;
  result.limitations.insert(analysisLimitations_.begin(),
                            analysisLimitations_.end());
  bool unknownEffects = inventoryUnknownMemoryEffects_ ||
                        analysisUnknownMemoryEffects_ ||
                        !incompleteFunctions_.empty();
  for (const auto id : reached_)
    if (functionAt(id).conflictingDefinitions) {
      result.limitations.insert(
          metadata_.intern("conflicting function definitions"));
      result.limitations.insert(
          metadata_.intern("conflicting function definitions: " +
                           metadata_.text(functionAt(id).name)));
      unknownEffects = true;
    }
  // Unavailable bodies are provisional, unlike intrinsic unknown pointer facts.
  result.unknownMemoryEffects = unknownEffects;
  // Keep their effects in the result's open boundary rather than permanently
  // tainting the monotone solver. Memory-dependent indirect calls remain open
  // while reachable effects are missing. Local values derived only from
  // known function addresses keep their complete target sets.
  for (const auto id : incompleteFunctions_) {
    const auto &function = functionAt(id);
    result.unresolvedLinkedSymbols.insert(function.linkage.begin(),
                                          function.linkage.end());
    result.unresolvedBodies.insert(id);
    result.limitations.insert(
        metadata_.intern("call target definition unavailable"));
  }
  for (const auto &function : functions_) {
    const auto id = function.id;
    result.names.emplace(id, function.name);
    result.usrs.emplace(id, function.usr);
    if (function.usesModel())
      result.externalModels.emplace(id, function.model);
  }
  result.reachable = consume ? std::move(reached_) : reached_;
  if (pta_.hasUnknownStore())
    result.limitations.insert(
        metadata_.intern("store through unknown address"));
  for (std::size_t i = 0; i < sites_.size(); ++i) {
    auto &site = sites_[i];
    if (!site.active)
      continue;
    const CallSiteId id(i + 1);
    result.locations.emplace(id, site.location);
    const bool open = unresolved(site, unknownEffects);
    if (overlayMayChange_)
      updateSummary(i, open);
    result.calls.push_back({id, site.caller,
                            consume ? std::move(site.targets) : site.targets,
                            open, site.body, site.externalEffects});
  }
  statistics_.pointerConstraints = pta_.constraintCount();
  statistics_.processedPointerConstraints = pta_.processedConstraints();
  result.pointerStorage = {pta_.nodeCount(), pta_.objectCount(),
                           pta_.targetRelations(), pta_.largestTargetSet(),
                           pta_.watcherCount()};
  result.pointerStorage.cfgLocalVariables = cfgLocalVariables_;
  result.pointerStorage.cfgLocalFallbacks = cfgLocalFallbacks_;
  result.statistics = statistics_;
  result.cfgs = consume ? std::move(cfgs_) : cfgs_;
  result.metadata = consume ? std::move(metadata_) : metadata_;
  if (overlayMayChange_) {
    const bool closed = result.closedWithinInputs();
    if (interCFG_->inputClosed() != closed ||
        interCFG_->inputFinalized() != finalized_)
      writableInterCFG().setInputStatus(closed, finalized_);
    if (interCFG_.unique())
      interCFG_->prepare();
  }
  result.interCFG = interCFG_;
  return result;
}

void Engine::buildDataFlow(CallGraphResult &result, bool consume) {
  if (overlayMayChange_ || !dataFlow_) {
    std::vector<DataFlowInput> inputs;
    for (const auto &function : functions_)
      if (function.body)
        inputs.push_back({function.body->cfg, function.body->data.get()});
    dataFlow_ = DataFlowBuilder::build(result, inputs, pta_, consume);
  }
  result.dataFlow = dataFlow_;
  overlayMayChange_ = false;
}

llvm::Expected<CallGraphResult>
Engine::run(const std::vector<std::string> &roots, bool finalize,
            bool consume) {
  CallGraphResult result;
  {
    Profiler::Scope phase(profiler_, "cg_pta");
    if (auto error = beginRun(roots, finalize))
      return std::move(error);
    if (auto error = solve())
      return std::move(error);
    result = snapshot(consume);
  }
  {
    Profiler::Scope phase(profiler_, "dfg");
    buildDataFlow(result, consume);
  }
  return result;
}

} // namespace

struct CallGraphSession::Impl final {
  explicit Impl(Profiler *profiler, FunctionFactObserver observe,
                ExternalModules models)
      : engine(profiler, std::move(observe), models), profiler(profiler) {}

  Engine engine;
  Profiler *profiler = nullptr;
};

CallGraphSession::CallGraphSession(Profiler *profiler,
                                   FunctionFactObserver observe,
                                   ExternalModules models)
    : impl_(std::make_unique<Impl>(profiler, std::move(observe), models)) {}

CallGraphSession::~CallGraphSession() = default;
CallGraphSession::CallGraphSession(CallGraphSession &&) noexcept = default;
CallGraphSession &
CallGraphSession::operator=(CallGraphSession &&) noexcept = default;

void CallGraphSession::addUnit(clang::ASTContext &unit) {
  Profiler::Scope phase(impl_->profiler, "fact_extraction");
  impl_->engine.extract(unit);
}

std::size_t CallGraphSession::cfgCount() const {
  return impl_->engine.cfgCount();
}

std::size_t CallGraphSession::failedCFGCount() const {
  return impl_->engine.failedCFGCount();
}

llvm::Expected<CallGraphResult>
CallGraphSession::analyze(const std::vector<std::string> &qualifiedRoots) {
  return impl_->engine.run(qualifiedRoots);
}

llvm::Expected<CallGraphResult>
CallGraphSession::finalize(const std::vector<std::string> &qualifiedRoots) & {
  return impl_->engine.run(qualifiedRoots, true);
}

llvm::Expected<CallGraphResult>
CallGraphSession::finalize(const std::vector<std::string> &qualifiedRoots) && {
  auto owned = std::move(impl_);
  return owned->engine.run(qualifiedRoots, true, true);
}

llvm::Expected<CallGraphResult>
CallGraphPTA::run(const std::vector<clang::ASTContext *> &units,
                  const std::vector<std::string> &qualifiedRoots,
                  Profiler *profiler, FunctionFactObserver observe,
                  ExternalModules models) {
  Profiler::Scope phase(profiler, "analysis");
  if (units.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "at least one unit is required");
  CallGraphSession session(profiler, std::move(observe), models);
  for (auto *unit : units) {
    if (!unit)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "null AST context");
    session.addUnit(*unit);
  }
  return std::move(session).finalize(qualifiedRoots);
}

bool CallGraphResult::closedWithinInputs() const {
  if (!limitations.empty() || !unresolvedBodies.empty())
    return false;
  return std::all_of(
      calls.begin(), calls.end(),
      [](const CallSiteResult &site) { return !site.unresolved; });
}

} // namespace wpgkit
