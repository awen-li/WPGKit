#include "CallGraphPTA.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <iostream>

using wpgkit::CallGraphPTA;
using wpgkit::CallGraphResult;
using wpgkit::CallGraphSession;

namespace {
bool edge(const CallGraphResult &result, const std::string &caller,
          const std::string &callee) {
  for (const auto &site : result.calls)
    if (result.metadata.text(result.names.at(site.caller)) == caller)
      for (const auto &target : site.targets)
        if (result.metadata.text(result.names.at(target)) == callee)
          return true;
  return false;
}

bool unresolvedEdge(const CallGraphResult &result, const std::string &callee) {
  for (const auto &site : result.calls)
    if (site.unresolved)
      for (const auto &target : site.targets)
        if (result.metadata.text(result.names.at(target)) == callee)
          return true;
  return false;
}

bool unresolved(const CallGraphResult &result) {
  for (const auto &site : result.calls)
    if (site.unresolved)
      return true;
  return false;
}

bool validGraphIds(const CallGraphResult &result) {
  for (const auto id : result.reachable)
    if (!id.isValid() || !result.names.contains(id) ||
        !result.usrs.contains(id))
      return false;
  std::set<wpgkit::CallSiteId> seenSites;
  for (const auto &site : result.calls) {
    if (!site.id.isValid() || !seenSites.insert(site.id).second ||
        !result.locations.contains(site.id) ||
        !result.names.contains(site.caller))
      return false;
    for (const auto target : site.targets)
      if (!result.names.contains(target) || !result.usrs.contains(target))
        return false;
  }
  return true;
}

bool validCFGCalls(const CallGraphResult &result) {
  for (const auto &site : result.calls) {
    const auto *body = result.cfg(site.body);
    if (!body || !body->graph || body->function != site.caller ||
        !body->unit.isValid() || !body->definition.file.isValid())
      return false;
    bool mapped = false;
    for (const auto &binding : body->calls) {
      if (binding.call != site.id)
        continue;
      bool found = false;
      for (const auto &block : body->graph->blocks())
        for (const auto &event : block.events)
          found |= event.point == binding.point;
      if (!found || binding.point.function() != site.caller)
        return false;
      mapped = true;
    }
    if (!mapped)
      return false;
  }
  return true;
}

bool check(bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

llvm::Expected<CallGraphResult>
run(const std::vector<clang::ASTContext *> &units,
    const std::vector<std::string> &roots) {
  return CallGraphPTA::run(units, roots, nullptr);
}

int parameterAndReferenceStorage() {
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "int *unused(); void entry(int n, bool flag, double value) {}",
        {"-std=c++20"}, "unused_pointer_storage.cpp");
    if (!check(unit != nullptr,
               "parse pointer-free root and unused declaration"))
      return 1;
    auto result = run({&unit->getASTContext()}, {"entry"});
    if (!result) {
      std::cerr << llvm::toString(result.takeError()) << '\n';
      return 1;
    }
    if (!check(
            result->closedWithinInputs() && result->calls.empty() &&
                result->pointerStorage.nodes == 0,
            "unused declarations and scalar parameters require no PTA storage"))
      return 1;
  }
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "using Fn=void(*)(); void target(){} void inspect(int *p){} "
        "Fn choose(int n, Fn f, bool flag){inspect(&n); return f;} "
        "void entry(){choose(1,target,true)();}",
        {"-std=c++20"}, "lazy_parameter_storage.cpp");
    if (!check(unit != nullptr,
               "parse mixed parameter and scalar address fixture"))
      return 1;
    auto result = run({&unit->getASTContext()}, {"entry"});
    if (!result) {
      std::cerr << llvm::toString(result.takeError()) << '\n';
      return 1;
    }
    if (!check(
            result->closedWithinInputs() &&
                result->pointerStorage.objects == 2 &&
                edge(*result, "choose", "inspect") &&
                edge(*result, "entry", "target"),
            "lazy scalar storage preserves addresses, argument positions and pointer returns"))
      return 1;
  }
  for (const auto *alias :
       {"Fn &r=p; set(&r);", "struct Slot{Fn &r;}; Slot s{p}; set(&s.r);"}) {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        std::string(
            "using Fn=void(*)(); void a(){} void b(){} void set(Fn *p){*p=b;} ") +
            "void entry(){Fn p=a; " + alias + " p();}",
        {"-std=c++20"}, "reference_address.cpp");
    if (!check(unit != nullptr, "parse address of callback reference"))
      return 1;
    auto result = run({&unit->getASTContext()}, {"entry"});
    if (!result) {
      std::cerr << llvm::toString(result.takeError()) << '\n';
      return 1;
    }
    if (!check(
            result->closedWithinInputs() && edge(*result, "entry", "a") &&
                edge(*result, "entry", "b"),
            "address of a reference aliases the referent for callback stores"))
      return 1;
  }
  return 0;
}

