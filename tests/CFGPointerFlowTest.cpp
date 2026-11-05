#include "CallGraphPTA.h"
#include "InterCFG.h"
#include "LocalDataFlow.h"
#include "ClangCFGLifter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <tuple>

using namespace wpgkit;

namespace {
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void add(CallGraphSession &session, const std::string &code,
         const char *file = "flow.cpp") {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"}, file);
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse CFG pointer fixture");
  session.addUnit(ast->getASTContext());
} // Solver must work after the AST is destroyed.

CallGraphResult solve(CallGraphSession &session, bool final = true) {
  auto result =
      final ? session.finalize({"entry"}) : session.analyze({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError());
    std::exit(1);
  }
  return std::move(*result);
}

std::vector<std::set<std::string>> targets(const CallGraphResult &result,
                                           const char *caller = "entry") {
  std::vector<const CallSiteResult *> sites;
  for (const auto &site : result.calls)
    if (result.metadata.text(result.names.at(site.caller)) == caller)
      sites.push_back(&site);
  std::sort(sites.begin(), sites.end(), [&](auto a, auto b) {
    return result.locations.at(a->id).expansion.offset <
           result.locations.at(b->id).expansion.offset;
  });
  std::vector<std::set<std::string>> values;
  for (const auto *site : sites) {
    auto &value = values.emplace_back();
    for (const auto id : site->targets)
      value.insert(result.metadata.text(result.names.at(id)));
  }
  return values;
}

CallGraphResult fixture(const char *body) {
  CallGraphSession session;
  add(session,
      std::string("using P=void(*)(); void a(){} void b(){} void c(){} ") +
          body);
  auto result = solve(session);
  check(result.interCFG && result.interCFG->inputFinalized(),
        "return converged incremental overlay");
  check(result.pointerStorage.cfgLocalVariables > 0,
        "fixture exercises sparse local flow");
  const auto again = solve(session);
  check(
      again.statistics == result.statistics &&
          again.interCFG.get() == result.interCFG.get(),
      "a repeated solve neither replays constraints nor duplicates connections");
  return result;
}

void connections(const InterCFG &graph) {
  for (std::size_t i = 0; i < graph.connections().size(); ++i) {
    const auto id = CallConnectionId{i + 1};
    const auto &call = *graph.connection(id);
    const auto edges = graph.successors(call.source);
    check(std::count_if(edges.begin(), edges.end(),
                        [&](const auto &edge) {
                          return edge.kind == InterCFGEdgeKind::Call &&
                                 edge.connection == id;
                        }) == 1,
          "one call edge per stable connection");
    const auto returns = graph.successors(*graph.exit(call.callee));
    auto continuation = call.source;
    ++continuation.offset;
    check(std::count_if(returns.begin(), returns.end(),
                        [&](const auto &edge) {
                          return edge.kind ==
                                     InterCFGEdgeKind::ReturnOrUnwind &&
                                 edge.connection == id &&
                                 edge.target == continuation;
                        }) == 1,
          "return retains its matching connection");
  }
}

// Storage regressions: a read must reuse its reaching definition; branch-only
// forwarding must not allocate points-to sets. This checks the memory invariant
// independently of whole-program extraction's other pointer nodes.
void checkLocalStorage(const char *code, std::size_t definitions,
                       std::size_t joins) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"},
                                                      "storage.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse pointer storage fixture");
  const clang::FunctionDecl *function = nullptr;
  for (const auto *decl :
       ast->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl);
        candidate && candidate->hasBody())
      function = candidate;
  check(function != nullptr, "storage fixture has a body");
  Andersen solver;
  unsigned baseAllocations = 0;
  auto graph = ClangCFGLifter{}.lift(
      *function, ast->getASTContext(), FunctionId{1}, {}, [&](clang::CFG *cfg) {
        check(cfg != nullptr, "storage fixture has a native CFG");
        LocalDataFlow flow(*function, cfg, solver,
                           [&](const clang::ValueDecl *) {
                             ++baseAllocations;
                             return solver.addNode();
                           });
        check(flow.fallbacks() == 0, "all storage fixture uses map to the CFG");
        struct Uses final : clang::RecursiveASTVisitor<Uses> {
          LocalDataFlow &flow;
          std::set<Andersen::Node> readNodes;
          explicit Uses(LocalDataFlow &flow) : flow(flow) {}
          bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
            if (const auto node = flow.read(expr).pointer)
              readNodes.insert(*node);
            return true;
          }
        } uses(flow);
        uses.TraverseStmt(function->getBody());
        check(uses.readNodes.size() == 1,
              "repeated reads share one reaching value");
      });
  check(bool(graph), "lift pointer storage fixture");
  check(baseAllocations == 0 && solver.nodeCount() == definitions + joins,
        "no per-read, forwarding-join or unused entry storage");
  check(solver.constraintCount() == (joins ? definitions : 0),
        "only nontrivial joins need copy constraints");
}
} // namespace

