#include "Andersen.h"
#include "CallGraphPTA.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

CallGraphResult analyze(const char *code) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {"-std=c++20"}, "pointer-completeness.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  CallGraphSession session;
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
        "unchanged finalization retains its fixed point and snapshots");
  return std::move(*result);
}

const CallSiteResult &callTo(const CallGraphResult &result, const char *name) {
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.metadata.text(result.names.at(target)) == name)
        return call;
  check(false, "expected target remains reachable");
  std::abort();
}

void immutableValues() {
  for (const auto *code : {R"cpp(
    using Fn = void (*)();
    void unknown(); void target() {}
    static void invoke(Fn fn) { unknown(); fn(); }
    void entry() { invoke(&target); }
  )cpp",
                           R"cpp(
    using Fn = void (*)();
    void unknown(); void target() {}
    Fn factory() { return &target; }
    void entry() { unknown(); auto fn = factory(); fn(); }
  )cpp",
                           R"cpp(
    void unknown(); void target() {} void alternative() {}
    void entry(bool choose) {
      auto fn = choose ? &target : &alternative;
      unknown(); fn();
    }
  )cpp"}) {
    const auto result = analyze(code);
    check(
        !callTo(result, "target").unresolved &&
            callTo(result, "unknown").unresolved &&
            !result.closedWithinInputs(),
        "unknown effects leave immutable target values closed and their own site open");
  }
}

void memoryValues() {
  for (const auto *code : {R"cpp(
    void unknown(); void target() {}
    auto shared = &target;
    void entry() { unknown(); shared(); }
  )cpp",
                           R"cpp(
    using Fn = void (*)();
    void replace(Fn *); void target() {}
    void entry() { Fn fn = &target; replace(&fn); fn(); }
  )cpp",
                           R"cpp(
    void unknown(); void target() {}
    struct Holder { void (*fn)(); };
    void entry() { Holder h{&target}; unknown(); h.fn(); }
  )cpp",
                           R"cpp(
    using Fn = void (*)();
    void unknown(); void target() {}
    void invoke(Fn *slot) { unknown(); (*slot)(); }
    void entry() { Fn fn = &target; invoke(&fn); }
  )cpp"}) {
    const auto result = analyze(code);
    check(
        callTo(result, "target").unresolved,
        "globals, escaped locals, fields and dereferences retain memory uncertainty");
  }
}

void incompleteTargets() {
  const auto missing = analyze(R"cpp(
    void missing();
    void entry() { auto fn = &missing; fn(); }
  )cpp");
  check(callTo(missing, "missing").unresolved,
        "a complete pointer target set does not supply a missing callee body");
  const auto external = analyze("void entry(void (*fn)()) { fn(); }");
  check(external.calls.size() == 1 && external.calls.front().unresolved &&
            external.calls.front().targets.empty(),
        "unknown entry pointers are not treated as complete empty target sets");
}

void escapedCallbacks() {
  for (const auto *code : {R"cpp(
    using Fn = void (*)();
    void external(void (*)(Fn)); void target() {}
    static void invoke(Fn fn) { fn(); }
    void entry() { invoke(&target); external(&invoke); }
  )cpp",
                           R"cpp(
    using Fn = void (*)();
    void external(void (*)(Fn)); void target() {}
    static void invoke(Fn fn);
    void entry() { invoke(&target); external(&invoke); }
    static void invoke(Fn fn) { fn(); }
  )cpp",
                           R"cpp(
    using Fn = void (*)();
    void external(); void target() {}
    void invoke(Fn fn) { fn(); }
    void entry() { invoke(&target); external(); }
  )cpp"}) {
    const auto result = analyze(code);
    check(
        callTo(result, "target").unresolved,
        "address-taken and exported functions retain possible external arguments");
  }
}