int crossUnitCalls() {
  auto caller = clang::tooling::buildASTFromCodeWithArgs(
      "void relay(void (*)()); void a() {} void b() {} "
      "void dead() { a(); } "
      "void entry(bool choose) { void (*fp)() = &a; if (choose) fp = &b; relay(fp); }",
      {"-std=c++20"}, "caller.cpp");
  auto callee = clang::tooling::buildASTFromCodeWithArgs(
      "void invoke(void (*cb)()) { cb(); } "
      "void relay(void (*cb)()) { invoke(cb); }",
      {"-std=c++20"}, "callee.cpp");
  if (!check(caller && callee, "parse two translation units"))
    return 1;
  auto result =
      run({&caller->getASTContext(), &callee->getASTContext()}, {"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*result, "entry", "relay") &&
                 edge(*result, "relay", "invoke") &&
                 edge(*result, "invoke", "a") && edge(*result, "invoke", "b"),
             "cross-unit callback targets grow through two call levels") ||
      !check(!edge(*result, "dead", "a"), "unreachable body is not scanned") ||
      !check(!unresolved(*result) && result->limitations.empty(),
             "supported callback fixture resolves without coverage warnings"))
    return 1;
  if (!check(result->closedWithinInputs(),
             "supported callback fixture closes within supplied contexts"))
    return 1;
  if (!check(validGraphIds(*result),
             "graph topology has typed IDs with separate complete metadata"))
    return 1;
  if (!check(validCFGCalls(*result),
             "every callback call fact maps to its compiled body's CFG event"))
    return 1;
  return 0;
}

int ownedCFGLifetime() {
  CallGraphResult retained;
  {
    CallGraphSession session;
    {
      auto unit = clang::tooling::buildASTFromCodeWithArgs(
          "void target() {} void relay(void (*)()); "
          "struct Guard { ~Guard() { target(); } }; "
          "void entry(bool early) { Guard g{}; if (early) return; relay(&target); }",
          {"-std=c++20"}, "cfg_caller.cpp");
      if (!check(unit != nullptr, "parse CFG lifetime caller"))
        return 1;
      session.addUnit(unit->getASTContext());
    }
    {
      auto unit = clang::tooling::buildASTFromCodeWithArgs(
          "void relay(void (*callback)()) { callback(); }", {"-std=c++20"},
          "cfg_callee.cpp");
      if (!check(unit != nullptr, "parse CFG lifetime callee"))
        return 1;
      session.addUnit(unit->getASTContext());
    }
    if (!check(session.cfgCount() == 4 && session.failedCFGCount() == 0,
               "each concrete body has a CFG before CG/PTA starts"))
      return 1;
    auto solved = session.analyze({"entry"});
    if (!solved) {
      std::cerr << llvm::toString(solved.takeError()) << '\n';
      return 1;
    }
    retained = std::move(*solved);
  } // Every AST, native CFG, and the analysis session have been destroyed.
  if (!check(
          retained.closedWithinInputs() && validCFGCalls(retained) &&
              edge(retained, "relay", "target"),
          "owned CFGs and callback results survive all compiler/session lifetimes"))
    return 1;
  bool multipleDestructorExits = false;
  for (const auto &body : retained.cfgs) {
    if (!body->graph)
      return 1;
    std::map<wpgkit::CallSiteId, unsigned> occurrences;
    for (const auto &binding : body->calls)
      ++occurrences[binding.call];
    for (const auto &[site, count] : occurrences)
      multipleDestructorExits |= count > 1;
  }
  if (!check(
          multipleDestructorExits,
          "one local destructor fact maps to both early and normal CFG exits"))
    return 1;
  if (!check(retained.cfg({}) == nullptr &&
                 retained.cfg(wpgkit::BodyId{retained.cfgs.size() + 1}) ==
                     nullptr,
             "invalid body identities do not resolve to a CFG"))
    return 1;
  return 0;
}

int implicitCleanupGap() {
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "struct Base { ~Base() {} }; struct Derived : Base { ~Derived() {} }; "
        "void entry() { Derived d; }",
        {"-std=c++20"}, "cfg_gap.cpp");
    if (!check(unit != nullptr, "parse implicit base destructor gap"))
      return 1;
    auto gap = run({&unit->getASTContext()}, {"entry"});
    if (!gap) {
      std::cerr << llvm::toString(gap.takeError()) << '\n';
      return 1;
    }
    bool retainedGap = false;
    for (const auto &body : gap->cfgs)
      retainedGap |= !body->unmodeledCalls.empty();
    if (!check(retainedGap &&
                   gap->hasLimitation("CFG call effects not captured") &&
                   !gap->closedWithinInputs(),
               "a CFG call missing from current facts remains an explicit gap"))
      return 1;
  }
  return 0;
}

