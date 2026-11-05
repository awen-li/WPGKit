#include "CallGraphPTA.h"
#include "DataFlowGraph.h"
#include "ExternalModules.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class Catalog final : public clang::RecursiveASTVisitor<Catalog> {
public:
  bool VisitFunctionDecl(clang::FunctionDecl *decl) {
    const auto name = decl->getQualifiedNameAsString();
    const auto model = ExternalModules{}.query(decl).model;
    if (model != ExternalModel::Unknown || !models.contains(name))
      models[name] = model;
    return true;
  }

  std::map<std::string, ExternalModel> models;
};

void add(CallGraphSession &session, const char *code, const char *file,
         const char *language = "-std=c++20", const char *option = nullptr) {
  std::vector<std::string> arguments{language};
  if (option)
    arguments.emplace_back(option);
  auto ast = clang::tooling::buildASTFromCodeWithArgs(code, arguments, file);
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse model fixture");
  session.addUnit(ast->getASTContext());
}

CallGraphResult solve(CallGraphSession &session, bool final = true) {
  auto result =
      final ? session.finalize({"entry"}) : session.analyze({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  return std::move(*result);
}

bool complete(const CallGraphResult &result, const char *name,
              CallEffect effect) {
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.metadata.text(result.names.at(target)) == name)
        return !call.unresolved && call.externalEffects.complete() &&
               call.externalEffects.has(effect);
  return false;
}

bool reached(const CallGraphResult &result, const char *name) {
  for (const auto id : result.reachable)
    if (result.metadata.text(result.names.at(id)) == name)
      return true;
  return false;
}

void recognition() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
    #include <bits/stl_tree.h>
    #include <system_error>
    struct Scalar { unsigned long value; Scalar() = default; };
    struct Nested : Scalar { double a[2]; Nested() = default; };
    struct Pointer { void *p; Pointer() = default; };
    struct PointerBase : Pointer { PointerBase() = default; };
    struct PointerMember { Pointer p; PointerMember() = default; };
    struct MemberPointer { int Scalar::*p; MemberPointer() = default; };
    struct Initialized { int i = 7; Initialized() = default; };
    struct Supplied { int i; Supplied() : i(1) {} };
    struct Copy { int i; Copy(const Copy &) = default; };
    namespace user {
      void *_Rb_tree_increment(void *);
      const std::error_category &generic_category();
    }
    void entry() {
      Scalar a; Nested b; Pointer c; PointerBase d; PointerMember e;
      MemberPointer f; Initialized g; Supplied h;
    }
  )cpp",
      {"-std=c++20", "-fno-builtin"}, "catalog.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse real declarations");
  Catalog catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  using M = ExternalModel;
  for (const auto *name : {"Scalar::Scalar", "Nested::Nested"})
    check(catalog.models.at(name) == M::TrivialScalarConstructor,
          "scalar default construction is recognized from Clang properties");
  for (const auto *name :
       {"Pointer::Pointer", "PointerBase::PointerBase",
        "PointerMember::PointerMember", "MemberPointer::MemberPointer"})
    check(
        catalog.models.at(name) == M::TrivialDefaultConstructor,
        "trivial pointer-bearing default construction introduces no non-null target");
  for (const auto *name :
       {"Initialized::Initialized", "Supplied::Supplied", "Copy::Copy",
        "user::_Rb_tree_increment", "user::generic_category"})
    check(catalog.models.at(name) == M::Unknown,
          "initializers, copies and user names require their actual behavior");
  for (const auto *name :
       {"std::_Rb_tree_increment", "std::_Rb_tree_decrement"})
    check(catalog.models.at(name) == M::FindMemory,
          "tree traversal retains unknown reachable-node returns");
  for (const auto *name : {"std::_Rb_tree_insert_and_rebalance",
                           "std::_Rb_tree_rebalance_for_erase"})
    check(catalog.models.at(name) == M::PointerStructure,
          "tree relinking has an argument-memory contract");
  for (const auto *name : {"std::generic_category", "std::system_category"})
    check(catalog.models.at(name) == M::ExternalPointer,
          "category access returns external storage without invoking methods");
}

void scalarAndFenceFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    struct Code { unsigned long value; Code() = default; };
    void callback() {}
    void entry() {
      auto cb = &callback;
      Code uninitialized; Code zeroed{};
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      __atomic_signal_fence(__ATOMIC_ACQUIRE);
      cb();
    }
  )cpp",
      "scalars.cpp");
  const auto result = solve(session);
  check(
      complete(result, "Code::Code", CallEffect::BeginLifetime) &&
          complete(result, "Code::Code", CallEffect::WriteArguments),
      "default/value construction retains lifetime and possible scalar writes");
  check(complete(result, "__atomic_thread_fence", CallEffect::Synchronize) &&
            complete(result, "__atomic_signal_fence", CallEffect::Synchronize),
        "fences retain ordering effects without inventing callbacks");
  check(
      result.statistics.unknownCalls == 0 && reached(result, "callback"),
      "scalar construction and fences do not havoc independent pointer storage");
  check(result.dataFlow &&
            std::any_of(result.dataFlow->gaps().begin(),
                        result.dataFlow->gaps().end(),
                        [](const auto &gap) {
                          return gap.kind == wpgkit::DataGapKind::ExternalState;
                        }),
        "fence effects remain a DFG resource gap, not a concurrency proof");
  check(std::none_of(result.dataFlow->gaps().begin(),
                     result.dataFlow->gaps().end(),
                     [](const auto &gap) {
                       using K = wpgkit::DataGapKind;
                       return gap.kind == K::UnknownCall ||
                              gap.kind == K::ArgumentMismatch;
                     }),
        "owned DFG call bindings support the constructor and fence summaries");
}

void treeFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <bits/stl_tree.h>
    #include <system_error>
    void callback() {}
    void entry() {
      std::_Rb_tree_node_base node{}, header{};
      auto cb = &callback;
      (void)std::_Rb_tree_increment(&node);
      (void)std::_Rb_tree_decrement(&node);
      std::_Rb_tree_insert_and_rebalance(true, &node, &header, header);
      (void)std::_Rb_tree_rebalance_for_erase(&node, header);
      (void)std::generic_category(); (void)std::system_category();
      cb();
    }
  )cpp",
      "trees.cpp");
  const auto result = solve(session);
  check(
      complete(result, "std::_Rb_tree_increment", CallEffect::ReadArguments) &&
          complete(result, "std::_Rb_tree_decrement",
                   CallEffect::ReadArguments),
      "tree traversal is read-only");
  check(complete(result, "std::_Rb_tree_insert_and_rebalance",
                 CallEffect::WriteArguments) &&
            complete(result, "std::_Rb_tree_rebalance_for_erase",
                     CallEffect::InputToMemory),
        "relinking retains reads, writes and input-to-memory flow");
  check(complete(result, "std::generic_category", CallEffect::ReadGlobals) &&
            complete(result, "std::system_category", CallEffect::ReadGlobals) &&
            reached(result, "callback"),
        "static category storage and independent callback are preserved");
}

void runtimePrecedence() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <bits/stl_tree.h>
    void entry() {
      std::_Rb_tree_node_base node{};
      (void)std::_Rb_tree_increment(&node);
    }
  )cpp",
      "caller.cpp");
  check(solve(session, false).statistics.modeledCalls == 0,
        "tree runtime summaries wait until imports finish");
  add(session, R"cpp(
    #include <bits/stl_tree.h>
    void supplied_effect() {}
    namespace std {
      _Rb_tree_node_base *_Rb_tree_increment(_Rb_tree_node_base *p) throw() {
        supplied_effect(); return p;
      }
    }
  )cpp",
      "definition.cpp");
  const auto result = solve(session);
  check(reached(result, "supplied_effect"),
        "late runtime body overrides summary");
}

