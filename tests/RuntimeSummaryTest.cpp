#include "CallGraphPTA.h"
#include "DataFlowGraph.h"
#include "InterCFG.h"
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

void add(CallGraphSession &session, const char *code,
         const char *language = "-std=c++20") {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {language, "-fno-builtin"}, "runtime-summary.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  session.addUnit(ast->getASTContext());
} // No AST survives into solving.

CallGraphResult solve(CallGraphSession &session, bool final = true) {
  auto result =
      final ? session.finalize({"entry"}) : session.analyze({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  return std::move(*result);
}

bool reached(const CallGraphResult &result, const char *name) {
  for (const auto id : result.reachable)
    if (result.metadata.text(result.names.at(id)) == name)
      return true;
  return false;
}

void summaryBoundary() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <stdlib.h>
    #include <pthread.h>
    int global;
    void nested() { ++global; }
    int compare(const void *, const void *) { nested(); return 0; }
    int compareContext(const void *, const void *, void *) { nested(); return 0; }
    void initialize() { nested(); }
    void entry() {
      int values[2] = {1, 2};
      pthread_once_t control = PTHREAD_ONCE_INIT;
      pthread_once(&control, &initialize);
      qsort(values, 2, sizeof(int), &compare);
      auto sort = &qsort_r;
      sort(values, 2, sizeof(int), &compareContext, &control);
      (void)bsearch(values, values, 2, sizeof(int), &compare);
    }
  )cpp");
  const auto provisional = solve(session, false);
  check(provisional.statistics.modeledCalls == 0,
        "runtime summaries wait for imports to finish");
  const auto result = solve(session);
  check(result.calls.size() == 4 && result.reachable.size() == 1 &&
            result.statistics.modeledCalls == 4 &&
            result.interCFG->connections().empty(),
        "runtime summaries do not add calls or activate callback bodies");
  check(std::all_of(result.calls.begin(), result.calls.end(),
                    [](const auto &call) {
                      return call.unresolved &&
                             !call.externalEffects.complete() &&
                             call.externalEffects.has(CallEffect::Callback);
                    }),
        "unmodeled callback effects remain explicit at the original sites");
  check(std::any_of(result.dataFlow->gaps().begin(),
                    result.dataFlow->gaps().end(),
                    [](const auto &gap) {
                      return gap.kind == wpgkit::DataGapKind::UnknownCall;
                    }),
        "DFG retains the unsummarized callback boundary");
  const auto repeated = solve(session);
  check(
      repeated.statistics == result.statistics &&
          repeated.interCFG == result.interCFG &&
          repeated.dataFlow == result.dataFlow,
      "unchanged summary application reuses the fixed point and graph snapshots");
}

void returnedAlias() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <stdlib.h>
    using Fn = void (*)();
    void target() {}
    int compare(const void *, const void *) { return 0; }
    void entry() {
      Fn slot = &target;
      auto found = static_cast<Fn *>(bsearch(&slot, &slot, 1, sizeof(Fn), &compare));
      if (found) (*found)();
    }
  )cpp");
  const auto result = solve(session);
  check(
      reached(result, "target") && !reached(result, "compare") &&
          result.calls.size() == 2 && !result.closedWithinInputs(),
      "search retains its base alias for ordinary PTA without expanding its comparator");
}

void readOnlyCallbacks() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <cstdlib>
    #include <cstring>
    #include <pthread.h>
    struct Entry { const char *name; };
    int compare(const void *a, const void *b) {
      return strcmp(static_cast<const Entry *>(a)->name,
                    static_cast<const Entry *>(b)->name);
    }
    void helper() {} void initialize() { helper(); }
    void entry() {
      Entry entries[2] = {{"a"}, {"b"}};
      qsort(entries, 2, sizeof(Entry), &compare);
      pthread_once_t once = PTHREAD_ONCE_INIT;
      pthread_once(&once, &initialize);
    }
  )cpp");
  const auto result = solve(session);
  check(
      result.calls.size() == 2 && result.reachable.size() == 1 &&
          result.statistics.unknownCalls == 0 && !reached(result, "compare") &&
          !reached(result, "helper"),
      "owned read-only callback effects close original sites without body activation");
  for (const auto &call : result.calls)
    check(!call.unresolved && call.externalEffects.complete() &&
              call.externalEffects.has(CallEffect::Callback),
          "a summarized callback remains an observable callback effect");
  for (const auto &summary : result.interCFG->summaries())
    check(summary.effects.complete(),
          "inter-CFG keeps the refined call-specific summary");
}

