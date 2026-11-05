#include "CallGraphPTA.h"
#include "InterCFG.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace wpgkit;

static_assert(
    std::is_same_v<decltype(CallGraphResult::names)::mapped_type, TextId>);
static_assert(
    std::is_same_v<decltype(CallGraphResult::usrs)::mapped_type, TextId>);
static_assert(
    std::is_same_v<decltype(CallGraphResult::limitations)::value_type, TextId>);

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void add(CallGraphSession &session, const char *code, const char *file) {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"}, file);
  check(ast != nullptr, "parse incremental fixture");
  session.addUnit(ast->getASTContext());
} // Every AST is destroyed before analyze().

CallGraphResult analyze(CallGraphSession &session) {
  auto result = session.analyze({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  return std::move(*result);
}

bool edge(const CallGraphResult &result, const char *caller,
          const char *target) {
  for (const auto &site : result.calls)
    if (result.metadata.text(result.names.at(site.caller)) == caller)
      for (const auto id : site.targets)
        if (result.metadata.text(result.names.at(id)) == target)
          return true;
  return false;
}

auto graph(const CallGraphResult &result) {
  using Row = std::tuple<std::string, std::string, std::set<std::string>, bool>;
  std::multiset<Row> rows;
  for (const auto &site : result.calls) {
    std::set<std::string> targets;
    for (const auto id : site.targets)
      targets.insert(result.metadata.text(result.usrs.at(id)));
    rows.emplace(result.metadata.text(result.usrs.at(site.caller)),
                 result.metadata.format(result.locations.at(site.id)),
                 std::move(targets), site.unresolved);
  }
  return rows;
}

void stableIds(const CallGraphResult &before, const CallGraphResult &after) {
  for (const auto &[id, usr] : before.usrs)
    check(after.usrs.at(id) == usr &&
              after.metadata.text(usr) == before.metadata.text(usr),
          "function and text IDs survive late imports");
  for (const auto &[id, location] : before.locations)
    check(after.locations.at(id) == location &&
              after.metadata.format(location) ==
                  before.metadata.format(location),
          "call-site and source-file IDs survive late activation");
}

void sameWork(const CallGraphResult &incremental,
              const CallGraphResult &batch) {
  check(graph(incremental) == graph(batch),
        "incremental graph equals batch graph");
  check(
      incremental.statistics.activatedBodies ==
              batch.statistics.activatedBodies &&
          incremental.statistics.connectedCalls ==
              batch.statistics.connectedCalls &&
          incremental.statistics.pointerConstraints ==
              batch.statistics.pointerConstraints,
      "imports do not replay bodies or duplicate interprocedural constraints");
}
} // namespace

int main() {
  const char *macroCode =
      "#define INVOKE() target()\n"
      "void target() {} void entry() { INVOKE(); INVOKE(); }";
  CallGraphResult ownedLocations;
  {
    CallGraphSession locations;
    add(locations, macroCode, "macro:source.cpp");
    ownedLocations = analyze(locations);
  }
  check(
      ownedLocations.closedWithinInputs() && ownedLocations.calls.size() == 2,
      "an owned graph keeps its metadata after both AST and session destruction");
  const auto firstLocation =
      ownedLocations.locations.at(ownedLocations.calls[0].id);
  const auto secondLocation =
      ownedLocations.locations.at(ownedLocations.calls[1].id);
  const auto fileId = ownedLocations.metadata.find("macro:source.cpp");
  check(
      fileId.isValid() && firstLocation.expansion.file == fileId &&
          firstLocation.spelling.file == fileId &&
          secondLocation.expansion.file == fileId &&
          secondLocation.spelling.file == fileId &&
          firstLocation.expansion.offset != secondLocation.expansion.offset &&
          firstLocation.spelling == secondLocation.spelling,
      "repeated macro calls share one file ID and preserve distinct integer source coordinates");
  check(ownedLocations.metadata.text(ownedLocations.names.at(
            ownedLocations.calls[0].caller)) == "entry" &&
            edge(ownedLocations, "entry", "target"),
        "integer graph attributes resolve to the original function metadata");

  // Moving out a terminal result must retain earlier snapshots and destroy the
  // solver without invalidating text, CFG ownership, targets, or summaries.
  CallGraphResult terminal, earlier;
  {
    CallGraphSession session;
    add(session,
        "using Fn = void (*)(); void target() {} Fn external(Fn); "
        "void entry() { external(&target)(); }",
        "terminal.cpp");
    earlier = analyze(session);
    const auto oldGraph = graph(earlier);
    const auto oldOverlay = earlier.interCFG;
    auto snapshot = session.finalize({"entry"});
    check(static_cast<bool>(snapshot), "finalize reusable session");
    auto consumed = std::move(session).finalize({"entry"});
    check(static_cast<bool>(consumed), "consume finalized session");
    check(
        graph(*consumed) == graph(*snapshot) &&
            consumed->statistics == snapshot->statistics &&
            consumed->reachable == snapshot->reachable &&
            consumed->limitations == snapshot->limitations &&
            consumed->unresolvedBodies == snapshot->unresolvedBodies &&
            consumed->interCFG == snapshot->interCFG,
        "consumption preserves the complete snapshot without repeating solver work");
    stableIds(*snapshot, *consumed);
    check(graph(earlier) == oldGraph && earlier.interCFG == oldOverlay &&
              !earlier.finalized && !earlier.interCFG->inputFinalized(),
          "consumption and finalization preserve older immutable snapshots");
    terminal = std::move(*consumed);
  }
  check(
      terminal.finalized && edge(terminal, "entry", "target") &&
          !terminal.closedWithinInputs() && terminal.interCFG->inputFinalized(),
      "consumed result retains known targets and conservative external effects");
  for (const auto &body : terminal.cfgs)
    if (body->graph)
      check(body->graph->block(body->graph->entry()),
            "consumed CFG survives session");
  terminal.metadata.intern("terminal result extension");
  check(!earlier.metadata.find("terminal result extension").isValid(),
        "consumed metadata remains independently extensible");

  CallGraphSession invalidTerminal;
  auto invalidResult = std::move(invalidTerminal).finalize({});
  check(!invalidResult, "consuming finalization reports invalid roots");
  llvm::consumeError(invalidResult.takeError());
  invalidTerminal = CallGraphSession{};
  add(invalidTerminal, "void entry() {}", "replacement.cpp");
  check(analyze(invalidTerminal).closedWithinInputs(),
        "consumed session can be move-assigned");

  const char *caller = "using Fn = void (*)(); Fn choose(); void target() {} "
                       "void entry() { choose()(); }";
  const char *callee =
      "using Fn = void (*)(); void target(); void aaa_unused() {} "
      "Fn choose() { return &target; }";
  CallGraphSession growing;
  add(growing, caller, "caller.cpp");
  const auto before = analyze(growing);
  check(!before.closedWithinInputs(), "unavailable return remains unresolved");
  add(growing, callee, "callee.cpp");
  const auto after = analyze(growing);
  check(after.closedWithinInputs() && edge(after, "entry", "target"),
        "late return discovers a target and removes provisional uncertainty");
  stableIds(before, after);
  const auto repeated = analyze(growing);
  check(repeated.statistics == after.statistics &&
            graph(repeated) == graph(after),
        "no new input means no body, call-binding, or PTA constraint work");
  CallGraphSession batch;
  add(batch, caller, "caller.cpp");
  add(batch, callee, "callee.cpp");
  sameWork(after, analyze(batch));
  CallGraphSession reverse;
  add(reverse, callee, "callee.cpp");
  add(reverse, caller, "caller.cpp");
  sameWork(after, analyze(reverse));

  const char *storeCaller =
      "using Fn = void (*)(); void set(Fn *); "
      "void a() {} void b() {} void entry() { Fn fp = &a; set(&fp); fp(); }";
  const char *storeCallee = "using Fn = void (*)(); void b(); "
                            "void set(Fn *out) { *out = &b; }";
  CallGraphSession stores;
  add(stores, storeCaller, "store_caller.cpp");
  analyze(stores);
  add(stores, storeCallee, "store_callee.cpp");
  const auto stored = analyze(stores);
  check(stored.closedWithinInputs() && edge(stored, "entry", "a") &&
            edge(stored, "entry", "b"),
        "late pointer store extends the caller's existing target set");
  CallGraphSession storeBatch;
  add(storeBatch, storeCaller, "store_caller.cpp");
  add(storeBatch, storeCallee, "store_callee.cpp");
  sameWork(stored, analyze(storeBatch));

  CallGraphSession affectedCalls;
  add(affectedCalls,
      "using Fn = void (*)(); void a() {} void b() {} "
      "Fn left = &a; Fn right = &b; void update(); "
      "void readLeft() { left(); } void readRight() { right(); } "
      "void entry() { readLeft(); readRight(); update(); }",
      "readers.cpp");
  const auto beforeUpdate = analyze(affectedCalls);
  add(affectedCalls,
      "using Fn = void (*)(); extern Fn left; "
      "void next() {} void update() { left = &next; }",
      "update.cpp");
  const auto afterUpdate = analyze(affectedCalls);
  check(
      afterUpdate.closedWithinInputs() &&
          edge(afterUpdate, "readLeft", "next") &&
          edge(afterUpdate, "readRight", "b") &&
          afterUpdate.statistics.resolvedIndirectCalls ==
              beforeUpdate.statistics.resolvedIndirectCalls + 1 &&
          afterUpdate.statistics.updatedFunctions ==
              beforeUpdate.statistics.updatedFunctions + 2,
      "changed global pointer revisits only its dependent call and newly reached functions");
  add(affectedCalls,
      "void unusedTarget() {} "
      "void unused() { void (*fp)() = &unusedTarget; fp(); }",
      "unused.cpp");
  const auto afterUnused = analyze(affectedCalls);
  check(
      afterUnused.statistics == afterUpdate.statistics &&
          afterUnused.interCFG == afterUpdate.interCFG,
      "unreachable pointer constraints neither update active functions nor copy the overlay");

  CallGraphSession availability;
  add(availability,
      "void left(); void right() {} void entry() { left(); right(); }",
      "availability.cpp");
  const auto unavailable = analyze(availability);
  add(availability, "void left() {}", "left.cpp");
  const auto captured = analyze(availability);
  check(captured.closedWithinInputs() && captured.unresolvedBodies.empty() &&
            captured.statistics.refreshedBodyStates ==
                unavailable.statistics.refreshedBodyStates + 1 &&
            captured.statistics.updatedFunctions ==
                unavailable.statistics.updatedFunctions + 1,
        "importing the missing body refreshes only its affected function");
  check(analyze(availability).statistics == captured.statistics,
        "a no-op result does not refresh unchanged function body states");
  add(availability, "void left() {}", "left.cpp");
  const auto duplicate = analyze(availability);
  check(
      duplicate.closedWithinInputs() && duplicate.cfgs == captured.cfgs &&
          duplicate.statistics == captured.statistics &&
          duplicate.interCFG == captured.interCFG,
      "an identical definition reuses its CFG, overlay and facts without solver work");

  CallGraphSession recursive;
  add(recursive,
      "void relay(void (*)(), bool); void target() {} "
      "void entry() { relay(&target, true); }",
      "recursive_caller.cpp");
  analyze(recursive);
  add(recursive,
      "void relay(void (*cb)(), bool again) { "
      "if (again) relay(cb, false); cb(); }",
      "recursive_callee.cpp");
  const auto recursed = analyze(recursive);
  check(recursed.closedWithinInputs() && edge(recursed, "relay", "relay") &&
            edge(recursed, "relay", "target") &&
            analyze(recursive).statistics == recursed.statistics,
        "late recursive callback reaches a stable incremental fixed point");

  CallGraphSession globals;
  add(globals, "extern void (*callback)(); void entry() { callback(); }",
      "reader.cpp");
  const auto withoutGlobal = analyze(globals);
  add(globals, "void late() {} void (*callback)() = &late;", "global.cpp");
  const auto withGlobal = analyze(globals);
  check(withGlobal.closedWithinInputs() && edge(withGlobal, "entry", "late"),
        "new global facts wake an existing indirect call");
  stableIds(withoutGlobal, withGlobal);

  const char *variantA =
      "void a() {} void b() {} "
      "void relay(void (*cb)()) { cb(); } void entry() { relay(&a); }";
  const char *variantB =
      "void a() {} void b() {} "
      "void relay(void (*cb)()) { cb(); } void entry() { relay(&b); }";
  CallGraphSession variants;
  add(variants, variantA, "header.cpp");
  const auto firstVariant = analyze(variants);
  add(variants, variantB, "header.cpp");
  const auto bothVariants = analyze(variants);
  check(
      !bothVariants.closedWithinInputs() && edge(bothVariants, "relay", "a") &&
          bothVariants.hasLimitation("conflicting function definitions") &&
          bothVariants.cfgs == firstVariant.cfgs &&
          bothVariants.statistics == firstVariant.statistics,
      "late conflicting definition opens coverage without duplicating or replaying its body");
  stableIds(firstVariant, bothVariants);
  CallGraphSession variantBatch;
  add(variantBatch, variantA, "header.cpp");
  add(variantBatch, variantB, "header.cpp");
  sameWork(bothVariants, analyze(variantBatch));

  // Include the same header in independent units. Repeated extraction must
  // neither allocate another graph/fact set nor reconnect existing callers.
  CallGraphSession shared;
  CallGraphResult firstShared;
  for (unsigned i = 0; i < 8; ++i) {
    auto ast = clang::tooling::buildASTFromCodeWithArgs(
        "#include \"shared.h\"\n", {"-std=c++20"},
        "/wpgkit-dedup/unit" + std::to_string(i) + ".cpp", "clang-tool",
        std::make_shared<clang::PCHContainerOperations>(),
        clang::tooling::getClangStripDependencyFileAdjuster(),
        {{"/wpgkit-dedup/shared.h",
          "inline void target() {} inline void target(int) {} "
          "template<class T> void invoke(void (*cb)()) { cb(); } "
          "inline void entry() { invoke<int>(&target); invoke<long>(&target); target(0); }"}});
    check(ast != nullptr,
          "parse repeated shared header with overloads and templates");
    shared.addUnit(ast->getASTContext());
    ast.reset();
    const auto current = analyze(shared);
    check(
        current.closedWithinInputs() && current.cfgs.size() == 5 &&
            current.statistics.activatedBodies == 5 &&
            current.calls.size() == 5,
        "overloads and template specializations each retain exactly one body");
    if (i == 0)
      firstShared = current;
    else {
      check(
          current.cfgs == firstShared.cfgs &&
              current.statistics == firstShared.statistics,
          "repeated header imports share existing CFGs and do no solver work");
      stableIds(firstShared, current);
    }
  }

  CallGraphSession locals;
  add(locals,
      "void left(); void right(); void a() {} void b() {} "
      "void entry() { left(); right(); }",
      "/wpgkit-dedup/main.cpp");
  for (bool left : {true, false}) {
    auto ast = clang::tooling::buildASTFromCodeWithArgs(
        std::string("void a(); void b();\n#define TARGET ") +
            (left ? "a" : "b") + "\n#include \"local.h\"\nvoid " +
            (left ? "left" : "right") + "() { local(); }",
        {"-std=c++20"},
        left ? "/wpgkit-dedup/left.cpp" : "/wpgkit-dedup/right.cpp",
        "clang-tool", std::make_shared<clang::PCHContainerOperations>(),
        clang::tooling::getClangStripDependencyFileAdjuster(),
        {{"/wpgkit-dedup/local.h", "static void (*fp)() = &TARGET; "
                                   "static void local() { fp(); }"}});
    check(ast != nullptr,
          "parse independent header-local functions and storage");
    locals.addUnit(ast->getASTContext());
  }
  const auto localResult = analyze(locals);
  unsigned localBodies = 0;
  for (const auto &cfg : localResult.cfgs)
    if (localResult.metadata.text(localResult.names.at(cfg->function)) ==
        "local") {
      ++localBodies;
      unsigned calls = 0;
      for (const auto &site : localResult.calls)
        if (site.caller == cfg->function) {
          ++calls;
          check(
              site.targets.size() == 1 && !site.unresolved,
              "file-local pointer storage does not leak targets between units");
        }
      check(calls == 1, "each file-local function retains its own call site");
    }
  check(
      localBodies == 2 && localResult.closedWithinInputs() &&
          edge(localResult, "local", "a") && edge(localResult, "local", "b"),
      "same-named static functions in a header are distinct program functions");

  // Different qualified callees with the same spelling must also be diagnosed.
  CallGraphSession qualifiedConflict;
  add(qualifiedConflict,
      "namespace A { void target() {} } namespace B { void target() {} } "
      "using A::target; void entry() { target(); }",
      "qualified.cpp");
  analyze(qualifiedConflict);
  add(qualifiedConflict,
      "namespace A { void target() {} } namespace B { void target() {} } "
      "using B::target; void entry() { target(); }",
      "qualified.cpp");
  const auto qualified = analyze(qualifiedConflict);
  check(
      qualified.cfgs.size() == 3 && !qualified.closedWithinInputs() &&
          qualified.hasLimitation("conflicting function definitions"),
      "resolved declaration identity participates in definition conflict detection");

  for (
      const auto *code :
      {"template<class T> void call() { TARGET(); } void entry() { call<int>(); }",
       "struct Item { int value; Item() : value((TARGET(), 0)) {} }; "
       "void entry() { Item item; }"}) {
    CallGraphSession conflicts;
    std::size_t count = 0;
    for (const auto *target : {"a", "b"}) {
      const auto source =
          std::string("void a() {} void b() {}\n#define TARGET ") + target +
          "\n" + code;
      add(conflicts, source.c_str(), "body_conflicts.cpp");
      if (count == 0)
        count = conflicts.cfgCount();
    }
    const auto conflicting = analyze(conflicts);
    check(
        conflicting.cfgs.size() == count && !conflicting.closedWithinInputs() &&
            conflicting.hasLimitation("conflicting function definitions"),
        "instantiated template bodies and constructor initializers detect conflicts");
  }

  CallGraphSession virtuals;
  add(virtuals,
      "struct Base { virtual void run() {} }; "
      "void entry(Base *b) { b->run(); }",
      "virtual.cpp");
  const auto firstVirtual = analyze(virtuals);
  add(virtuals,
      "struct Base { virtual void run(); }; "
      "struct Derived : Base { void run() override {} };",
      "override.cpp");
  const auto newVirtual = analyze(virtuals);
  check(
      edge(newVirtual, "entry", "Derived::run") &&
          !newVirtual.closedWithinInputs(),
      "late override extends existing virtual site and preserves open dispatch");
  stableIds(firstVirtual, newVirtual);

  CallGraphSession families;
  add(families,
      "struct Base { virtual void run() {} }; "
      "struct Other { virtual void run() {} }; "
      "void entry(Base *b, Other *o) { b->run(); o->run(); }",
      "families.cpp");
  const auto initialFamilies = analyze(families);
  add(families,
      "struct Unused { virtual void run() {} }; "
      "struct UnusedChild : Unused { void run() override {} };",
      "unrelated.cpp");
  const auto unrelated = analyze(families);
  check(unrelated.statistics == initialFamilies.statistics,
        "an unrelated override causes no function, dispatch, or PTA work");
  add(families,
      "struct Base { virtual void run(); }; "
      "struct Child : Base { void run() override {} };",
      "child.cpp");
  const auto child = analyze(families);
  check(edge(child, "entry", "Child::run") &&
            child.statistics.resolvedVirtualTargets ==
                unrelated.statistics.resolvedVirtualTargets + 1 &&
            child.statistics.updatedFunctions ==
                unrelated.statistics.updatedFunctions + 1,
        "one new override updates only its affected site and function");
  add(families,
      "struct Base { virtual void run(); }; "
      "struct Child : Base { void run() override; }; "
      "struct Leaf : Child { void run() override {} };",
      "leaf.cpp");
  const auto leaf = analyze(families);
  check(
      edge(leaf, "entry", "Leaf::run") &&
          leaf.statistics.resolvedVirtualTargets ==
              child.statistics.resolvedVirtualTargets + 1 &&
          leaf.statistics.updatedFunctions ==
              child.statistics.updatedFunctions + 1,
      "a transitive override notifies the original base call without revisiting other targets");

  CallGraphSession lateVirtualSite;
  add(lateVirtualSite,
      "struct Base { virtual void run() {} }; "
      "struct Child : Base { void run() override {} }; "
      "struct Leaf : Child { void run() override {} }; "
      "void later(Base *); void entry(Base *b) { later(b); }",
      "hierarchy.cpp");
  const auto hierarchyOnly = analyze(lateVirtualSite);
  add(lateVirtualSite,
      "struct Base { virtual void run(); }; "
      "void later(Base *b) { b->run(); }",
      "later.cpp");
  const auto lateSite = analyze(lateVirtualSite);
  check(
      edge(lateSite, "later", "Base::run") &&
          edge(lateSite, "later", "Child::run") &&
          edge(lateSite, "later", "Leaf::run") &&
          lateSite.statistics.resolvedVirtualTargets ==
              hierarchyOnly.statistics.resolvedVirtualTargets + 3 &&
          lateSite.statistics.updatedFunctions ==
              hierarchyOnly.statistics.updatedFunctions + 4,
      "a newly reached virtual site follows the complete existing override subtree");

  CallGraphSession diamond;
  add(diamond,
      "struct Root { virtual void run() {} }; "
      "struct Left : virtual Root { void run() override {} }; "
      "struct Right : virtual Root { void run() override {} }; "
      "void entry(Root *r) { r->run(); }",
      "diamond.cpp");
  const auto beforeDiamond = analyze(diamond);
  add(diamond,
      "struct Root { virtual void run(); }; "
      "struct Left : virtual Root { void run() override; }; "
      "struct Right : virtual Root { void run() override; }; "
      "struct Join : Left, Right { void run() override {} };",
      "join.cpp");
  const auto afterDiamond = analyze(diamond);
  check(edge(afterDiamond, "entry", "Join::run") &&
            afterDiamond.statistics.resolvedVirtualTargets ==
                beforeDiamond.statistics.resolvedVirtualTargets + 1,
        "two affected override paths produce only one new call target");

  CallGraphSession unknown;
  add(unknown,
      "void missing(); void entry(void (*external)()) { missing(); external(); }",
      "unknown.cpp");
  analyze(unknown);
  add(unknown, "void missing() {}", "missing.cpp");
  const auto stillUnknown = analyze(unknown);
  check(!stillUnknown.closedWithinInputs() &&
            stillUnknown.unresolvedBodies.empty(),
        "resolving a body preserves intrinsic unknown entry pointers");

  auto otherRoot = growing.analyze({"target"});
  check(!otherRoot, "changing roots requires an independent session");
  llvm::consumeError(otherRoot.takeError());
  check(analyze(growing).statistics == after.statistics,
        "rejected root change leaves the original analysis intact");

  CallGraphSession ambiguous;
  add(ambiguous, "void entry() {}", "entry.cpp");
  analyze(ambiguous);
  add(ambiguous, "void entry(int) {}", "overload.cpp");
  auto ambiguousRoot = ambiguous.analyze({"entry"});
  check(!ambiguousRoot,
        "indexed root lookup detects a late ambiguous definition");
  llvm::consumeError(ambiguousRoot.takeError());
}