int defaultExpressions() {
  // Clang's default-expression wrappers can hide calls from both the current
  // body visitor and CFG options. Their absence is not closed call coverage.
  for (
      const auto &[code, limitation] :
      {std::pair{"void use(int = hidden()) {} void entry() { use(); }",
                 "default argument effects not modeled"},
       std::pair{
           "struct Item { Item(int = hidden()) {} }; void entry() { Item item; }",
           "default argument effects not modeled"},
       std::pair{
           "struct Item { int value = hidden(); }; void entry() { Item item; }",
           "default member initializer effects not modeled"}}) {
    CallGraphSession defaults;
    {
      auto unit = clang::tooling::buildASTFromCodeWithArgs(
          std::string(
              "void target() {} int hidden() { target(); return 0; } ") +
              code,
          {"-std=c++20"}, "default_effects.cpp");
      if (!check(unit != nullptr, "parse hidden default-expression calls"))
        return 1;
      defaults.addUnit(unit->getASTContext());
    }
    auto incomplete = defaults.analyze({"entry"});
    if (!incomplete) {
      std::cerr << llvm::toString(incomplete.takeError()) << '\n';
      return 1;
    }
    if (!check(
            !incomplete->closedWithinInputs() &&
                incomplete->hasLimitation(limitation),
            "unsupported default-expression calls prevent false closure after AST release"))
      return 1;
  }
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "int hidden() { return 1; } void use(int = hidden()) {} "
        "void dead() { use(); } void entry() { use(0); }",
        {"-std=c++20"}, "unused_default.cpp");
    if (!check(unit != nullptr, "parse explicitly supplied argument"))
      return 1;
    auto explicitArgument = run({&unit->getASTContext()}, {"entry"});
    if (!explicitArgument) {
      std::cerr << llvm::toString(explicitArgument.takeError()) << '\n';
      return 1;
    }
    if (!check(
            explicitArgument->closedWithinInputs(),
            "unused defaults in declarations and unreachable bodies do not open coverage"))
      return 1;
  }
  return 0;
}

int declarationIdentity() {
  CallGraphSession systemHeader;
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "void target(void) {}\nstatic void helper(void);\n"
        "void entry(void) { helper(); }\n#include \"implementation.h\"\n",
        {"-std=c11"}, "/wpgkit-identity/main.c", "clang-tool",
        std::make_shared<clang::PCHContainerOperations>(),
        clang::tooling::getClangStripDependencyFileAdjuster(),
        {{"/wpgkit-identity/implementation.h",
          "#pragma clang system_header\nstatic void helper(void) { target(); }\n"}});
    if (!check(unit != nullptr, "parse helper defined in a system header"))
      return 1;
    systemHeader.addUnit(unit->getASTContext());
  }
  auto reconciled = systemHeader.analyze({"entry"});
  if (!reconciled) {
    std::cerr << llvm::toString(reconciled.takeError()) << '\n';
    return 1;
  }
  if (!check(
          edge(*reconciled, "entry", "helper") &&
              edge(*reconciled, "helper", "target") &&
              reconciled->closedWithinInputs(),
          "user declaration and system-header definition share an identity after AST release"))
    return 1;
  return 0;
}

int globalPointers() {
  auto globalUse = clang::tooling::buildASTFromCodeWithArgs(
      "extern void (*fp)(); void entry() { fp(); }", {"-std=c++20"},
      "global_use.cpp");
  auto globalDefinition = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void (*fp)() = &a;", {"-std=c++20"},
      "global_definition.cpp");
  if (!check(globalUse && globalDefinition, "parse cross-unit global fixture"))
    return 1;
  auto global =
      run({&globalUse->getASTContext(), &globalDefinition->getASTContext()},
          {"entry"});
  if (!global) {
    std::cerr << llvm::toString(global.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*global, "entry", "a") && !unresolved(*global),
             "USR reconciles global function pointer across units"))
    return 1;
  return 0;
}

int pointerAliases() {
  auto store = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void b() {} "
      "void entry() { void (*fp)() = &a; void (**pp)() = &fp; *pp = &b; fp(); }",
      {"-std=c++20"}, "store.cpp");
  if (!check(store != nullptr, "parse pointer-store fixture"))
    return 1;
  auto stored = run({&store->getASTContext()}, {"entry"});
  if (!stored) {
    std::cerr << llvm::toString(stored.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*stored, "entry", "a") && edge(*stored, "entry", "b"),
             "store through pointer adds possible call target"))
    return 1;

  auto alias = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void b() {} "
      "void set(void (**out)()) { *out = &b; } "
      "void entry() { void (*fp)() = &a; set(&fp); fp(); }",
      {"-std=c++20"}, "alias.cpp");
  if (!check(alias != nullptr, "parse cross-function alias fixture"))
    return 1;
  auto throughAlias = run({&alias->getASTContext()}, {"entry"});
  if (!throughAlias) {
    std::cerr << llvm::toString(throughAlias.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*throughAlias, "entry", "set") &&
                 edge(*throughAlias, "entry", "a") &&
                 edge(*throughAlias, "entry", "b"),
             "formal pointer alias changes caller's function target"))
    return 1;
  return 0;
}

int pointerReturns() {
  auto returned = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void (*choose())() { return &a; } "
      "void entry() { choose()(); }",
      {"-std=c++20"}, "return.cpp");
  if (!check(returned != nullptr, "parse returned callback fixture"))
    return 1;
  auto throughReturn = run({&returned->getASTContext()}, {"entry"});
  if (!throughReturn) {
    std::cerr << llvm::toString(throughReturn.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*throughReturn, "entry", "choose") &&
                 edge(*throughReturn, "entry", "a"),
             "returned function pointer adds indirect edge"))
    return 1;
  return 0;
}