void callbackProofBoundaries() {
  for (
      const auto *definition :
      {"int compare(const void*,const void*) { return ++state; }",
       "int compare(const void* a,const void* b) { return compare(a,b); }",
       "extern int unavailable(); int compare(const void*,const void*) { return unavailable(); }"}) {
    CallGraphSession session;
    const auto code =
        std::string("#include <cstdlib>\nint state;\n") + definition +
        "\nvoid entry(){int values[2]; qsort(values,2,sizeof(int),&compare);}";
    add(session, code.c_str());
    const auto result = solve(session);
    check(
        result.calls.size() == 1 && result.calls.front().unresolved &&
            !reached(result, "compare"),
        "mutations, recursive proofs and missing helpers retain open callback effects");
  }
  CallGraphSession mixed;
  add(mixed, R"cpp(
    #include <cstdlib>
    int state;
    int clean(const void*,const void*) { return 0; }
    int changed(const void*,const void*) { return ++state; }
    void entry(bool choose) {
      int values[2]; auto compare = choose ? &clean : &changed;
      qsort(values, 2, sizeof(int), compare);
    }
  )cpp");
  const auto result = solve(mixed);
  check(result.calls.size() == 1 && result.calls.front().unresolved,
        "every candidate callback must satisfy the read-only proof");
}

void callbackTemporaries() {
  CallGraphSession local;
  add(local, R"cpp(
    #include <cstdlib>
    #include <functional>
    struct Local {
      Local() = default;
      int value(int x) const { return x; }
    };
    int compare(const void *a, const void *b) {
      return Local().value(std::less<int>()(*static_cast<const int *>(a),
                                            *static_cast<const int *>(b)));
    }
    void entry() { int v[2]; qsort(v, 2, sizeof(int), &compare); }
  )cpp");
  const auto result = solve(local);
  check(result.calls.size() == 1 && !result.calls.front().unresolved &&
            !reached(result, "compare"),
        "fresh comparator temporaries do not write caller-owned storage");
  for (const auto *definition :
       {"struct Empty {}; int compare(const void *a,const void *) { "
        "new (const_cast<void *>(a)) Empty; return 0; }",
        "int state; struct Local { Local() { ++state; } "
        "int get() { return 0; } }; "
        "int compare(const void *,const void *) { return Local().get(); }",
        "int change(int &x) { return ++x; } "
        "int compare(const void *a,const void *) { "
        "return change(*const_cast<int *>(static_cast<const int *>(a))); }"}) {
    CallGraphSession unsafe;
    const auto code =
        std::string("#include <cstdlib>\n#include <new>\n") + definition +
        "\nvoid entry(){int v[2]; qsort(v,2,sizeof(int),&compare);}";
    add(unsafe, code.c_str());
    const auto result = solve(unsafe);
    check(
        result.calls.size() == 1 && result.calls.front().unresolved &&
            !reached(result, "compare"),
        "placement construction, effectful temporaries and reference writes keep callbacks open");
  }
}

void arrayDeletion() {
  for (const bool replacement : {false, true}) {
    CallGraphSession session;
    add(session, R"cpp(
      #include <new>
      void entry(void *memory) { ::operator delete[](memory); }
    )cpp");
    check(solve(session, false).statistics.modeledCalls == 0,
          "delegation waits for later supplied definitions");
    if (replacement)
      add(session, R"cpp(
        #include <new>
        int changed;
        void operator delete(void *) noexcept { ++changed; }
      )cpp");
    const auto result = solve(session);
    check(
        result.calls.size() == 1 &&
            result.calls.front().unresolved == replacement,
        "array delete checks scalar replacement effects without activating its body");
    check(result.statistics.activatedBodies == 1,
          "model delegation does not add body reachability");
  }
}