void callbackParameters() {
  const auto result = analyze(R"cpp(
    #include <cstdlib>
    using Compare = int (*)(const void *, const void *);
    void unknown();
    int compare(const void *, const void *) { return 0; }
    static void sort(int *values, Compare fn) { qsort(values, 2, sizeof(int), fn); }
    void entry() { int values[2]; unknown(); sort(values, &compare); }
  )cpp");
  const auto &sort = callTo(result, "qsort");
  check(!sort.unresolved && sort.externalEffects.complete() &&
            sort.externalEffects.has(CallEffect::Callback),
        "a read-only comparator remains provable through by-value parameters");
  for (const auto function : result.reachable)
    check(result.metadata.text(result.names.at(function)) != "compare",
          "callback summaries do not activate additional target bodies");
}

void provenanceGrowth() {
  Andersen solver;
  const auto function = solver.addNode();
  const auto value = solver.addNode();
  const auto copied = solver.addNode();
  const auto storage = solver.addNode();
  const auto pointer = solver.addNode();
  solver.address(value, function);
  solver.copy(copied, value);
  solver.address(storage, function);
  solver.address(pointer, storage);
  check(solver.solve() && !solver.isExternallyAffected(value) &&
            !solver.isExternallyAffected(copied),
        "addressed storage does not contaminate a literal function address");
  solver.takeChangedNodes();
  const auto targets = solver.targetCount(copied);
  solver.load(value, pointer);
  check(solver.solve() && solver.isExternallyAffected(copied) &&
            solver.targetCount(copied) == targets,
        "a memory dependency propagates even without new concrete targets");
  const auto changed = solver.takeChangedNodes();
  check(std::find(changed.begin(), changed.end(), copied) != changed.end(),
        "provenance-only growth wakes dependent call sites");
  const auto work = solver.processedConstraints();
  solver.markExternallyAffected(value);
  check(solver.solve() && solver.processedConstraints() == work,
        "repeated provenance adds no solver work");
}

void constantDefinitions() {
  for (
      const auto &[first, second, conflict] :
      {std::tuple{
           "constexpr unsigned key = 7; inline unsigned helper() { return key; }",
           "constexpr unsigned key = 7; inline unsigned helper() { return key; }",
           false},
       std::tuple{
           "constexpr unsigned key = 7; inline unsigned helper() { return key; }",
           "constexpr unsigned key = 9; inline unsigned helper() { return key; }",
           true},
       std::tuple{
           "constexpr unsigned key = 7; inline const unsigned *helper() { return &key; }",
           "constexpr unsigned key = 7; inline const unsigned *helper() { return &key; }",
           true}}) {
    CallGraphSession session;
    for (const auto *prefix : {first, second}) {
      const std::string code =
          std::string(prefix) + " void entry() { helper(); }";
      auto ast = clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"},
                                                          "shared-constants.h");
      check(ast && !ast->getDiagnostics().hasErrorOccurred(),
            "parse constants");
      session.addUnit(ast->getASTContext());
    }
    auto result = session.finalize({"entry"});
    check(static_cast<bool>(result), "analyze shared constant definitions");
    check(
        callTo(*result, "helper").unresolved == conflict &&
            result->hasLimitation("conflicting function definitions") ==
                conflict,
        "equal constant values agree; different values and addressed objects conflict");
    check(result->cfgs.size() == 2, "shared definitions still retain one CFG");
  }
}