void unknownTreeReturn() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <bits/stl_tree.h>
    struct Node : std::_Rb_tree_node_base { void (*callback)(); };
    void callback() {}
    void entry() {
      Node node{}; node.callback = &callback;
      auto *next = static_cast<Node *>(std::_Rb_tree_increment(&node));
      next->callback();
    }
  )cpp",
      "unknown-return.cpp");
  const auto result = solve(session);
  check(
      complete(result, "std::_Rb_tree_increment", CallEffect::ReadArguments) &&
          result.statistics.unknownCalls > 0,
      "complete traversal effects do not invent a closed set of node targets");
}

void numericConversionFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <cstdlib>
    struct End { char *value; };
    struct Dispatch { void (*callback)(); };
    void callback() {}
    void entry() {
      End end; Dispatch dispatch;
      dispatch.callback = &callback;
      (void)strtol("123tail", &end.value, 10);
      (void)strtod("1.5tail", &end.value);
      dispatch.callback();
    }
  )cpp",
      "conversion.cpp");
  const auto result = solve(session);
  check(complete(result, "strtol", CallEffect::WriteArguments) &&
            complete(result, "strtod", CallEffect::WriteArguments),
        "numeric conversion retains its end-pointer write");
  check(result.statistics.unknownCalls == 0 && reached(result, "callback"),
        "end-pointer stores do not clobber an unrelated callback field");
}

void scalarRehashFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <initializer_list>
    #include <tuple>
    #include <ext/aligned_buffer.h>
    #include <bits/functional_hash.h>
    #include <bits/hashtable_policy.h>
    void entry(std::__detail::_Prime_rehash_policy &policy) {
      (void)policy._M_next_bkt(11);
      (void)policy._M_need_rehash(11, 10, 2);
    }
  )cpp",
      "rehash.cpp", "-std=c++17");
  const auto result = solve(session);
  for (const auto *name :
       {"std::__detail::_Prime_rehash_policy::_M_next_bkt",
        "std::__detail::_Prime_rehash_policy::_M_need_rehash"})
    check(
        complete(result, name, CallEffect::WriteArguments) &&
            complete(result, name, CallEffect::FloatingEnvironment),
        "rehash policy preserves scalar receiver writes and floating effects");
  check(ExternalModules::writtenArguments(ExternalModel::ScalarRehashPolicy) ==
            0,
        "scalar-only rehash storage needs no pointer clobbering");
}

void stringSearchFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <string>
    void entry(const std::string &text) {
      (void)text.find('x', 0); (void)text.find("abc", 0, 3);
      (void)text.rfind('x', 0); (void)text.rfind("abc", 0, 3);
      (void)text.find_first_of("ab", 0, 2);
      (void)text.find_last_of("ab", 0, 2);
      (void)text.find_first_not_of("ab", 0, 2);
      (void)text.find_last_not_of('a', 0);
    }
  )cpp",
      "string-search.cpp", "-std=c++17");
  const auto result = solve(session);
  unsigned searches = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto name = result.metadata.text(result.names.at(target));
      if (name.find("std::basic_string<char>::") != 0)
        continue;
      ++searches;
      check(!call.unresolved &&
                call.externalEffects.has(CallEffect::ReadArguments) &&
                !call.externalEffects.has(CallEffect::WriteArguments) &&
                !call.externalEffects.has(CallEffect::Callback),
            "standard character searches only read their inputs");
    }
  check(searches == 8, "all eight system search overloads are summarized");
}