void lateCallbackUncertainty() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <cstdlib>
    using Compare = int (*)(const void *, const void *);
    void replace(Compare *);
    int clean(const void *, const void *) { return 0; }
    void entry() {
      int values[2]; Compare compare = &clean;
      qsort(values, 2, sizeof(int), compare);
      replace(&compare);
    }
  )cpp");
  const auto result = solve(session);
  check(
      result.calls.size() == 2 &&
          std::all_of(result.calls.begin(), result.calls.end(),
                      [](const auto &call) { return call.unresolved; }),
      "later unknown writes revisit the summary for mutable callback storage");
  check(solve(session).statistics == result.statistics,
        "callback uncertainty converges without repeated fallback work");
}

void allocationResult() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    void entry() {
      auto storage = static_cast<void **>(::operator new(sizeof(void *)));
      *storage = nullptr;
    }
  )cpp");
  const auto result = solve(session);
  check(
      !result.closedWithinInputs() && result.calls.size() == 1 &&
          result.calls.front().externalEffects.has(CallEffect::Allocate) &&
          !result.hasLimitation("store through unknown address"),
      "open failure-handler effects do not erase the allocator's known result address");
}

bool unknownAddress(const CallGraphResult &result) {
  // Ignore the deliberately unknown call effects; check the subsequent store.
  for (const auto &gap : result.dataFlow->gaps()) {
    if (gap.kind != wpgkit::DataGapKind::UnknownAddress ||
        !gap.position.block.isValid())
      continue;
    const bool call =
        std::any_of(result.dataFlow->nodes().begin(),
                    result.dataFlow->nodes().end(), [&](const auto &node) {
                      return node.kind == wpgkit::DataNodeKind::CallResult &&
                             node.position == gap.position;
                    });
    if (!call)
      return true;
  }
  return false;
}

void allocationWrappers() {
  for (
      const auto *expression :
      {"::operator new[](sizeof(void *))",
       "::operator new(sizeof(void *), std::nothrow)",
       "::operator new[](sizeof(void *), std::nothrow)",
       "::operator new[](sizeof(void *), std::align_val_t(64))",
       "::operator new(sizeof(void *), std::align_val_t(64), std::nothrow)",
       "::operator new[](sizeof(void *), std::align_val_t(64), std::nothrow)"}) {
    CallGraphSession session;
    const auto code =
        std::string(
            "#include <new>\nvoid entry(){auto p=static_cast<void**>(") +
        expression + "); *p=nullptr;}";
    add(session, code.c_str());
    const auto result = solve(session);
    check(
        result.calls.size() == 1 && result.calls.front().unresolved &&
            result.statistics.activatedBodies == 1 && !unknownAddress(result),
        "default allocation wrapper chains preserve fresh normal results without body activation");
  }
  CallGraphSession replaced;
  add(replaced, R"cpp(
    #include <new>
    void entry() {
      auto p = static_cast<void **>(::operator new[](sizeof(void *), std::nothrow));
      *p = nullptr;
    }
  )cpp");
  add(replaced, R"cpp(
    #include <new>
    void *storage;
    void *operator new[](std::size_t) { return &storage; }
  )cpp");
  const auto result = solve(replaced);
  check(
      result.statistics.activatedBodies == 1 && unknownAddress(result),
      "a supplied intermediate allocation delegate prevents the default fresh-result proof");
}