int recursiveCalls() {
  auto recursion = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} "
      "void recur(void (*cb)()) { cb(); if (cb) recur(cb); } "
      "void entry() { recur(&target); }",
      {"-std=c++20"}, "recursive.cpp");
  if (!check(recursion != nullptr, "parse recursive callback fixture"))
    return 1;
  auto recursive = run({&recursion->getASTContext()}, {"entry"});
  if (!recursive) {
    std::cerr << llvm::toString(recursive.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*recursive, "recur", "recur") &&
                 edge(*recursive, "recur", "target") && !unresolved(*recursive),
             "recursive call graph converges without rescanning body"))
    return 1;

  auto unknown = clang::tooling::buildASTFromCodeWithArgs(
      "void entry(void (*callback)()) { callback(); }", {"-std=c++20"},
      "unknown.cpp");
  if (!check(unknown != nullptr, "parse unknown-input fixture"))
    return 1;
  auto open = run({&unknown->getASTContext()}, {"entry"});
  if (!open) {
    std::cerr << llvm::toString(open.takeError()) << '\n';
    return 1;
  }
  if (!check(unresolved(*open), "unmodeled root input retains unknown edge"))
    return 1;
  if (!check(!open->closedWithinInputs(),
             "unknown root input prevents closed call coverage"))
    return 1;
  return 0;
}

int pointerMutation() {
  auto mutation = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} "
      "void entry() { void (*fp)() = &target; void (**p)() = &fp; "
      "p += 0; (*p)(); }",
      {"-std=c++20"}, "mutation.cpp");
  if (!check(mutation != nullptr, "parse pointer mutation fixture"))
    return 1;
  auto mutated = run({&mutation->getASTContext()}, {"entry"});
  if (!mutated) {
    std::cerr << llvm::toString(mutated.takeError()) << '\n';
    return 1;
  }
  if (!check(!mutated->closedWithinInputs() &&
                 mutated->hasLimitation(
                     "pointer compound assignment not modeled") &&
                 unresolved(*mutated),
             "pointer mutation cannot silently preserve a closed call target"))
    return 1;
  return 0;
}

int defaultDeallocation() {
  auto deletion = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} void entry(int *pointer) { delete pointer; target(); }",
      {"-std=c++20"}, "deletion.cpp");
  if (!check(deletion != nullptr, "parse deallocation fixture"))
    return 1;
  auto deleted = run({&deletion->getASTContext()}, {"entry"});
  if (!deleted) {
    std::cerr << llvm::toString(deleted.takeError()) << '\n';
    return 1;
  }
  if (!check(deleted->closedWithinInputs() && !unresolved(*deleted),
             "default scalar deallocation has a complete lifetime contract"))
    return 1;
  return 0;
}

int implicitDestruction() {
  auto destruction = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} "
      "struct Item { ~Item() { target(); } }; "
      "void entry(Item *pointer) { Item local{}; delete pointer; Item{}; }",
      {"-std=c++20"}, "destruction.cpp");
  if (!check(destruction != nullptr, "parse destructor fixture"))
    return 1;
  auto destructionResult = run({&destruction->getASTContext()}, {"entry"});
  if (!destructionResult) {
    std::cerr << llvm::toString(destructionResult.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*destructionResult, "entry", "Item::~Item") &&
                 edge(*destructionResult, "Item::~Item", "target") &&
                 destructionResult->closedWithinInputs(),
             "local, temporary, and delete destructor bodies are reachable"))
    return 1;

  auto localDestructor = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} struct Item { ~Item() { target(); } }; "
      "void entry() { Item local; }",
      {"-std=c++20"}, "local_destructor.cpp");
  if (!check(localDestructor != nullptr, "parse uninitialized local fixture"))
    return 1;
  auto localDestructorResult =
      run({&localDestructor->getASTContext()}, {"entry"});
  if (!localDestructorResult) {
    std::cerr << llvm::toString(localDestructorResult.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*localDestructorResult, "entry", "Item::~Item") &&
                 edge(*localDestructorResult, "Item::~Item", "target"),
             "local destructor is indexed without an initializer"))
    return 1;
  return 0;
}

int memberCallbacks() {
  auto objectCalls = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} "
      "struct Holder { void (*callback)(); "
      "Holder(void (*f)()) : callback(f) {} "
      "void run() { callback(); } }; "
      "void entry() { Holder h(&target); h.run(); }",
      {"-std=c++20"}, "object_calls.cpp");
  if (!check(objectCalls != nullptr, "parse object call fixture"))
    return 1;
  auto objectResult = run({&objectCalls->getASTContext()}, {"entry"});
  if (!objectResult) {
    std::cerr << llvm::toString(objectResult.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*objectResult, "entry", "Holder::Holder") &&
                 edge(*objectResult, "entry", "Holder::run") &&
                 edge(*objectResult, "Holder::run", "target"),
             "constructor initializer feeds field callback call"))
    return 1;

  auto externalField = clang::tooling::buildASTFromCodeWithArgs(
      "struct Holder { void (*callback)(); }; "
      "void target() {} "
      "void entry(Holder *external) { Holder local{&target}; "
      "external->callback(); local.callback(); }",
      {"-std=c++20"}, "external_field.cpp");
  if (!check(externalField != nullptr, "parse external field fixture"))
    return 1;
  auto externalFieldResult = run({&externalField->getASTContext()}, {"entry"});
  if (!externalFieldResult) {
    std::cerr << llvm::toString(externalFieldResult.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*externalFieldResult, "entry", "target") &&
                 unresolved(*externalFieldResult),
             "unknown object field retains an unknown callback target"))
    return 1;
  return 0;
}