void sharedFlow() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      "void entry(bool b, int n, int *p){int x=n; int *q=p; "
      "if(b){x=2; q=nullptr;} while(b){x=x+1; q=p;} int y=x; int *r=q;}",
      {"-std=c++20"}, "shared-flow.cpp");
  const clang::FunctionDecl *function = nullptr;
  for (const auto *decl :
       ast->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl))
      function = candidate;
  check(function != nullptr, "shared flow fixture has a body");
  FunctionDataFlow data;
  auto cfg = ClangCFGLifter{}.lift(
      *function, ast->getASTContext(), FunctionId{1}, {},
      [&](clang::CFG *native) {
        Andersen pointerOnly, shared;
        LocalDataFlow baseline(
            *function, native, pointerOnly,
            [&](const clang::ValueDecl *) { return pointerOnly.addNode(); });
        LocalDataFlow flow(
            *function, native, shared,
            [&](const clang::ValueDecl *) { return shared.addNode(); }, &data,
            BodyId{1});
        check(
            shared.nodeCount() == pointerOnly.nodeCount() &&
                shared.constraintCount() == pointerOnly.constraintCount(),
            "scalar def/use extraction creates no extra PTA nodes or constraints");
        check(flow.variables() == baseline.variables(),
              "same pointer variable inventory");
      });
  check(bool(cfg) && data.parameters.size() == 3,
        "shared flow retains all formal inputs");
  check(std::count_if(data.nodes.begin(), data.nodes.end(),
                      [](const auto &node) {
                        return node.kind == DataNodeKind::Join;
                      }) >= 2 &&
            !data.uses.empty(),
        "shared scalar/pointer branch and loop joins retain def/use edges");
}

void missingCFG() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      "void entry(){int *p=nullptr; int *q=p;}", {"-std=c++20"},
      "missing-cfg.cpp");
  const clang::FunctionDecl *function = nullptr;
  for (const auto *decl :
       ast->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl))
      function = candidate;
  check(function != nullptr, "missing-CFG fixture has a function");
  const auto *body = llvm::cast<clang::CompoundStmt>(function->getBody());
  const auto *decl = llvm::cast<clang::VarDecl>(
      llvm::cast<clang::DeclStmt>(*body->body_begin())->getSingleDecl());
  const auto *copy = llvm::cast<clang::VarDecl>(
      llvm::cast<clang::DeclStmt>(body->body_back())->getSingleDecl());
  const auto *ref =
      llvm::cast<clang::DeclRefExpr>(copy->getInit()->IgnoreParenImpCasts());
  Andersen solver;
  const auto storage = solver.addNode(), object = solver.addNode();
  solver.beginCapture();
  LocalDataFlow flow(*function, nullptr, solver,
                     [&](const clang::ValueDecl *) { return storage; });
  const auto definition = flow.initialize(decl).pointer,
             use = flow.read(ref).pointer;
  check(definition && use,
        "eligible fallback variable has a definition and use");
  solver.address(*definition, object);
  check(flow.variables() == 0 && flow.fallbacks() == 2,
        "missing CFG falls back for the whole variable");
  solver.replay(solver.endCapture());
  check(solver.solve() && solver.mayPointTo(*use, object) &&
            solver.isUnknown(*use),
        "fallback retains known targets and uncertain execution order");
}