void deletionWrappers() {
  const char *code = R"cpp(
    #include <new>
    void entry(void *p) {
      ::operator delete(p, sizeof(void *));
      ::operator delete[](p, sizeof(void *));
      ::operator delete[](p, std::align_val_t(64));
      ::operator delete(p, sizeof(void *), std::align_val_t(64));
      ::operator delete[](p, sizeof(void *), std::align_val_t(64));
      ::operator delete(p, std::nothrow);
      ::operator delete[](p, std::nothrow);
      ::operator delete(p, std::align_val_t(64), std::nothrow);
      ::operator delete[](p, std::align_val_t(64), std::nothrow);
    }
  )cpp";
  for (const bool replacement : {false, true}) {
    auto ast = clang::tooling::buildASTFromCodeWithArgs(
        code, {"-std=c++20", "-fsized-deallocation"}, "delete-wrappers.cpp");
    check(ast && !ast->getDiagnostics().hasErrorOccurred(),
          "parse sized deletes");
    CallGraphSession session;
    session.addUnit(ast->getASTContext());
    ast.reset();
    if (replacement)
      add(session, R"cpp(
        #include <new>
        int state;
        void operator delete[](void *) noexcept { ++state; }
        void operator delete[](void *, std::align_val_t) noexcept { ++state; }
      )cpp");
    const auto result = solve(session);
    check(
        result.calls.size() == 9 &&
            result.statistics.unknownCalls == (replacement ? 4u : 0u),
        "sized, aligned and nothrow deletion checks every replaced array delegate");
  }
}

void stringAllocation() {
  for (const bool replacement : {false, true}) {
    CallGraphSession session;
    add(session, R"cpp(
      #include <bits/c++config.h>
      #define private public
      #include <string>
      #undef private
      void entry(std::string &s) {
        std::size_t capacity = 16;
        auto p = reinterpret_cast<void **>(s._M_create(capacity, 0));
        *p = nullptr;
      }
    )cpp",
        "-std=c++17");
    if (replacement)
      add(session, R"cpp(
        #include <new>
        void *storage;
        void *operator new(std::size_t) { return &storage; }
      )cpp");
    const auto result = solve(session);
    check(
        result.calls.size() == 1 && result.calls.front().unresolved &&
            result.statistics.activatedBodies == 1 &&
            unknownAddress(result) == replacement,
        "string allocation preserves fresh results only with its default allocation delegate");
  }
}

void stringContracts() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <bits/c++config.h>
    #define private public
    #include <string>
    #undef private
    void entry(std::string &s, const char *text) {
      s._M_erase(0, 1);
      (void)s._M_append(text, 1);
      (void)s._M_replace(0, 1, text, 1);
      s._M_assign(s);
      s._M_mutate(0, 1, text, 1);
      s._M_construct(8, 'x');
      s.resize(32, 'x');
      s.reserve(64);
      s.reserve();
    }
  )cpp",
      "-std=c++17");
  const auto result = solve(session);
  unsigned erase = 0, self = 0, mutate = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto found = result.externalModels.find(target);
      if (found == result.externalModels.end())
        continue;
      if (found->second == ExternalModel::StringErase) {
        ++erase;
        check(
            !call.unresolved && !call.externalEffects.has(CallEffect::Callback),
            "standard string erasure has bounded writes without allocation callbacks");
      } else if (found->second == ExternalModel::StringMutate) {
        ++mutate;
        check(call.unresolved && call.externalEffects.has(CallEffect::Allocate),
              "string growth retains its allocation and exception boundary");
      } else if (found->second == ExternalModel::StringReturnSelf) {
        ++self;
        check(
            call.unresolved &&
                call.externalEffects.has(CallEffect::InputToReturn),
            "string return aliases coexist with open allocation and exception effects");
      }
    }
  check(erase == 1 && self == 2 && mutate == 6,
        "recognize exact standard string signatures");
}

void oldStringABI() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      "#include <string>\nvoid entry(std::string& s){ s.erase(0,1); }",
      {"-std=c++17", "-D_GLIBCXX_USE_CXX11_ABI=0"}, "cow-string.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse COW ABI");
  CallGraphSession session;
  session.addUnit(ast->getASTContext());
  ast.reset();
  const auto result = solve(session);
  for (const auto &[id, model] : result.externalModels)
    check(model != ExternalModel::StringErase &&
              model != ExternalModel::StringCreate &&
              model != ExternalModel::StringMutate,
          "the COW ABI must not inherit the modern string mutation contract");
  // Public mutation return aliases apply to both ABIs; they retain unknown
  // allocation/callback effects rather than claiming bounded COW writes.
  check(!ExternalModules::summary(ExternalModel::StringReturnSelf).complete(),
        "receiver return aliases do not suppress COW allocation effects");
}