void devirtualization() {
  for (const auto *code : {R"cpp(
    struct Base { virtual int run() { return 1; } };
    struct Derived final : Base { int run() override { return 2; } };
    void entry(Derived *value) { value->run(); }
  )cpp",
                           R"cpp(
    struct Base { virtual int run() { return 1; } };
    struct Derived : Base { int run() final { return 2; } };
    void entry(Derived &value) { value.run(); }
  )cpp",
                           R"cpp(
    struct Base { virtual int run() { return 1; } };
    struct Derived : Base { int run() override { return 2; } };
    void entry() { Derived value; value.run(); }
  )cpp",
                           R"cpp(
    struct Base { virtual int run() { return 1; } };
    struct Derived final : Base { int run() override { return 2; } };
    void entry(Derived *value) { static_cast<Base *>(value)->run(); }
  )cpp",
                           R"cpp(
    struct Base { virtual int operator()() { return 1; } };
    struct Derived final : Base { int operator()() override { return 2; } };
    void entry(Derived &value) { value(); }
  )cpp"}) {
    const auto result = analyze(code);
    const auto *name = std::string(code).find("operator()") == std::string::npos
                           ? "Derived::run"
                           : "Derived::operator()";
    const auto &call = callTo(result, name);
    check(!call.unresolved && call.targets.size() == 1,
          "Clang-proven virtual targets use a single ordinary call binding");
    check(!result.hasLimitation(
              "virtual dispatch may have targets outside supplied units"),
          "a proven target does not introduce an open virtual boundary");
  }
  const auto dynamic = analyze(R"cpp(
    struct Base { virtual int run() { return 1; } };
    struct Derived final : Base { int run() override { return 2; } };
    void entry(Base *value) { value->run(); }
  )cpp");
  check(
      callTo(dynamic, "Base::run").unresolved &&
          callTo(dynamic, "Base::run").targets.size() == 2,
      "an unknown base object retains all known overrides and its open boundary");
  const auto qualified = analyze(R"cpp(
    struct Base { virtual int run() { return 1; } };
    struct Derived final : Base { int run() override { return 2; } };
    void entry(Derived &value) { value.Base::run(); }
  )cpp");
  check(!callTo(qualified, "Base::run").unresolved &&
            callTo(qualified, "Base::run").targets.size() == 1,
        "explicit qualification retains the named base implementation");
}

void distinctLambdaSpecializations() {
  const char *code = R"cpp(
    inline void left() {} inline void right() {}
    template<class F> struct Callback {
      F fn;
      Callback(F value) : fn(value) {}
      void run() { fn(); }
    };
    template<class T> struct Factory {
      static void run(bool choose) {
        if (choose) {
          auto first = [] { left(); };
          Callback<decltype(first)> callback(first);
          callback.run();
        } else {
          auto second = [] { right(); };
          Callback<decltype(second)> callback(second);
          callback.run();
        }
      }
    };
    void entry(bool choose) { Factory<int>::run(choose); }
  )cpp";
  CallGraphSession session;
  std::size_t cfgs = 0;
  for (unsigned unit = 0; unit != 2; ++unit) {
    auto ast = clang::tooling::buildASTFromCodeWithArgs(
        code, {"-xc++", "-std=c++20"}, "lambda-specializations.h");
    check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse lambdas");
    session.addUnit(ast->getASTContext());
    ast.reset();
    auto result =
        unit ? session.finalize({"entry"}) : session.analyze({"entry"});
    check(static_cast<bool>(result), "analyze distinct lambda specializations");
    check(!result->hasLimitation("conflicting function definitions"),
          "distinct lambda specializations never merge their bodies");
    check(!callTo(*result, "left").unresolved &&
              !callTo(*result, "right").unresolved,
          "both lambda bodies and their calls remain reachable");
    unsigned wrappers = 0;
    for (const auto function : result->reachable) {
      const auto name = result->metadata.text(result->names.at(function));
      wrappers += name.starts_with("Callback<") && name.ends_with("::run");
    }
    check(wrappers == 2, "each closure type retains its own wrapper body");
    if (unit)
      check(result->cfgs.size() == cfgs,
            "the same specializations are shared across translation units");
    cfgs = result->cfgs.size();
  }
}
} // namespace

int main() {
  immutableValues();
  memoryValues();
  incompleteTargets();
  escapedCallbacks();
  callbackParameters();
  provenanceGrowth();
  constantDefinitions();
  devirtualization();
  distinctLambdaSpecializations();
}