void localFlowCases() {
  const auto overwrite = fixture("void entry(){P p=a; P q=p; p=b; q(); p();}");
  check(targets(overwrite) == std::vector<std::set<std::string>>{{"a"}, {"b"}},
        "later definitions cannot flow backward into an earlier copy");
  check(overwrite.closedWithinInputs(),
        "supported local overwrite remains closed");
  connections(*overwrite.interCFG);

  const auto branch = fixture(
      "void entry(bool cond){P p=a; P q=p; if(cond) p=b; else p=c; q(); p();}");
  check(
      targets(branch) == std::vector<std::set<std::string>>{{"a"}, {"b", "c"}},
      "join only definitions from predecessor branches, keeping old copies separate");
  const auto loop =
      fixture("void entry(bool cond){P p=a; while(cond){p(); p=b;} p();}");
  check(targets(loop) ==
            std::vector<std::set<std::string>>{{"a", "b"}, {"a", "b"}},
        "a loop backedge carries the previous iteration's definition");
  const auto loopKill =
      fixture("void entry(bool cond){P p=a; while(cond){p=b; p();} p();}");
  check(
      targets(loopKill) ==
          std::vector<std::set<std::string>>{{"b"}, {"a", "b"}},
      "a local overwrite kills the preceding definition within each iteration");
  const auto self = fixture("void entry(){P p=a; p=p; p(); p=b; p();}");
  check(targets(self) == std::vector<std::set<std::string>>{{"a"}, {"b"}},
        "self assignment reads before writing");
  const auto optionalWrite =
      fixture("void entry(bool cond){P p=a; if(cond) p=b; p();}");
  check(targets(optionalWrite) ==
            std::vector<std::set<std::string>>{{"a", "b"}},
        "a branch without a definition preserves its predecessor value");
  const auto doLoop =
      fixture("void entry(bool cond){P p=a; do {p=b;} while(cond); p();}");
  check(targets(doLoop) == std::vector<std::set<std::string>>{{"b"}},
        "a mandatory loop iteration replaces the entry value");
  const auto exceptions = fixture("void throws(){throw 1;} void entry(){P p=a; "
                                  "try {throws(); p=b;} catch(...) {p();}}");
  check(targets(exceptions).back().contains("a"),
        "exceptional predecessors retain the pre-call local value");
  const auto early =
      fixture("void entry(bool cond){P p=a; if(cond){p(); return;} p=b; p();}");
  check(targets(early) == std::vector<std::set<std::string>>{{"a"}, {"b"}},
        "definitions beyond an early return cannot reach the returned branch");
  const auto null = fixture("void entry(){P p=a; P q=p; p=nullptr; q(); p();}");
  check(
      targets(null) == std::vector<std::set<std::string>>{{"a"}, {}},
      "a null definition kills prior targets without erasing an earlier copy");
  const auto multiple = fixture("void entry(){P p=a,q=p; q(); p=b; p();}");
  check(targets(multiple) == std::vector<std::set<std::string>>{{"a"}, {"b"}},
        "multi-declaration initialization retains execution order");
}

void callBindingFlow() {
  const auto recursive =
      fixture("P recur(P p,int n){if(n) return recur(p,n-1); p=b; return p;} "
              "void entry(){P p=recur(a,2); p();}");
  check(targets(recursive) ==
            std::vector<std::set<std::string>>{{"recur"}, {"b"}},
        "recursive call and return bindings converge over local definitions");
  connections(*recursive.interCFG);
  const auto args = fixture(
      "P choose(P p){P q=p; p=b; return q;} void entry(){P p=choose(a); p();}");
  check(targets(args) == std::vector<std::set<std::string>>{{"choose"}, {"a"}},
        "call bindings use entry parameters and positioned return operands");
  const auto overwrittenParameter = fixture(
      "P choose(P p){p=b; return p;} void entry(){P p=choose(a); p();}");
  check(targets(overwrittenParameter) ==
            std::vector<std::set<std::string>>{{"choose"}, {"b"}},
        "callee parameter assignment does not merge the old actual argument");
}

void conservativeLocalFallbacks() {
  const auto alias =
      fixture("void entry(){P p=a; P* alias=&p; P q=p; *alias=b; q(); p();}");
  check(targets(alias) ==
            std::vector<std::set<std::string>>{{"a", "b"}, {"a", "b"}},
        "address-taken variables retain conservative shared storage");
  const auto reference = fixture(
      "void mutate(P& p){p=b;} void entry(){P p=a; mutate(p); P q=p; q();}");
  check(targets(reference) ==
            std::vector<std::set<std::string>>{{"mutate"}, {"a", "b"}},
        "reference bindings exclude local strong updates");
  const auto nested = fixture("void entry(){P p=a; P q=p; (p=b,p()); q();}");
  check(targets(nested) ==
            std::vector<std::set<std::string>>{{"a", "b"}, {"a", "b"}},
        "unsupported nested writes fall back for the entire variable");

  const auto captured =
      fixture("void entry(){P p=a; auto unused=[&p]{}; P q=p; p=b; q();}");
  check(targets(captured) == std::vector<std::set<std::string>>{{"a", "b"}},
        "skipping a lambda body still excludes its captured pointer storage");
}