void suppliedBody() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <stdlib.h>
    void replacement() {}
    extern "C" void qsort(void *, size_t, size_t,
                         int (*)(const void *, const void *)) { replacement(); }
    int ignored(const void *, const void *) { return 0; }
    void entry() {
      int values[2];
      qsort(values, 2, sizeof(int), &ignored);
    }
  )cpp");
  const auto result = solve(session);
  check(
      reached(result, "replacement") && !reached(result, "ignored") &&
          result.statistics.modeledCalls == 0 && result.closedWithinInputs(),
      "an explicitly supplied implementation retains precedence over runtime summaries");
}

void recognitionBoundaries() {
  for (const auto *code : {R"cpp(
    extern "C" void qsort(void *, unsigned long, unsigned long,
                         int (*)(const void *, const void *));
    int compare(const void *, const void *) { return 0; }
    void entry() { int data[2]; qsort(data, 2, sizeof(int), &compare); }
  )cpp",
                           R"cpp(
    # 1 "unsupported-system.h" 1 3
    extern "C" void qsort_r(void *, unsigned long, unsigned long, void *,
                           int (*)(void *, const void *, const void *));
    # 1 "user.cpp" 2
    int compare(void *, const void *, const void *) { return 0; }
    void entry() { int data[2]; qsort_r(data, 2, sizeof(int), data, &compare); }
  )cpp"}) {
    CallGraphSession session;
    add(session, code);
    const auto result = solve(session);
    check(
        !result.closedWithinInputs() && !reached(result, "compare"),
        "unsupported runtime declarations retain unknown effects without body expansion");
    for (const auto &[id, model] : result.externalModels)
      check(model != ExternalModel::Sort &&
                model != ExternalModel::SortWithContext,
            "a precise sorting contract requires its reviewed declaration");
  }
}

void contractEffects() {
  for (const auto model : {ExternalModel::Sort, ExternalModel::SortWithContext,
                           ExternalModel::Search, ExternalModel::Once}) {
    const auto effects = ExternalModules::summary(model);
    check(!effects.complete() && effects.has(CallEffect::Callback),
          "runtime effects alone do not establish the callback's effects");
  }
  check(ExternalModules::writtenArguments(ExternalModel::Sort) == 1 &&
            ExternalModules::writtenArguments(ExternalModel::SortWithContext) ==
                1 &&
            ExternalModules::writtenArguments(ExternalModel::Search) == 0 &&
            ExternalModules::returnedArgument(ExternalModel::Search) == 1,
        "API-owned array writes and search aliases remain distinct");
  check(!ExternalModules::summary(ExternalModel::Search)
             .has(CallEffect::WriteArguments),
        "search itself does not write its input array");
}

void alignedDeletion() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    void target() {}
    void entry() {
      auto cb = &target;
      ::operator delete(nullptr, std::align_val_t(64));
      cb();
    }
  )cpp");
  check(solve(session, false).statistics.modeledCalls == 0,
        "aligned delete waits for possible supplied replacements");
  const auto result = solve(session);
  check(result.closedWithinInputs() && result.statistics.unknownCalls == 0 &&
            reached(result, "target"),
        "default aligned scalar delete preserves unrelated pointer facts");
}

void handlerRegistration() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    void first() {} void second() {}
    void entry() {
      std::set_new_handler(&first);
      std::set_new_handler(&second);
      (void)std::get_new_handler();
    }
  )cpp");
  const auto result = solve(session);
  check(
      result.closedWithinInputs() && result.calls.size() == 3 &&
          !reached(result, "first") && !reached(result, "second"),
      "handler registration and query summarize shared state without invoking handlers");
  for (const auto &call : result.calls)
    check(call.externalEffects.has(CallEffect::ReadExternalState) &&
              !call.externalEffects.has(CallEffect::Callback),
          "registration itself has no callback effect");
  CallGraphSession queried;
  add(queried, R"cpp(
    #include <new>
    void handler() {}
    void entry() {
      std::set_new_handler(&handler);
      auto saved = std::get_new_handler();
      if (saved) saved();
    }
  )cpp");
  const auto invoked = solve(queried);
  check(
      reached(invoked, "handler"),
      "ordinary calls through queried handler aliases retain registered targets");
}