void isolatedRuntimeFlow() {
  // Declaration-only fixture isolates the reviewed contracts from unrelated
  // global-initializer gaps in the full system-header inventory.
  CallGraphSession session;
  add(session, R"cpp(
    # 1 "reviewed-runtime.h" 1 3
    namespace std {
      template<class> struct char_traits;
      template<class> struct allocator;
      template<class C, class T = char_traits<C>, class A = allocator<C>>
      struct basic_string { unsigned long find(C, unsigned long) const; };
      namespace __detail {
        struct _Prime_rehash_policy {
          float factor; mutable unsigned long threshold;
          unsigned long _M_next_bkt(unsigned long) const;
        };
      }
    }
    # 1 "runtime-user.cpp" 2
    struct Dispatch { void (*callback)(); };
    void callback() {}
    void entry(const std::basic_string<char> &text,
               std::__detail::_Prime_rehash_policy &policy) {
      Dispatch dispatch; dispatch.callback = &callback;
      (void)text.find('x', 0);
      (void)policy._M_next_bkt(11);
      dispatch.callback();
    }
  )cpp",
      "runtime-user.cpp");
  const auto result = solve(session);
  check(result.statistics.unknownCalls == 0 && reached(result, "callback"),
        "read-only searches and scalar policy writes preserve callback fields");
}

void customStringTraits() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <string>
    void custom_effect() {}
    struct Traits : std::char_traits<char> {
      static const char *find(const char *s, std::size_t, const char &) {
        custom_effect(); return s;
      }
    };
    void entry(const std::basic_string<char, Traits> &text) {
      (void)text.find('x', 0);
    }
  )cpp",
      "custom-traits.cpp", "-std=c++17");
  const auto result = solve(session);
  check(reached(result, "custom_effect"),
        "custom string traits must retain their user-defined calls");
}

void unavailableCustomTraits() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <string>
    struct Traits : std::char_traits<char> {};
    extern template class std::basic_string<char, Traits>;
    void entry(const std::basic_string<char, Traits> &text) {
      (void)text.find('x', 0);
    }
  )cpp",
      "external-traits.cpp", "-std=c++17");
  const auto result = solve(session);
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.metadata.text(result.names.at(target)) ==
          "std::basic_string<char, Traits>::find") {
        check(
            call.unresolved && !call.externalEffects.complete(),
            "unavailable custom traits must not receive a read-only contract");
        return;
      }
  check(false, "find call with unavailable custom-traits implementation");
}

void pointerBearingPolicy() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(R"cpp(
    # 1 "changed-runtime.h" 1 3
    namespace std::__detail {
      struct _Prime_rehash_policy {
        virtual void hook();
        unsigned long _M_next_bkt(unsigned long) const;
      };
    }
  )cpp",
                                                      {"-std=c++20"},
                                                      "policy-layout.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse changed policy");
  Catalog catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  const auto model =
      catalog.models.at("std::__detail::_Prime_rehash_policy::_M_next_bkt");
  check(!ExternalModules::summary(model).complete(),
        "a changed policy with a vptr cannot use the scalar-only contract");
}

void byteFlagFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    void callback() {}
    void entry() {
      bool flag = false;
      auto cb = &callback;
      (void)__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE);
      __atomic_clear(&flag, __ATOMIC_RELEASE);
      cb();
    }
  )cpp",
      "atomic-flags.cpp");
  check(solve(session, false).statistics.modeledCalls == 0,
        "byte-write models wait for the complete field inventory");
  add(session, "struct Later { void (*callback)(); };", "late-flags.cpp");
  const auto result = solve(session);
  check(complete(result, "__atomic_test_and_set", CallEffect::ReadArguments) &&
            complete(result, "__atomic_test_and_set",
                     CallEffect::WriteArguments) &&
            complete(result, "__atomic_clear", CallEffect::WriteArguments),
        "byte flags retain atomic reads and writes");
  check(result.statistics.unknownCalls == 0 && reached(result, "callback"),
        "atomic byte flags do not invent unknown callbacks");
  check(ExternalModules::summary(ExternalModel::AtomicClear)
                .has(CallEffect::Synchronize) &&
            !ExternalModules::summary(ExternalModel::AtomicClear)
                 .has(CallEffect::ReadArguments),
        "atomic clear retains synchronization without a read dependency");
}