int globalConstruction() {
  auto globalConstructor = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void b() {} "
      "struct Holder { void (*callback)(); "
      "constexpr Holder(void (*f)()) : callback(f) {} }; "
      "Holder global(&a); "
      "void entry() { Holder local(&b); global.callback(); }",
      {"-std=c++20"}, "global_constructor.cpp");
  if (!check(globalConstructor != nullptr,
             "parse constant global constructor fixture"))
    return 1;
  auto globalConstructorResult =
      run({&globalConstructor->getASTContext()}, {"entry"});
  if (!globalConstructorResult) {
    std::cerr << llvm::toString(globalConstructorResult.takeError()) << '\n';
    return 1;
  }
  if (!check(!globalConstructorResult->closedWithinInputs() &&
                 globalConstructorResult->hasLimitation(
                     "constant global record constructor not modeled"),
             "global record construction cannot borrow local field targets"))
    return 1;
  return 0;
}

int lambdaCalls() {
  auto lambda = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} void entry() { auto call = [] { target(); }; call(); }",
      {"-std=c++20"}, "lambda.cpp");
  if (!check(lambda != nullptr, "parse captureless lambda fixture"))
    return 1;
  auto lambdaResult = run({&lambda->getASTContext()}, {"entry"});
  if (!lambdaResult) {
    std::cerr << llvm::toString(lambdaResult.takeError()) << '\n';
    return 1;
  }
  bool reachedLambdaTarget = false;
  for (const auto &site : lambdaResult->calls)
    for (const auto &target : site.targets)
      if (lambdaResult->metadata.text(lambdaResult->names.at(target)) ==
              "target" &&
          lambdaResult->metadata.text(lambdaResult->names.at(site.caller))
                  .find("operator()") != std::string::npos)
        reachedLambdaTarget = true;
  if (!check(reachedLambdaTarget, "lambda call operator body reaches target"))
    return 1;

  auto lambdaCallback = clang::tooling::buildASTFromCodeWithArgs(
      "void target() {} void entry() { "
      "auto invoke = [](void (*f)()) { f(); }; invoke(&target); }",
      {"-std=c++20"}, "lambda_callback.cpp");
  if (!check(lambdaCallback != nullptr, "parse lambda callback fixture"))
    return 1;
  auto lambdaCallbackResult =
      run({&lambdaCallback->getASTContext()}, {"entry"});
  if (!lambdaCallbackResult) {
    std::cerr << llvm::toString(lambdaCallbackResult.takeError()) << '\n';
    return 1;
  }
  bool callbackReached = false;
  for (const auto &site : lambdaCallbackResult->calls)
    for (const auto &target : site.targets)
      if (lambdaCallbackResult->metadata.text(
              lambdaCallbackResult->names.at(target)) == "target" &&
          lambdaCallbackResult->metadata
                  .text(lambdaCallbackResult->names.at(site.caller))
                  .find("operator()") != std::string::npos)
        callbackReached = true;
  if (!check(callbackReached,
             "lambda formal receives explicit callback argument"))
    return 1;

  return 0;
}

int bodyVariants() {
  const char *variantCode = "void a() {} void b() {} inline void pick() {\n"
                            "#ifdef CHOICE_A\n a();\n#else\n b();\n#endif\n"
                            "}\nvoid entry() { void (*fp)() = &pick; fp(); }\n";
  auto variantA = clang::tooling::buildASTFromCodeWithArgs(
      variantCode, {"-std=c++20", "-DCHOICE_A"}, "variant.cpp");
  auto variantB = clang::tooling::buildASTFromCodeWithArgs(
      variantCode, {"-std=c++20"}, "variant.cpp");
  if (!check(variantA && variantB, "parse multiple compilation variants"))
    return 1;
  auto variants =
      run({&variantA->getASTContext(), &variantB->getASTContext()}, {"entry"});
  if (!variants) {
    std::cerr << llvm::toString(variants.takeError()) << '\n';
    return 1;
  }
  if (!check(
          edge(*variants, "pick", "a") && !variants->closedWithinInputs() &&
              variants->hasLimitation("conflicting function definitions") &&
              variants->cfgs.size() == 4 && unresolvedEdge(*variants, "pick"),
          "conflicting definitions retain one body and explicit unknown effects"))
    return 1;
  return 0;
}