void terminationRegistration() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <cstdlib>
    #include <exception>
    #include <cxxabi.h>
    void handler() {} void status(int, void *) {} void cleanup(void *) {}
    void entry(void *context) {
      std::set_terminate(&handler);
      (void)std::get_terminate();
      atexit(&handler);
      at_quick_exit(&handler);
      on_exit(&status, context);
      __cxxabiv1::__cxa_atexit(&cleanup, context, nullptr);
    }
  )cpp");
  const auto result = solve(session);
  check(
      result.calls.size() == 6 && result.statistics.unknownCalls == 0 &&
          result.statistics.activatedBodies == 1,
      "termination registration stores callbacks without invoking or analyzing their bodies");
  for (const auto &call : result.calls)
    check(!call.externalEffects.has(CallEffect::Callback) &&
              !call.externalEffects.has(CallEffect::WriteArguments),
          "registration does not modify callback arguments");
  CallGraphSession isolated;
  add(isolated, R"cpp(
    #include <new>
    #include <exception>
    void allocator() {} void terminator() {}
    void entry() {
      std::set_new_handler(&allocator);
      std::set_terminate(&terminator);
      auto callback = std::get_terminate(); callback();
    }
  )cpp");
  const auto called = solve(isolated);
  check(
      reached(called, "terminator") && !reached(called, "allocator") &&
          !called.closedWithinInputs(),
      "separate handler registries retain registered aliases and unknown outside handlers");
}

void registeredContextEffects() {
  for (const bool invoke : {false, true}) {
    CallGraphSession session;
    const auto code = std::string(R"cpp(
      #include <cstdlib>
      void cleanup(int, void *) {}
      void runHandlers();
      void entry() {
        int value = 0; int *saved = &value;
        on_exit(&cleanup, &saved);
    )cpp") + (invoke ? "runHandlers();" : "") +
                      "*saved = 1; }";
    add(session, code.c_str());
    const auto result = solve(session);
    check(
        unknownAddress(result) == invoke && !reached(result, "cleanup"),
        "registration preserves a local context until unknown handler effects can modify it");
  }
}

void registrationBoundaries() {
  for (const auto *code : {R"cpp(
    extern "C" int atexit(void (*)());
    void cleanup() {}
    void entry() { atexit(&cleanup); }
  )cpp",
                           R"cpp(
    # 1 "changed-system.h" 1 3
    extern "C" int on_exit(void (*)(void *), void *);
    # 1 "user.cpp" 2
    void cleanup(void *) {}
    void entry() { on_exit(&cleanup, nullptr); }
  )cpp"}) {
    CallGraphSession session;
    add(session, code);
    const auto result = solve(session);
    check(result.calls.size() == 1 && result.calls.front().unresolved &&
              !reached(result, "cleanup"),
          "untrusted names and changed registration signatures stay open");
  }
  CallGraphSession exiting;
  add(exiting, R"cpp(
    #include <cstdlib>
    void cleanup() {}
    void entry() { atexit(&cleanup); exit(0); }
  )cpp");
  const auto result = solve(exiting);
  check(
      result.calls.size() == 2 && result.statistics.unknownCalls == 1 &&
          !reached(result, "cleanup"),
      "complete registration does not claim a complete exit-handler inventory");
  CallGraphSession supplied;
  add(supplied, R"cpp(
    #include <cstdlib>
    extern "C" int atexit(void (*fn)()) noexcept { fn(); return 0; }
    void cleanup() {}
    void entry() { atexit(&cleanup); }
  )cpp");
  check(reached(solve(supplied), "cleanup"),
        "a supplied implementation overrides the registration contract");
}