void byteWritesRemainUnknown() {
  CallGraphSession session;
  add(session, R"cpp(
    void callback() {}
    void entry() {
      auto cb = &callback;
      (void)__atomic_test_and_set(static_cast<void *>(&cb), __ATOMIC_RELAXED);
      cb();
    }
  )cpp",
      "partial-pointer-write.cpp");
  const auto result = solve(session);
  check(complete(result, "__atomic_test_and_set", CallEffect::WriteArguments) &&
            result.statistics.unknownCalls > 0,
        "a byte write to pointer storage must not leave a closed target set");
}

void immediateExitFlow() {
  for (const char *name : {"_exit", "_Exit"}) {
    CallGraphSession session;
    const auto code = std::string("#include <unistd.h>\n#include <cstdlib>\n") +
                      "void entry() { " + name + "(1); }";
    add(session, code.c_str(), "immediate-exit.cpp");
    const auto result = solve(session);
    check(complete(result, name, CallEffect::NoReturn) &&
              complete(result, name, CallEffect::WriteExternalState) &&
              result.statistics.unknownCalls == 0,
          "immediate exit has no userspace termination callbacks");
  }
  check(!ExternalModules::summary(ExternalModel::TerminateRuntime).complete(),
        "exit/abort/exception-handler paths still require callback coverage");
}

void scalarMathOutput() {
  CallGraphSession session;
  add(session, R"cpp(
    extern "C" double modf(double, double *);
    extern "C" double frexp(double, int *);
    struct Dispatch { void (*callback)(); };
    void callback() {}
    void entry() {
      Dispatch dispatch; dispatch.callback = &callback;
      double integerPart; int exponent;
      (void)modf(1.5, &integerPart);
      (void)frexp(8.0, &exponent);
      dispatch.callback();
    }
  )cpp",
      "math-output.cpp");
  const auto result = solve(session);
  check(complete(result, "modf", CallEffect::WriteArguments) &&
            complete(result, "frexp", CallEffect::FloatingEnvironment) &&
            result.statistics.unknownCalls == 0 && reached(result, "callback"),
        "typed scalar math outputs preserve unrelated callback fields");
}

void tokenizerFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <string.h>
    char *entry(char *buffer) {
      char *saved = nullptr;
      (void)strtok_r(buffer, ":", &saved);
      return strtok_r(nullptr, ":", &saved);
    }
  )cpp",
      "tokens.cpp");
  const auto result = solve(session);
  unsigned calls = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.metadata.text(result.names.at(target)) == "strtok_r") {
        ++calls;
        check(!call.unresolved && call.externalEffects.complete() &&
                  call.externalEffects.has(CallEffect::WriteArguments) &&
                  !call.externalEffects.has(CallEffect::WriteGlobals) &&
                  !call.externalEffects.has(CallEffect::Callback),
              "tokenizer effects exclude hidden globals and callbacks");
      }
  check(calls == 2, "model both initial and saved-cursor tokenization");
  CallGraphSession interior;
  add(interior, R"cpp(
    #include <string.h>
    void entry(char *buffer) {
      char *saved = nullptr;
      auto token = strtok_r(buffer, ":", &saved);
      if (token) (*reinterpret_cast<void (**)()>(token))();
    }
  )cpp",
      "token-interior.cpp");
  check(solve(interior).statistics.unknownCalls > 0,
        "an interior token pointer cannot establish a closed callback target");
}

