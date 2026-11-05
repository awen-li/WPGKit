#include "CallGraphPTA.h"
#include "DataFlowGraph.h"
#include "InterCFG.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

auto parse(const char *code, const char *file = "empty-models.cpp") {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"}, file);
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  return ast;
}

CallGraphResult take(llvm::Expected<CallGraphResult> result) {
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  return std::move(*result);
}

CallGraphResult analyze(const char *code) {
  CallGraphSession session(nullptr, {}, ExternalModules::empty());
  auto ast = parse(code);
  session.addUnit(ast->getASTContext());
  ast.reset();
  return take(std::move(session).finalize({"entry"}));
}

bool hasTarget(const CallGraphResult &result, const CallSiteResult &site,
               const char *name) {
  return std::any_of(
      site.targets.begin(), site.targets.end(), [&](auto target) {
        return result.metadata.text(result.names.at(target)) == name;
      });
}

const CallSiteResult &callTo(const CallGraphResult &result, const char *name) {
  auto found = std::find_if(
      result.calls.begin(), result.calls.end(),
      [&](const auto &call) { return hasTarget(result, call, name); });
  check(found != result.calls.end(), "retain known callee identity");
  return *found;
}

void checkEmptyCatalog(const CallGraphResult &result) {
  check(result.finalized && result.statistics.modeledCalls == 0 &&
            result.externalModels.empty(),
        "empty catalog never applies or reports a reviewed model");
  check(result.interCFG && result.dataFlow && !result.cfgs.empty() &&
            result.interCFG->inputFinalized(),
        "CFG/inter-CFG/DFG survive without external models or native storage");
}

void checkUnknownCall(const CallGraphResult &result,
                      const CallSiteResult &call) {
  check(call.unresolved && call.externalEffects == ExternalSummary::unknown(),
        "unmodeled call retains every conservative may-effect");
  check(std::any_of(result.interCFG->summaries().begin(),
                    result.interCFG->summaries().end(),
                    [&](const auto &summary) {
                      return summary.site == call.id &&
                             summary.effects == ExternalSummary::unknown();
                    }),
        "inter-CFG carries the conservative summary");
  check(std::any_of(result.dataFlow->gaps().begin(),
                    result.dataFlow->gaps().end(),
                    [&](const auto &gap) {
                      return gap.site == call.id &&
                             gap.kind == DataGapKind::UnknownCall;
                    }),
        "DFG preserves the unknown-call gap");
}

void queryResults() {
  auto ast = parse(R"cpp(
    extern "C" void *memcpy(void *, const void *, __SIZE_TYPE__);
    void unavailable();
  )cpp");
  auto &context = ast->getASTContext();
  const auto find = [&](const char *name) -> const clang::FunctionDecl * {
    for (const auto *decl :
         context.getTranslationUnitDecl()->lookup(&context.Idents.get(name)))
      if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl))
        return function;
    return nullptr;
  };
  const auto *copy = find("memcpy"), *unavailable = find("unavailable");
  check(copy && unavailable, "find query fixtures");
  const ExternalModules catalog;
  const auto modeled = catalog.query(copy);
  check(modeled.matched() && modeled.model == ExternalModel::CopyMemory &&
            modeled.summary.complete() &&
            modeled.summary.has(CallEffect::WriteArguments) &&
            !modeled.summary.has(CallEffect::WriteGlobals),
        "a matching rule returns its reviewed summary through the query API");
  for (const auto result :
       {ExternalModelResult{}, ExternalModules::query(), catalog.query(nullptr),
        catalog.query(unavailable), ExternalModules::empty().query(copy)})
    check(
        !result.matched() && result.summary == ExternalSummary::unknown(),
        "default, unresolved, absent and empty-catalog queries are conservative");
  ast.reset();
  check(ExternalModules::query(modeled.model).summary == modeled.summary,
        "saved numeric identities query the same contract after AST release");
}