void unknownLocalFlow() {
  const auto maybeInit =
      fixture("void entry(bool cond){P p; if(cond) p=a; p();}");
  check(targets(maybeInit) == std::vector<std::set<std::string>>{{"a"}} &&
            !maybeInit.closedWithinInputs(),
        "an uninitialized predecessor remains unknown at the join");
  const auto unknown = fixture(
      "P external(); void entry(){P p=a; P q=p; p=external(); q(); p();}");
  check(
      targets(unknown)[1] == std::set<std::string>{"a"} &&
          !unknown.closedWithinInputs(),
      "unknown later return preserves the earlier local value and open effects");
  check(!unknown.interCFG->summaries().empty(),
        "unknown calls retain summary continuations");
  const auto uninit = fixture("void entry(){P p; p(); p=a; p();}");
  check(
      targets(uninit) == std::vector<std::set<std::string>>{{}, {"a"}} &&
          !uninit.closedWithinInputs(),
      "uninitialized entry value remains unknown without importing a later definition");
}

void incrementalFlow() {
  const char *caller = "using P=void(*)(); P choose(); void a(){} void b(){} "
                       "void entry(){P p=choose(); a(); P q=p; p=b; q();}";
  const char *callee = "using P=void(*)(); void a(); P choose(){return a;}";
  CallGraphSession growing;
  add(growing, caller, "caller.cpp");
  const auto before = solve(growing, false);
  check(before.interCFG->connections().size() == 1 &&
            !before.interCFG->summaries().empty(),
        "initial known edge coexists with provisional missing body effects");
  const auto original = *before.interCFG->connection(CallConnectionId{1});
  add(growing, callee, "callee.cpp");
  const auto after = solve(growing, false);
  check(after.closedWithinInputs() && after.interCFG->summaries().empty(),
        "late body and discovered target replace provisional summaries");
  check(targets(after) ==
            std::vector<std::set<std::string>>{{"choose"}, {"a"}, {"a"}},
        "late returned target refines only affected local uses");
  check(before.interCFG->connections().size() == 1 &&
            !before.interCFG->summaries().empty(),
        "earlier graph snapshots stay immutable");
  const auto *stable = after.interCFG->connection(CallConnectionId{1});
  check(stable->source == original.source && stable->site == original.site &&
            stable->callee == original.callee,
        "late edge insertion never renumbers a previous connection");
  connections(*after.interCFG);
  CallGraphSession batch;
  add(batch, caller, "caller.cpp");
  add(batch, callee, "callee.cpp");
  const auto together = solve(batch, false);
  check(
      targets(together) == targets(after) &&
          together.statistics.pointerConstraints ==
              after.statistics.pointerConstraints,
      "incremental imports and batch extraction produce the same sparse solution");
}

int main() {
  sharedFlow();
  checkLocalStorage(
      "void use(int*); void entry(){int *p=nullptr; use(p); use(p);}", 1, 0);
  checkLocalStorage("void use(int*); void entry(){int *p=nullptr;"
                    "auto unused=[]{int *q=nullptr; return q;}; use(p);}",
                    1, 0);
  checkLocalStorage(
      "void use(int*); void other(); void entry(bool b){int *p=nullptr;"
      "if(b) other(); else other(); use(p); use(p);}",
      1, 0);
  checkLocalStorage(
      "void use(int*); void other(); void entry(bool b){int *p=nullptr;"
      "while(b){other();} use(p); use(p);}",
      1, 0);
  checkLocalStorage("void use(int*); void entry(bool b){int *p;"
                    "if(b) p=nullptr; else p=nullptr; use(p); use(p);}",
                    2, 1);
  missingCFG();
  localFlowCases();
  callBindingFlow();
  conservativeLocalFallbacks();
  unknownLocalFlow();
  incrementalFlow();
}