void reviewedRuntimeDeclarations() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
    #include <condition_variable>
    #include <bits/atomic_futex.h>
    #include <math.h>
    #include <sys/stat.h>
    #include <sys/resource.h>
    #include <time.h>
    #include <string.h>
  )cpp",
      {"-std=c++20"}, "runtime-declarations.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse native runtime declarations and layouts");
  Catalog catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  using M = ExternalModel;
  for (const auto *name :
       {"modf", "modff", "modfl", "frexp", "frexpf", "frexpl"})
    check(
        catalog.models.at(name) == M::MathOutput,
        "math output signatures retain their floating and integer output types");
  for (const auto *name :
       {"stat", "fstat", "lstat", "getrlimit", "getrusage", "clock_gettime"})
    check(catalog.models.at(name) == M::ScalarSystemOutput,
          "native scalar output layouts exclude pointer storage");
  for (const auto *name : {"std::condition_variable::notify_one",
                           "std::condition_variable::notify_all",
                           "pthread_cond_signal", "pthread_cond_broadcast"})
    check(catalog.models.at(name) == M::ConditionNotify,
          "native condition notifications retain synchronization contracts");
  check(catalog.models.at("pthread_cond_init") == M::ConditionInitialize &&
            catalog.models.at("pthread_cond_destroy") == M::ConditionDestroy &&
            catalog.models.at(
                "std::__atomic_futex_unsigned_base::_M_futex_notify_all") ==
                M::FutexWake,
        "native condition lifecycle and futex wake contracts are recognized");
  check(!ExternalModules::summary(catalog.models.at("pthread_cond_wait"))
             .complete(),
        "condition wait remains partial");
  check(catalog.models.at("strtok_r") == M::Tokenize,
        "restricted tokenizer parameters are accepted");
}

void synchronizationFlow() {
  CallGraphSession session;
  add(session, R"cpp(
    # 1 "reviewed-runtime.h" 1 3
    namespace std {
      struct condition_variable {
        unsigned state[12];
        condition_variable() noexcept;
        ~condition_variable() noexcept;
        void notify_one() noexcept;
        void notify_all() noexcept;
      };
      struct __atomic_futex_unsigned_base {
        static void _M_futex_notify_all(unsigned *);
      };
    }
    # 1 "synchronization.cpp" 2
    void callback() {}
    void entry() {
      std::condition_variable condition;
      struct Dispatch { void (*cb)(); } s;
      s.cb = &callback;
      condition.notify_one();
      condition.notify_all();
      std::__atomic_futex_unsigned_base::_M_futex_notify_all(condition.state);
      s.cb();
    }
  )cpp",
      "synchronization.cpp");
  const auto result = solve(session);
  check(complete(result, "std::condition_variable::condition_variable",
                 CallEffect::BeginLifetime) &&
            complete(result, "std::condition_variable::~condition_variable",
                     CallEffect::EndLifetime),
        "condition lifetime calls remain supported after AST release");
  for (const auto *name :
       {"std::condition_variable::notify_one",
        "std::condition_variable::notify_all",
        "std::__atomic_futex_unsigned_base::_M_futex_notify_all"})
    check(complete(result, name, CallEffect::Synchronize),
          "notification retains ordering without invented callbacks");
  check(result.statistics.unknownCalls == 0 && reached(result, "callback"),
        "scalar condition state does not invalidate unrelated pointer fields");
  check(result.dataFlow &&
            std::any_of(result.dataFlow->gaps().begin(),
                        result.dataFlow->gaps().end(),
                        [](const auto &gap) {
                          return gap.kind == wpgkit::DataGapKind::ExternalState;
                        }),
        "thread interaction remains an external-state DFG gap");
}