int pointerReferences() {
  auto reference = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void b() {} "
      "void set(void (*&out)()) { out = &b; } "
      "void entry() { void (*fp)() = &a; set(fp); fp(); }",
      {"-std=c++20"}, "reference.cpp");
  if (!check(reference != nullptr, "parse pointer-reference fixture"))
    return 1;
  auto throughReference = run({&reference->getASTContext()}, {"entry"});
  if (!throughReference) {
    std::cerr << llvm::toString(throughReference.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*throughReference, "entry", "a") &&
                 edge(*throughReference, "entry", "b") &&
                 throughReference->closedWithinInputs(),
             "pointer-reference parameter updates caller callback"))
    return 1;

  auto referenceReturn = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void b() {} void (*global)() = &b; "
      "auto get() -> void (*&)() { return global; } "
      "void entry() { void (*fp)() = &a; fp = get(); fp(); }",
      {"-std=c++20"}, "reference_return.cpp");
  if (!check(referenceReturn != nullptr,
             "parse pointer-reference return fixture"))
    return 1;
  auto fromReference = run({&referenceReturn->getASTContext()}, {"entry"});
  if (!fromReference) {
    std::cerr << llvm::toString(fromReference.takeError()) << '\n';
    return 1;
  }
  // The local definition overwrites fp with the loaded global value.
  if (!check(!edge(*fromReference, "entry", "a") &&
                 edge(*fromReference, "entry", "b") &&
                 fromReference->closedWithinInputs(),
             "pointer-reference return aliases global callback"))
    return 1;

  auto referenceField = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void b() {} "
      "struct Holder { void (*&callback)(); "
      "Holder(void (*&input)()) : callback(input) {} "
      "void update() { callback = &b; } }; "
      "void entry() { void (*fp)() = &a; Holder holder(fp); "
      "holder.update(); holder.callback(); fp(); }",
      {"-std=c++20"}, "reference_field.cpp");
  if (!check(referenceField != nullptr, "parse reference field fixture"))
    return 1;
  auto referenceFieldResult =
      run({&referenceField->getASTContext()}, {"entry"});
  if (!referenceFieldResult) {
    std::cerr << llvm::toString(referenceFieldResult.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*referenceFieldResult, "entry", "a") &&
                 edge(*referenceFieldResult, "entry", "b"),
             "reference field aliases its callback storage"))
    return 1;
  return 0;
}

int globalInitializers() {
  auto constantGlobal = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} constexpr auto choose() -> void (*)() { return &a; } "
      "void (*global)() = choose(); void entry() { global(); }",
      {"-std=c++20"}, "constant_global.cpp");
  if (!check(constantGlobal != nullptr, "parse constant global call fixture"))
    return 1;
  auto initialized = run({&constantGlobal->getASTContext()}, {"entry"});
  if (!initialized) {
    std::cerr << llvm::toString(initialized.takeError()) << '\n';
    return 1;
  }
  if (!check(!initialized->closedWithinInputs() && unresolved(*initialized),
             "constant global call initializer remains conservatively open"))
    return 1;
  return 0;
}

int volatilePointers() {
  auto volatilePointer = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void (*volatile callback)() = &a; "
      "void entry() { callback(); }",
      {"-std=c++20"}, "volatile_pointer.cpp");
  if (!check(volatilePointer != nullptr, "parse volatile pointer fixture"))
    return 1;
  auto volatileResult = run({&volatilePointer->getASTContext()}, {"entry"});
  if (!volatileResult) {
    std::cerr << llvm::toString(volatileResult.takeError()) << '\n';
    return 1;
  }
  if (!check(!volatileResult->closedWithinInputs() &&
                 volatileResult->hasLimitation(
                     "volatile pointer access not modeled") &&
                 unresolved(*volatileResult),
             "volatile pointer cannot silently retain one target"))
    return 1;
  return 0;
}

int assemblyEffects() {
  auto assembly = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void entry() { void (*fp)() = &a; "
      "asm volatile(\"\" ::: \"memory\"); fp(); }",
      {"-std=c++20"}, "assembly.cpp");
  if (!check(assembly != nullptr, "parse inline assembly fixture"))
    return 1;
  auto assemblyResult = run({&assembly->getASTContext()}, {"entry"});
  if (!assemblyResult) {
    std::cerr << llvm::toString(assemblyResult.takeError()) << '\n';
    return 1;
  }
  if (!check(!assemblyResult->closedWithinInputs() &&
                 assemblyResult->hasLimitation(
                     "inline assembly effects not indexed") &&
                 unresolved(*assemblyResult),
             "inline assembly conservatively opens later indirect calls"))
    return 1;
  return 0;
}

int libraryDeclarations() {
  auto library = clang::tooling::buildASTFromCodeWithArgs(
      "#include <string>\n"
      "void entry(std::string &text) { text.find('x'); }",
      {"-std=c++20"}, "library.cpp");
  if (!check(library != nullptr, "parse standard library call fixture"))
    return 1;
  auto libraryCalls = run({&library->getASTContext()}, {"entry"});
  if (!libraryCalls) {
    std::cerr << llvm::toString(libraryCalls.takeError()) << '\n';
    return 1;
  }
  for (const auto &site : libraryCalls->calls)
    for (const auto &target : site.targets)
      if (!check(libraryCalls->names.contains(target),
                 "every discovered target has a reportable name"))
        return 1;
  return 0;
}

