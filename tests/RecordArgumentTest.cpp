#include "CallGraphPTA.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

CallGraphResult analyze(const char *code, bool fields) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"},
                                                      "record-argument.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  AnalysisOptions options;
  options.fieldSensitive = fields;
  CallGraphSession session(nullptr, {}, ExternalModules{}, options);
  session.addUnit(ast->getASTContext());
  ast.reset();
  auto result = session.finalize({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  auto repeated = session.finalize({"entry"});
  check(repeated && repeated->statistics == result->statistics &&
            repeated->interCFG == result->interCFG &&
            repeated->dataFlow == result->dataFlow,
        "repeated finalization reuses the fixed point after AST release");
  return std::move(*result);
}

std::set<std::string> targets(const CallGraphResult &result,
                              const char *caller) {
  std::set<std::string> names;
  for (const auto &call : result.calls)
    if (result.metadata.text(result.names.at(call.caller)) == caller)
      for (auto target : call.targets)
        names.emplace(result.metadata.text(result.names.at(target)));
  return names;
}

void copiesAndTemporaries(bool fields) {
  for (auto *code : {R"cpp(
    void target() {}
    struct Callback { void (*fn)(); };
    void invoke(Callback action) { action.fn(); }
    void entry() { Callback action{target}; invoke(action); }
  )cpp",
                     R"cpp(
    void target() {}
    struct Callback { void (*fn)(); Callback(void (*f)()) : fn(f) {} };
    void invoke(Callback action) { action.fn(); }
    void entry() { invoke(Callback(target)); }
  )cpp",
                     R"cpp(
    void target() {}
    struct Callback { void (*fn)(); };
    void invoke(Callback action) { action.fn(); }
    void entry() { auto call = &invoke; call(Callback{target}); }
  )cpp",
                     R"cpp(
    void target() {}
    struct Callback { void (*fn)(); };
    struct Pass { Callback action; };
    void invoke(Pass pass) { pass.action.fn(); }
    void entry() { Pass pass{{target}}; invoke(pass); }
  )cpp"}) {
    auto result = analyze(code, fields);
    check(targets(result, "invoke").contains("target"),
          "by-value pointer fields recover the callback target");
  }
}

void independentStorage(bool fields) {
  auto result = analyze(R"cpp(
    void original() {} void replacement() {}
    struct Callback { void (*fn)(); };
    void replace(Callback action) { action.fn = replacement; action.fn(); }
    void entry() { Callback action{original}; replace(action); action.fn(); }
  )cpp",
                        fields);
  check(targets(result, "replace").contains("replacement"),
        "writes reach the independent parameter object");
  check(targets(result, "entry").contains("original") &&
            !targets(result, "entry").contains("replacement"),
        "writing a by-value parameter does not mutate the caller's record");
}
} // namespace

int main() {
  for (bool fields : {false, true}) {
    copiesAndTemporaries(fields);
    independentStorage(fields);
  }
}