void scalarSystemFlow() {
  CallGraphSession session;
  add(session, R"c(
    #include <sys/stat.h>
    #include <sys/resource.h>
    #include <time.h>
    void callback(void) {}
    void entry(void) {
      struct State { struct stat status; void (*cb)(void); } s;
      struct rlimit limit;
      struct rusage usage;
      struct timespec time;
      s.cb = &callback;
      stat("input", &s.status); lstat("input", &s.status); fstat(0, &s.status);
      getrlimit(0, &limit); getrusage(0, &usage);
      clock_gettime(0, &time);
      s.cb();
    }
  )c",
      "scalar-system.c", "-std=gnu17");
  const auto result = solve(session);
  for (const auto *name :
       {"stat", "lstat", "fstat", "getrlimit", "getrusage", "clock_gettime"})
    check(complete(result, name, CallEffect::WriteArguments) &&
              complete(result, name, CallEffect::ReadExternalState),
          "C record output retains writes and system dependencies");
  check(result.statistics.unknownCalls == 0 && reached(result, "callback"),
        "scalar system outputs do not clobber neighboring callback fields");
}

void incompatibleRuntimeLayouts() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
    # 1 "different-runtime.h" 1 3
    struct stat { void *storage; };
    extern "C" int stat(const char *, struct stat *);
    struct Condition { void *storage; };
    extern "C" int pthread_cond_signal(Condition *);
    extern "C" char *strtok_r(char *, const char *, int *);
    extern "C" double modf(double, void **);
    namespace std {
      struct condition_variable { void *storage; void notify_all(); };
    }
  )cpp",
      {"-std=c++20", "-fno-builtin"}, "different-runtime.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse incompatible system declarations");
  Catalog catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  using M = ExternalModel;
  check(
      catalog.models.at("stat") == M::Buffer1 &&
          catalog.models.at("pthread_cond_signal") == M::ThreadRuntime &&
          catalog.models.at("std::condition_variable::notify_all") ==
              M::ThreadRuntime &&
          catalog.models.at("strtok_r") == M::Unknown &&
          catalog.models.at("modf") == M::Unknown,
      "pointer-bearing layouts and incompatible signatures keep fallback models");
}

void suppliedTokenizerBody() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <string.h>
    void entry(char *input) {
      char *saved = nullptr;
      (void)strtok_r(input, ":", &saved);
    }
  )cpp",
      "token-caller.cpp");
  check(solve(session, false).statistics.modeledCalls == 0,
        "tokenizer waits for the complete input inventory");
  add(session, R"cpp(
    #include <string.h>
    void supplied_effect() {}
    extern "C" char *strtok_r(char *input, const char *, char **saved) noexcept {
      supplied_effect(); *saved = input; return input;
    }
  )cpp",
      "token-body.cpp");
  check(reached(solve(session), "supplied_effect"),
        "supplied tokenizer body overrides the reviewed runtime contract");
}

void defaultScalarDelete() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    void callback() {}
    void entry(void *storage) {
      struct Dispatch { void (*cb)(); } dispatch{&callback};
      ::operator delete(storage);
      __builtin_operator_delete(nullptr);
      dispatch.cb();
    }
  )cpp",
      "default-delete.cpp");
  check(solve(session, false).statistics.modeledCalls == 0,
        "default delete contract waits for replacement definitions");
  const auto result = solve(session);
  check(
      complete(result, "operator delete", CallEffect::EndLifetime) &&
          result.statistics.unknownCalls == 0 && reached(result, "callback"),
      "default scalar delete ends storage lifetime without arbitrary pointer writes");

  CallGraphSession array;
  add(array, R"cpp(
    #include <new>
    void entry(void *storage) { ::operator delete[](storage); }
  )cpp",
      "array-delete.cpp");
  check(solve(array).statistics.unknownCalls == 0,
        "default array delete uses the reviewed default scalar delegate");
}

void suppliedScalarDelete() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    void entry(void *storage) { ::operator delete(storage); }
  )cpp",
      "delete-caller.cpp");
  (void)solve(session, false);
  add(session, R"cpp(
    #include <new>
    void replacement_effect() {}
    void operator delete(void *) noexcept { replacement_effect(); }
  )cpp",
      "delete-replacement.cpp");
  const auto result = solve(session);
  check(reached(result, "replacement_effect") &&
            result.statistics.modeledCalls == 0,
        "supplied scalar delete body overrides the default lifetime contract");
}