void callSiteQueries() {
  const auto bits = [](FormatEffect effect) {
    return static_cast<std::uint8_t>(static_cast<unsigned>(effect) << 2);
  };
  const auto output = ExternalModules::query(ExternalModel::Fprintf,
                                             bits(FormatEffect::OutputOnly));
  const auto count = ExternalModules::query(ExternalModel::Fprintf,
                                            bits(FormatEffect::WriteCount));
  const auto unsupported = ExternalModules::query(
      ExternalModel::Fprintf, bits(FormatEffect::Unsupported));
  check(
      output.matched() && output.summary.complete() &&
          output.summary.has(CallEffect::WriteStream) &&
          !output.summary.has(CallEffect::WriteArguments) &&
          count.summary.has(CallEffect::WriteArguments),
      "query uses call-site format facts instead of a declaration-only summary");
  check(unsupported.matched() && !unsupported.summary.complete(),
        "recognizing a rule does not conceal unsupported call effects");
  check(
      !ExternalModules::query(ExternalModel::Sort).summary.complete() &&
          ExternalModules::query(ExternalModel::Sort, 0, true)
              .summary.complete(),
      "callback effects stay open until their read-only contract is established");
}

void internalCalls() {
  auto ast = parse(R"cpp(
    using Fn = void (*)();
    void target() {}
    Fn identity(Fn callback) { return callback; }
    void entry() { Fn callback = identity(&target); callback(); }
  )cpp");
  auto result = take(CallGraphPTA::run({&ast->getASTContext()}, {"entry"},
                                       nullptr, {}, ExternalModules::empty()));
  ast.reset();
  checkEmptyCatalog(result);
  check(result.closedWithinInputs() && result.statistics.unknownCalls == 0 &&
            result.calls.size() == 2 && result.reachable.size() == 3,
        "internal direct and indirect calls resolve without models");
  check(!callTo(result, "target").unresolved &&
            result.interCFG->connections().size() == 2,
        "ordinary PTA and inter-CFG bindings remain active");
  check(std::any_of(result.dataFlow->edges().begin(),
                    result.dataFlow->edges().end(),
                    [](const auto &edge) {
                      return edge.kind == DataEdgeKind::ActualFormal;
                    }),
        "internal DFG dependencies remain active");
}

void unknownMemoryAndReturn() {
  const auto result = analyze(R"cpp(
    using Fn = void (*)();
    extern "C" void *memcpy(void *, const void *, __SIZE_TYPE__);
    void target() {}
    Fn global = &target;
    void entry() {
      Fn local = &target;
      auto copy = &memcpy;
      auto returned = static_cast<Fn *>(copy(&local, &global, sizeof(local)));
      (*returned)(); local(); global();
    }
  )cpp");
  checkEmptyCatalog(result);
  check(!result.closedWithinInputs() && result.unknownMemoryEffects &&
            !result.unresolvedBodies.empty() && result.calls.size() == 4,
        "unavailable effects keep coverage open");
  checkUnknownCall(result, callTo(result, "memcpy"));
  for (const auto &call : result.calls) {
    if (hasTarget(result, call, "memcpy"))
      continue;
    check(
        hasTarget(result, call, "target"),
        "fallback preserves known aliases through return, argument and global");
    checkUnknownCall(result, call);
  }
  check(std::any_of(result.dataFlow->edges().begin(),
                    result.dataFlow->edges().end(),
                    [&](const auto &edge) {
                      return edge.kind == DataEdgeKind::Summary &&
                             result.dataFlow->node(edge.target)->kind ==
                                 DataNodeKind::CallResult;
                    }),
        "generic DFG summary retains input-to-return dependencies");
}