void mappingAliases() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <sys/mman.h>
    using Fn = void (*)();
    void target() {}
    void entry() {
      Fn slot = &target;
      void *memory = mmap(&slot, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (memory != MAP_FAILED) (*static_cast<Fn *>(memory))();
      mprotect(memory, 4096, PROT_READ);
      munmap(memory, 4096);
    }
  )cpp");
  const auto result = solve(session);
  check(reached(result, "target"),
        "mapped results retain possible address aliases");
  unsigned modeled = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto name = result.metadata.text(result.names.at(target));
      if (name == "mmap" || name == "mprotect" || name == "munmap") {
        ++modeled;
        check(
            !call.unresolved && call.externalEffects.complete(),
            "mapping contracts remain complete despite unknown mapped contents");
      }
    }
  check(modeled == 3, "all mapping variants were summarized");
  check(!ExternalModules::summary(ExternalModel::ProtectMemory)
             .has(CallEffect::WriteArguments),
        "changing page permissions does not overwrite pointer values");
}

void mappingSignature() {
  CallGraphSession session;
  add(session, R"cpp(
    # 1 "unsupported-system.h" 1 3
    extern "C" void *mmap(void **, unsigned long, int, int, int, long);
    # 1 "user.cpp" 2
    void entry() { void *p = nullptr; (void)mmap(&p, 1, 0, 0, -1, 0); }
  )cpp");
  const auto result = solve(session);
  check(!result.closedWithinInputs(),
        "changed mapping signatures retain the partial contract");
}

void nativeStateEffects() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <pthread.h>
    #include <signal.h>
    // The reviewed Linux normal-mutex initializer is all-zero storage. Avoid
    // an aggregate union initializer, which is a separate extractor gap.
    pthread_mutex_t mutex;
    pthread_rwlock_t rwlock;
    void target() {}
    void entry() {
      auto cb = &target;
      pthread_mutex_lock(&mutex); pthread_mutex_unlock(&mutex);
      pthread_rwlock_rdlock(&rwlock); pthread_rwlock_unlock(&rwlock);
      pthread_rwlock_wrlock(&rwlock); pthread_rwlock_unlock(&rwlock);
      stack_t old;
      sigaltstack(nullptr, &old);
      cb();
    }
  )cpp");
  const auto result = solve(session);
  check(
      result.closedWithinInputs() && reached(result, "target") &&
          result.statistics.unknownCalls == 0,
      "locks and alternate-stack state do not invent callbacks or arbitrary global writes");
  check(ExternalModules::writtenArguments(ExternalModel::SignalStack) == 2,
        "alternate-stack registration writes only the previous-state output");
}

void nativeStateSignatures() {
  CallGraphSession session;
  add(session, R"cpp(
    # 1 "unsupported-system.h" 1 3
    typedef struct { long state; } pthread_rwlock_t;
    extern "C" int pthread_rwlock_rdlock(const pthread_rwlock_t *);
    extern "C" long pthread_rwlock_wrlock(pthread_rwlock_t *);
    # 1 "user.cpp" 2
    void entry(pthread_rwlock_t *lock) {
      pthread_rwlock_rdlock(lock);
      pthread_rwlock_wrlock(lock);
    }
  )cpp");
  const auto result = solve(session);
  check(result.calls.size() == 2 &&
            std::all_of(result.calls.begin(), result.calls.end(),
                        [](const auto &call) { return call.unresolved; }),
        "unsupported lock constness and return types retain unknown effects");
}
} // namespace

int main() {
  contractEffects();
  summaryBoundary();
  returnedAlias();
  readOnlyCallbacks();
  callbackProofBoundaries();
  callbackTemporaries();
  arrayDeletion();
  lateCallbackUncertainty();
  allocationResult();
  allocationWrappers();
  deletionWrappers();
  stringAllocation();
  stringContracts();
  oldStringABI();
  suppliedBody();
  recognitionBoundaries();
  alignedDeletion();
  handlerRegistration();
  terminationRegistration();
  registrationBoundaries();
  registeredContextEffects();
  mappingAliases();
  mappingSignature();
  nativeStateEffects();
  nativeStateSignatures();
}