int missingBodies() {
  auto external = clang::tooling::buildASTFromCodeWithArgs(
      "void a() {} void opaque(); "
      "void entry() { void (*fp)() = &a; opaque(); fp(); }",
      {"-std=c++20"}, "external.cpp");
  if (!check(external != nullptr, "parse unavailable-body fixture"))
    return 1;
  auto effects = run({&external->getASTContext()}, {"entry"});
  if (!effects) {
    std::cerr << llvm::toString(effects.takeError()) << '\n';
    return 1;
  }
  if (!check(
          edge(*effects, "entry", "a") && !unresolvedEdge(*effects, "a") &&
              unresolvedEdge(*effects, "opaque") &&
              effects->hasLimitation("call target definition unavailable"),
          "unavailable body stays open without contaminating a local function address"))
    return 1;
  return 0;
}

int virtualCalls() {
  auto virtualCall = clang::tooling::buildASTFromCodeWithArgs(
      "struct Base { virtual void run() {} }; "
      "struct Derived : Base { void run() override {} }; "
      "void entry(Base *object) { object->run(); }",
      {"-std=c++20"}, "virtual.cpp");
  if (!check(virtualCall != nullptr, "parse virtual-call fixture"))
    return 1;
  auto dynamic = run({&virtualCall->getASTContext()}, {"entry"});
  if (!dynamic) {
    std::cerr << llvm::toString(dynamic.takeError()) << '\n';
    return 1;
  }
  if (!check(
          unresolved(*dynamic) &&
              dynamic->hasLimitation(
                  "virtual dispatch may have targets outside supplied units"),
          "virtual dispatch retains an explicit unknown target"))
    return 1;

  auto virtualEntry = clang::tooling::buildASTFromCodeWithArgs(
      "struct Base { virtual void run(); }; "
      "void entry(Base *object) { object->run(); }",
      {"-std=c++20"}, "virtual_entry.cpp");
  auto virtualDefinitions = clang::tooling::buildASTFromCodeWithArgs(
      "struct Base { virtual void run(); }; "
      "struct Derived : Base { void run() override; }; "
      "void Base::run() {} void Derived::run() {}",
      {"-std=c++20"}, "virtual_definitions.cpp");
  if (!check(virtualEntry && virtualDefinitions,
             "parse cross-unit virtual fixture"))
    return 1;
  auto virtualTargets = run(
      {&virtualEntry->getASTContext(), &virtualDefinitions->getASTContext()},
      {"entry"});
  if (!virtualTargets) {
    std::cerr << llvm::toString(virtualTargets.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*virtualTargets, "entry", "Base::run") &&
                 edge(*virtualTargets, "entry", "Derived::run") &&
                 unresolved(*virtualTargets),
             "known cross-unit overrides augment an open virtual call"))
    return 1;
  return 0;
}

int incrementalImports() {
  CallGraphSession owned;
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "void relay(void (*)()); void target() {} "
        "void entry() { relay(&target); }",
        {"-std=c++20"}, "owned_caller.cpp");
    if (!check(unit != nullptr, "parse owned caller"))
      return 1;
    owned.addUnit(unit->getASTContext());
  }
  auto firstPass = owned.analyze({"entry"});
  if (!firstPass) {
    std::cerr << llvm::toString(firstPass.takeError()) << '\n';
    return 1;
  }
  if (!check(
          edge(*firstPass, "entry", "relay") && unresolved(*firstPass),
          "owned facts retain an unresolved cross-unit call after AST release"))
    return 1;
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "void relay(void (*callback)()) { callback(); }", {"-std=c++20"},
        "owned_callee.cpp");
    if (!check(unit != nullptr, "parse owned callee"))
      return 1;
    owned.addUnit(unit->getASTContext());
  }
  auto secondPass = owned.analyze({"entry"});
  if (!secondPass) {
    std::cerr << llvm::toString(secondPass.takeError()) << '\n';
    return 1;
  }
  if (!check(edge(*secondPass, "entry", "relay") &&
                 edge(*secondPass, "relay", "target") &&
                 secondPass->closedWithinInputs(),
             "owned facts resolve a later unit without retaining either AST"))
    return 1;
  auto repeated = owned.analyze({"entry"});
  if (!repeated) {
    std::cerr << llvm::toString(repeated.takeError()) << '\n';
    return 1;
  }
  if (!check(
          repeated->calls.size() == secondPass->calls.size() &&
              repeated->statistics == secondPass->statistics &&
              edge(*repeated, "relay", "target") &&
              repeated->closedWithinInputs(),
          "repeated owned-fact analysis preserves state without solver work"))
    return 1;
  if (!check(
          firstPass->cfgs.size() == 2 && secondPass->cfgs.size() == 3 &&
              firstPass->cfgs.front() == secondPass->cfgs.front() &&
              repeated->cfgs == secondPass->cfgs,
          "incremental snapshots share immutable CFGs without changing earlier inventories"))
    return 1;
  return 0;
}