void unknownReceiver() {
  const auto result = analyze(R"cpp(
    using Fn = void (*)();
    void target() {}
    struct Box { Fn callback; void touch(); ~Box(); };
    void entry() {
      Box box; box.callback = &target;
      box.touch(); box.callback();
    }
  )cpp");
  checkEmptyCatalog(result);
  checkUnknownCall(result, callTo(result, "Box::touch"));
  checkUnknownCall(result, callTo(result, "Box::~Box"));
  checkUnknownCall(result, callTo(result, "target"));
}

void isolatedCatalogs() {
  auto ast = parse("long entry() { return __builtin_expect(1, 0); }");
  CallGraphSession builtin;
  CallGraphSession empty(nullptr, {}, ExternalModules::empty());
  builtin.addUnit(ast->getASTContext());
  empty.addUnit(ast->getASTContext());
  ast.reset();
  const auto conservative = take(empty.finalize({"entry"}));
  checkEmptyCatalog(conservative);
  check(conservative.calls.size() == 1, "retain an unmodeled builtin call");
  checkUnknownCall(conservative, conservative.calls.front());
  const auto refined = take(builtin.finalize({"entry"}));
  check(refined.closedWithinInputs() && refined.statistics.modeledCalls == 1 &&
            refined.statistics.unknownCalls == 0,
        "empty selection does not alter another session's builtin catalog");
  const auto repeated = take(empty.finalize({"entry"}));
  check(repeated.statistics == conservative.statistics &&
            repeated.dataFlow == conservative.dataFlow,
        "empty-catalog finalization is idempotent");
}

void incrementalBodies() {
  CallGraphSession session(nullptr, {}, ExternalModules::empty());
  {
    auto ast = parse("void relay(void (*)()); void target() {} "
                     "void entry() { relay(&target); }",
                     "entry.cpp");
    session.addUnit(ast->getASTContext());
  }
  const auto before = take(session.analyze({"entry"}));
  check(!before.closedWithinInputs() && before.statistics.unknownCalls == 0,
        "missing bodies stay provisional before finalization");
  {
    auto ast =
        parse("void relay(void (*callback)()) { callback(); }", "relay.cpp");
    session.addUnit(ast->getASTContext());
  }
  const auto after = take(session.finalize({"entry"}));
  checkEmptyCatalog(after);
  check(after.closedWithinInputs() && after.statistics.unknownCalls == 0 &&
            !callTo(after, "target").unresolved,
        "late supplied bodies resolve without external summaries");
  check(take(session.analyze({"entry"})).statistics == after.statistics,
        "unchanged analysis does not replay the empty-catalog fixed point");
}

void inputRunners(const std::string &directory) {
  for (int mode = 0; mode != 3; ++mode) {
    const auto run = [&](ExternalModules models) {
      if (mode == 0)
        return CallGraphPTA::runFiles({directory + "/entry.cpp"}, directory,
                                      {"main"}, nullptr, {}, models);
      if (mode == 1)
        return CallGraphPTA::runLinkMap(directory + "/linked.map", directory,
                                        {"main"}, nullptr, {}, models);
      return CallGraphPTA::runLinkMaps({directory + "/linked.map"}, directory,
                                       {"main"}, nullptr, {}, models);
    };
    const auto refined = take(run({}));
    check(refined.calls.size() == 1 && !refined.calls.front().unresolved &&
              refined.statistics.modeledCalls == 1 &&
              refined.statistics.unknownCalls == 0,
          "input runner uses the available scalar contract");
    const auto conservative = take(run(ExternalModules::empty()));
    checkEmptyCatalog(conservative);
    check(conservative.calls.size() == 1 &&
              conservative.statistics.unknownCalls == 1,
          "input runner applies fallback with no available model");
    checkUnknownCall(conservative, conservative.calls.front());
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2) {
    inputRunners(argv[1]);
    return 0;
  }
  check(argc == 1, "expected no arguments or the fixture directory");
  queryResults();
  callSiteQueries();
  internalCalls();
  unknownMemoryAndReturn();
  unknownReceiver();
  isolatedCatalogs();
  incrementalBodies();
}