void builtinAllocationReplacements() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    using Fn = void (*)();
    void entry() {
      auto slot = static_cast<Fn *>(__builtin_operator_new(sizeof(Fn)));
      (*slot)();
      __builtin_operator_delete(slot);
    }
  )cpp",
      "builtin-allocation.cpp");
  check(solve(session, false).statistics.modeledCalls == 0,
        "allocation builtins wait for replaceable operator bodies");
  add(session, R"cpp(
    #include <new>
    void callback() {}
    void allocation_effect() {}
    void deletion_effect() {}
    void (*stored)() = &callback;
    void *operator new(__SIZE_TYPE__) { allocation_effect(); return &stored; }
    void operator delete(void *) noexcept { deletion_effect(); }
  )cpp",
      "builtin-replacements.cpp");
  const auto result = solve(session);
  check(
      reached(result, "callback") && reached(result, "allocation_effect") &&
          reached(result, "deletion_effect") &&
          result.statistics.unknownCalls == 0,
      "allocation builtins use supplied operators and preserve returned pointer flow");
  check(solve(session).statistics == result.statistics,
        "resolved builtin operators converge without repeating work");
}

void builtinAllocationOverloads() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <new>
    void unselected() {}
    void aligned_allocation() {}
    void aligned_deletion() {}
    void sized_deletion() {}
    alignas(64) unsigned char storage[64];
    void *operator new(__SIZE_TYPE__) { unselected(); return storage; }
    void *operator new(__SIZE_TYPE__, std::align_val_t) {
      aligned_allocation(); return storage;
    }
    void operator delete(void *) noexcept { unselected(); }
    void operator delete(void *, std::align_val_t) noexcept { aligned_deletion(); }
    void operator delete(void *, __SIZE_TYPE__, std::align_val_t) noexcept {
      sized_deletion();
    }
    void entry() {
      auto p = __builtin_operator_new(8, std::align_val_t(64));
      __builtin_operator_delete(p, std::align_val_t(64));
      __builtin_operator_delete(nullptr, static_cast<__SIZE_TYPE__>(8), std::align_val_t(64));
    }
  )cpp",
      "builtin-overloads.cpp", "-std=c++20", "-fsized-deallocation");
  const auto result = solve(session);
  check(reached(result, "aligned_allocation") &&
            reached(result, "aligned_deletion") &&
            reached(result, "sized_deletion") &&
            !reached(result, "unselected") &&
            result.statistics.unknownCalls == 0,
        "builtin overloads use Clang's selected alignment and size signatures");
  check(result.dataFlow && std::none_of(result.dataFlow->gaps().begin(),
                                        result.dataFlow->gaps().end(),
                                        [](const auto &gap) {
                                          using K = wpgkit::DataGapKind;
                                          return gap.kind ==
                                                     K::ArgumentMismatch ||
                                                 gap.kind == K::UnmappedCall ||
                                                 gap.kind == K::UnknownCall;
                                        }),
        "resolved builtins preserve existing CFG and DFG call bindings");
}
} // namespace

int main() {
  recognition();
  scalarAndFenceFlow();
  treeFlow();
  runtimePrecedence();
  unknownTreeReturn();
  numericConversionFlow();
  scalarRehashFlow();
  stringSearchFlow();
  isolatedRuntimeFlow();
  customStringTraits();
  unavailableCustomTraits();
  pointerBearingPolicy();
  byteFlagFlow();
  byteWritesRemainUnknown();
  immediateExitFlow();
  scalarMathOutput();
  tokenizerFlow();
  reviewedRuntimeDeclarations();
  synchronizationFlow();
  scalarSystemFlow();
  incompatibleRuntimeLayouts();
  suppliedTokenizerBody();
  defaultScalarDelete();
  suppliedScalarDelete();
  builtinAllocationReplacements();
  builtinAllocationOverloads();
}