int instantiatedCallbacks() {
  const char *templateCallbacks =
      "template<class T> void target() {} "
      "template<class T> void relay(void (*callback)()) { callback(); } "
      "void unused() { asm volatile(\"\" ::: \"memory\"); } "
      "template<class T> void start() { "
      "void (*fp)(void (*)()) = &relay<T>; fp(&target<T>); } "
      "void entry() { start<int>(); }";
  CallGraphSession upfront;
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        templateCallbacks, {"-std=c++20"}, "upfront.cpp");
    if (!check(unit != nullptr, "parse all instantiated callback bodies once"))
      return 1;
    upfront.addUnit(unit->getASTContext());
  }
  auto capturedUpfront = upfront.analyze({"entry"});
  if (!capturedUpfront) {
    std::cerr << llvm::toString(capturedUpfront.takeError()) << '\n';
    return 1;
  }
  if (!check(
          capturedUpfront->closedWithinInputs() &&
              edge(*capturedUpfront, "start", "relay") &&
              edge(*capturedUpfront, "relay", "target"),
          "nested template callbacks converge after AST release without activating unused effects"))
    return 1;
  return 0;
}

int sharedTemplateExpressions() {
  CallGraphSession sharedExpressions;
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "struct Item { Item() {} }; "
        "template<class T> Item make() { return Item(); } "
        "void unused() { make<int>(); } void entry() { make<long>(); }",
        {"-std=c++20"}, "shared.cpp");
    if (!check(unit != nullptr, "parse shared template expressions"))
      return 1;
    sharedExpressions.addUnit(unit->getASTContext());
  }
  auto sharedResult = sharedExpressions.analyze({"entry"});
  if (!sharedResult) {
    std::cerr << llvm::toString(sharedResult.takeError()) << '\n';
    return 1;
  }
  if (!check(
          edge(*sharedResult, "make", "Item::Item") &&
              sharedResult->closedWithinInputs() &&
              validCFGCalls(*sharedResult),
          "shared template expressions retain their owning body's calls and CFG mappings"))
    return 1;
  return 0;
}

int instantiatedOverrides() {
  CallGraphSession instantiatedOverrides;
  {
    auto unit = clang::tooling::buildASTFromCodeWithArgs(
        "struct Base { virtual void run() {} }; "
        "template<class T> struct Derived : Base { void run() override {} }; "
        "Derived<int> object; void entry(Base *base) { base->run(); }",
        {"-std=c++20"}, "instantiated_override.cpp");
    if (!check(unit != nullptr, "parse instantiated virtual override"))
      return 1;
    instantiatedOverrides.addUnit(unit->getASTContext());
  }
  auto instantiatedResult = instantiatedOverrides.analyze({"entry"});
  if (!instantiatedResult) {
    std::cerr << llvm::toString(instantiatedResult.takeError()) << '\n';
    return 1;
  }
  if (!check(
          edge(*instantiatedResult, "entry", "Derived<int>::run") &&
              !edge(*instantiatedResult, "entry", "Derived::run"),
          "dispatch indexes concrete template overrides, not dependent patterns"))
    return 1;
  return 0;
}

} // namespace

bool fieldSensitivityOptions() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(R"cpp(
    void first() {} void second() {} void unrelated() {}
    struct Callbacks { void (*left)(); void (*right)(); };
    void entry() {
      Callbacks selected{first, second}, other{unrelated, unrelated};
      selected.left();
    }
  )cpp",
                                                      {"-std=c++20"},
                                                      "field_options.cpp");
  if (!ast || ast->getDiagnostics().hasErrorOccurred())
    return false;
  auto defaults = CallGraphPTA::run({&ast->getASTContext()}, {"entry"});
  if (!defaults) {
    std::cerr << llvm::toString(defaults.takeError()) << '\n';
    return false;
  }
  if (!check(edge(*defaults, "entry", "first") &&
                 edge(*defaults, "entry", "second") &&
                 !edge(*defaults, "entry", "unrelated"),
             "default run merges fields but separates objects"))
    return false;
  wpgkit::AnalysisOptions options{.fieldSensitive = true};
  CallGraphSession session(nullptr, {}, {}, options);
  options.fieldSensitive = false;
  session.addUnit(ast->getASTContext());
  auto run = CallGraphPTA::run({&ast->getASTContext()}, {"entry"}, nullptr, {},
                               {}, {.fieldSensitive = true});
  ast.reset();
  auto precise = session.finalize({"entry"});
  for (auto *result : {&run, &precise}) {
    if (!*result) {
      std::cerr << llvm::toString(result->takeError()) << '\n';
      return false;
    }
    if (!check(edge(**result, "entry", "first") &&
                   !edge(**result, "entry", "second"),
               "explicit field mode is copied and survives AST release"))
      return false;
  }
  return true;
}

int main() {
  if (!fieldSensitivityOptions())
    return 1;
  for (const auto test : {parameterAndReferenceStorage,
                          crossUnitCalls,
                          ownedCFGLifetime,
                          implicitCleanupGap,
                          defaultExpressions,
                          declarationIdentity,
                          globalPointers,
                          pointerAliases,
                          pointerReturns,
                          recursiveCalls,
                          pointerMutation,
                          defaultDeallocation,
                          implicitDestruction,
                          memberCallbacks,
                          globalConstruction,
                          lambdaCalls,
                          bodyVariants,
                          pointerReferences,
                          globalInitializers,
                          volatilePointers,
                          assemblyEffects,
                          libraryDeclarations,
                          missingBodies,
                          virtualCalls,
                          incrementalImports,
                          instantiatedCallbacks,
                          sharedTemplateExpressions,
                          instantiatedOverrides})
    if (test() != 0)
      return 1;
  return 0;
}
