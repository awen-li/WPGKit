#include "ExternalModules.h"
#include "CallGraphPTA.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class Declarations final : public clang::RecursiveASTVisitor<Declarations> {
public:
  bool shouldVisitTemplateInstantiations() const {
    return true;
  }

  bool VisitFunctionDecl(clang::FunctionDecl *decl) {
    const auto name = decl->getQualifiedNameAsString();
    const auto model = ExternalModules{}.query(decl).model;
    if (model != ExternalModel::Unknown || !models.contains(name))
      models[name] = model;
    return true;
  }

  std::map<std::string, ExternalModel> models;
};

void add(CallGraphSession &session, const char *code,
         const char *file = "model.cpp") {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"}, file);
  check(ast != nullptr, "parse model integration fixture");
  session.addUnit(ast->getASTContext());
} // Release the AST before solving.

CallGraphResult solve(CallGraphSession &session, bool final = true) {
  auto result =
      final ? session.finalize({"entry"}) : session.analyze({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  return std::move(*result);
}

bool edge(const CallGraphResult &result, const char *from, const char *to) {
  for (const auto &site : result.calls)
    if (result.metadata.text(result.names.at(site.caller)) == from)
      for (const auto target : site.targets)
        if (result.metadata.text(result.names.at(target)) == to)
          return true;
  return false;
}

bool completeCall(const CallGraphResult &result, const char *name,
                  CallEffect effect) {
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.metadata.text(result.names.at(target)) == name)
        return !call.unresolved && call.externalEffects.complete() &&
               call.externalEffects.has(effect);
  return false;
}

void builtinIntrinsics() {
  CallGraphSession builtin;
  add(builtin, R"cpp(
    using Fn = void (*)();
    void callback() {}
    void entry() {
      Fn p = &callback;
      auto aligned = static_cast<Fn *>(__builtin_assume_aligned(&p, alignof(Fn)));
      (*aligned)();
      auto storage = static_cast<Fn *>(__builtin_alloca(sizeof(Fn)));
      *storage = &callback; (*storage)();
      unsigned value;
      (void)__builtin_add_overflow(1u, 2u, &value);
      (void)__builtin_sub_overflow(3u, 2u, &value);
      (void)__builtin_mul_overflow(3u, 2u, &value);
      (void)__builtin_bswap32(value); (void)__builtin_bswap64(value);
      (void)__builtin_parity(value); (void)__builtin_ffs(value);
      (void)__atomic_always_lock_free(sizeof(int), nullptr);
    }
  )cpp");
  const auto builtins = solve(builtin);
  if (!builtins.closedWithinInputs()) {
    for (const auto limitation : builtins.limitations)
      std::cerr << "builtin: " << builtins.metadata.text(limitation) << '\n';
    for (const auto &call : builtins.calls)
      if (call.unresolved)
        for (const auto target : call.targets)
          std::cerr << "unresolved builtin: "
                    << builtins.metadata.text(builtins.names.at(target))
                    << '\n';
  }
  check(
      edge(builtins, "entry", "callback") && builtins.closedWithinInputs() &&
          builtins.statistics.unknownCalls == 0,
      "alignment, stack allocation and scalar intrinsics preserve callback flow");
  check(
      completeCall(builtins, "__builtin_add_overflow",
                   CallEffect::WriteArguments) &&
          completeCall(builtins, "__builtin_alloca", CallEffect::BeginLifetime),
      "overflow writes and stack allocation lifetimes remain explicit");
  check(solve(builtin).statistics == builtins.statistics,
        "new intrinsic models retain incremental finalization");
}

void readOnlyRuntime() {
  CallGraphSession readers;
  add(readers, R"cpp(
    extern "C" unsigned long strlen(const char *);
    extern "C" int memcmp(const void *, const void *, unsigned long);
    extern "C" int strcmp(const char *, const char *);
    extern "C" int isalpha(int);
    extern "C" char *strchr(const char *, int);
    void preserved() {}
    void entry() {
      void (*p)() = &preserved;
      char first[] = {'a', 0}, second[] = {'b', 0};
      auto length = &strlen;
      (void)length(first); (void)memcmp(&p, &p, sizeof(p));
      (void)strcmp(first, second); (void)isalpha('x');
      (void)strchr(first, 'a'); p();
    }
  )cpp");
  check(solve(readers, false).statistics.modeledCalls == 0,
        "read-only runtime models wait for supplied definitions");
  const auto read = solve(readers);
  check(read.closedWithinInputs() && read.statistics.unknownCalls == 0 &&
            edge(read, "entry", "preserved"),
        "read-only APIs do not havoc pointer arguments or callbacks");
  check(
      completeCall(read, "strlen", CallEffect::ReadArguments) &&
          completeCall(read, "isalpha", CallEffect::ReadGlobals) &&
          completeCall(read, "strchr", CallEffect::InputToReturn),
      "runtime summaries preserve reads, locale input and return dependencies");

  CallGraphSession interior;
  add(interior, R"cpp(
    extern "C" void *memchr(const void *, int, unsigned long);
    void entry() {
      unsigned char data[sizeof(void (*)())] = {};
      auto found = static_cast<void (**)()>(memchr(data, 0, sizeof(data)));
      if (found) (*found)();
    }
  )cpp");
  const auto search = solve(interior);
  check(
      completeCall(search, "memchr", CallEffect::ReadArguments) &&
          search.statistics.unknownCalls > 0 && !search.closedWithinInputs(),
      "a modeled byte search does not prove an interior function-pointer load");
}

void emptyConstruction() {
  CallGraphSession empty;
  add(empty, R"cpp(
    struct Empty { Empty() = default; };
    struct EmptyBase {};
    struct EmptyDerived : EmptyBase { EmptyDerived() = default; };
    void entry() { Empty a{}; EmptyDerived b{}; }
  )cpp");
  const auto constructed = solve(empty);
  check(constructed.closedWithinInputs() &&
            completeCall(constructed, "Empty::Empty",
                         CallEffect::BeginLifetime) &&
            completeCall(constructed, "EmptyDerived::EmptyDerived",
                         CallEffect::BeginLifetime),
        "trivial empty construction does not require a materialized body");
}

void readRuntimePrecedence() {
  CallGraphSession replacement;
  add(replacement, R"cpp(
    extern "C" unsigned long strlen(const char *);
    void entry() { (void)strlen("x"); }
  )cpp");
  check(solve(replacement, false).statistics.modeledCalls == 0,
        "runtime read model is not applied before imports end");
  add(replacement, R"cpp(
    void replacement_effect();
    extern "C" unsigned long strlen(const char *) { replacement_effect(); return 0; }
  )cpp",
      "read-replacement.cpp");
  const auto replaced = solve(replacement);
  check(edge(replaced, "strlen", "replacement_effect") &&
            replaced.statistics.modeledCalls == 0 &&
            !replaced.closedWithinInputs(),
        "a supplied read-only API replacement is analyzed instead of modeled");
}

void memoryContracts() {
  CallGraphSession memory;
  add(memory, R"cpp(
    #include <cstdlib>
    #include <cstring>
    void entry() {
      char source[8] = {}, destination[8] = {};
      memcpy(destination, source, sizeof(source));
      memset(destination, 0, sizeof(destination));
      void *p = malloc(8); p = realloc(p, 16); free(p);
    }
  )cpp");
  const auto result = solve(memory);
  check(
      completeCall(result, "malloc", CallEffect::Allocate) &&
          completeCall(result, "realloc", CallEffect::EndLifetime) &&
          completeCall(result, "free", CallEffect::EndLifetime) &&
          completeCall(result, "memcpy", CallEffect::WriteArguments) &&
          completeCall(result, "memset", CallEffect::WriteArguments),
      "standard C memory contracts retain allocation, lifetime and byte writes");

  CallGraphSession fields;
  add(fields, R"cpp(
    #include <cstring>
    using Fn = void (*)();
    struct A { Fn cb; }; struct B { Fn cb; };
    void callback() {}
    void entry() {
      A source{&callback}; B destination{};
      memcpy(&destination, &source, sizeof(source));
      destination.cb();
    }
  )cpp");
  const auto copied = solve(fields);
  check(
      completeCall(copied, "memcpy", CallEffect::WriteArguments) &&
          copied.statistics.unknownCalls > 0 && !copied.closedWithinInputs(),
      "byte copies across different field declarations retain unknown callback contents");
}

void runtimeCatalog() {
  // Real system declarations exercise recognition with builtin-library
  // recognition disabled, including POSIX output positions and C++ methods.
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
    #include <cstdlib>
    #include <cstdio>
    #include <cstring>
    #include <cmath>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <pthread.h>
    #include <pwd.h>
    #include <chrono>
    #include <condition_variable>
    namespace user { double sqrt(double); int stat(const char *, void *); }
    double supplied(double value) { return value; }
  )cpp",
      {"-std=c++20", "-fno-builtin"}, "system-catalog.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse system API catalog");
  Declarations catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  using M = ExternalModel;
  for (const auto &[name, model] :
       std::initializer_list<std::pair<const char *, M>>{
           {"malloc", M::CAllocate},
           {"free", M::CFree},
           {"memcpy", M::CopyMemory},
           {"printf", M::Printf},
           {"sprintf", M::Sprintf},
           {"snprintf", M::Snprintf},
           {"puts", M::StreamOutput},
           {"fread", M::StreamInput},
           {"fopen", M::OpenStream},
           {"fclose", M::CloseStream},
           {"fseek", M::StreamOutput},
           {"ftell", M::StreamOutput},
           {"rewind", M::StreamOutput},
           {"sqrt", M::Math},
           {"modf", M::MathOutput},
           {"strtol", M::NumericConversion},
           {"strdup", M::DuplicateString},
           {"posix_memalign", M::OutAllocate},
           {"getenv", M::ExternalPointer},
           {"strerror", M::StaticBuffer},
           {"stat", M::ScalarSystemOutput},
           {"getcwd", M::ReturnBuffer0},
           {"getuid", M::SystemQuery},
           {"unlink", M::SystemUpdate},
           {"read", M::IORead},
           {"write", M::IOWrite},
           {"qsort", M::Sort},
           {"qsort_r", M::SortWithContext},
           {"bsearch", M::Search},
           {"getpwuid_r", M::PasswordLookup},
           {"pthread_create", M::ThreadRuntime},
           {"fork", M::ProcessRuntime},
           {"mmap", M::MapMemory},
           {"strtok_r", M::Tokenize},
           {"vsnprintf", M::VSnprintf},
           {"exit", M::TerminateRuntime},
           {"std::chrono::steady_clock::now", M::SystemQuery},
           {"std::condition_variable::wait", M::ThreadRuntime}}) {
    if (!catalog.models.contains(name) || catalog.models.at(name) != model)
      std::cerr << "unexpected catalog model: " << name << '\n';
    check(catalog.models.contains(name) && catalog.models.at(name) == model,
          "reviewed system declarations map to their shared contracts");
  }
  check(catalog.models.at("user::sqrt") == M::Unknown &&
            catalog.models.at("user::stat") == M::Unknown &&
            catalog.models.at("supplied") == M::Unknown,
        "user declarations and definitions do not acquire runtime contracts");
}

void runtimeLookupGuards() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
    # 1 "indexed-runtime.h" 1 3
    extern "C" {
      unsigned getuid();
      int getpagesize(int);
      long sysconf(int, ...);
      long wpgkit_unknown_runtime();
    }
    namespace user { unsigned getuid(); }
  )cpp",
      {"-std=c++20", "-fno-builtin"}, "indexed-runtime.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse indexed runtime declarations");
  Declarations catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  check(catalog.models.at("getuid") == ExternalModel::SystemQuery,
        "indexed runtime name selects its reviewed model");
  for (const auto *name :
       {"getpagesize", "sysconf", "wpgkit_unknown_runtime", "user::getuid"})
    check(catalog.models.at(name) == ExternalModel::Unknown,
          "indexed lookup preserves arity, variadic, scope and miss guards");
}

void mathRuntime() {
  CallGraphSession math;
  add(math, R"cpp(
    extern "C" double sqrt(double);
    void callback() {}
    void entry(double input) {
      auto cb = &callback;
      (void)sqrt(input); (void)__builtin_log2(input);
      (void)__builtin_isunordered(input, 1.0);
      (void)__builtin_signbit(input); cb();
    }
  )cpp");
  const auto calculated = solve(math);
  check(
      completeCall(calculated, "sqrt", CallEffect::FloatingEnvironment) &&
          completeCall(calculated, "__builtin_log2", CallEffect::WriteErrno) &&
          completeCall(calculated, "__builtin_isunordered",
                       CallEffect::InputToReturn) &&
          calculated.statistics.unknownCalls == 0 &&
          edge(calculated, "entry", "callback"),
      "math and floating predicates retain environment effects without callback havoc");
}

void outputBuffers() {
  CallGraphSession buffers;
  add(buffers, R"cpp(
    #include <cstdlib>
    #include <cstring>
    #include <cmath>
    #include <unistd.h>
    #include <sys/stat.h>
    void callback() {}
    void entry() {
      void *memory = nullptr; posix_memalign(&memory, 16, 32);
      auto slot = static_cast<void (**)()>(memory); *slot = &callback; (*slot)();
      char data[64] = "12"; char *end = nullptr;
      (void)strtol(data, &end, 10); (void)strdup(data);
      (void)getcwd(data, sizeof(data));
      struct stat status; (void)stat("file", &status);
      double integer; (void)modf(1.5, &integer);
    }
  )cpp");
  const auto output = solve(buffers);
  check(
      completeCall(output, "posix_memalign", CallEffect::WriteArguments) &&
          completeCall(output, "strtol", CallEffect::InputToMemory) &&
          completeCall(output, "strdup", CallEffect::Allocate) &&
          completeCall(output, "getcwd", CallEffect::WriteArguments) &&
          completeCall(output, "stat", CallEffect::WriteArguments) &&
          completeCall(output, "modf", CallEffect::WriteArguments) &&
          edge(output, "entry", "callback"),
      "output buffers, allocated output pointers and aliases survive AST destruction");
}

void callbackRuntime() {
  CallGraphSession callbacks;
  add(callbacks, R"cpp(
    #include <cstdlib>
    int state;
    int compare(const void *, const void *) { return ++state; }
    void entry() { int items[2] = {}; qsort(items, 2, sizeof(int), &compare); }
  )cpp");
  const auto callbackResult = solve(callbacks);
  check(!callbackResult.closedWithinInputs() &&
            callbackResult.statistics.unknownCalls > 0 &&
            !edge(callbackResult, "entry", "compare"),
        "sorting applies a partial summary without activating the comparator");
}

void streamRuntime() {
  CallGraphSession streams;
  add(streams, R"cpp(
    #include <cstdio>
    void entry() {
      if (auto *file = fopen("file", "r")) {
        (void)fseek(file, 0, SEEK_END); (void)ftell(file); rewind(file); fclose(file);
      }
    }
  )cpp");
  const auto streamResult = solve(streams);
  check(completeCall(streamResult, "fopen", CallEffect::BeginLifetime) &&
            completeCall(streamResult, "fclose", CallEffect::EndLifetime) &&
            completeCall(streamResult, "fseek", CallEffect::WriteStream) &&
            completeCall(streamResult, "rewind", CallEffect::WriteStream),
        "standard stream contracts preserve stream state and lifetime effects");
}

void vectorIntrinsics() {
  CallGraphSession vectors;
  add(vectors, R"cpp(
    using V4 = int __attribute__((vector_size(16)));
    using V2 = long long __attribute__((vector_size(16)));
    void callback() {}
    void entry() {
      V4 a{}; V2 b{}; auto cb = &callback;
      (void)__builtin_ia32_pshufd(a, 0);
      (void)__builtin_ia32_pmuludq128(a, a);
      (void)__builtin_ia32_psllqi128(b, 1);
      (void)__builtin_ia32_psrlqi128(b, 1);
      (void)__builtin_rotateleft64(1, 2); __builtin_prefetch(&cb); cb();
    }
  )cpp");
  const auto vectorResult = solve(vectors);
  check(
      vectorResult.closedWithinInputs() &&
          vectorResult.statistics.unknownCalls == 0 &&
          edge(vectorResult, "entry", "callback"),
      "reviewed integer SIMD, rotate and prefetch builtins do not havoc callback storage");
}

void forwardingAndFinalization() {
  CallGraphSession intrinsics;
  add(intrinsics, R"cpp(
    #include <utility>
    #include <memory>
    using Fn = void (*)();
    void first() {} void second() {}
    void relay(Fn);
    void entry() {
      Fn p = &first;
      auto q = std::move(p); relay(std::forward<Fn>(q));
      (*std::addressof(p))();
      std::forward<Fn &>(p) = &second;
      std::as_const(p)();
      auto function = std::addressof(first); function();
      (*__builtin_addressof(p))();
    }
  )cpp");
  const auto provisional = solve(intrinsics, false);
  check(provisional.statistics.modeledCalls > 0 && !provisional.finalized,
        "intrinsics refine calls before unknown effects are finalized");
  add(intrinsics, "void relay(void (*cb)()) { cb(); }", "relay.cpp");
  const auto intrinsic = solve(intrinsics);
  if (!intrinsic.unresolvedBodies.empty())
    for (const auto limitation : intrinsic.limitations)
      std::cerr << intrinsic.metadata.text(limitation) << '\n';
  check(
      edge(intrinsic, "relay", "first") && edge(intrinsic, "entry", "second") &&
          intrinsic.unresolvedBodies.empty(),
      "forwarding, reference writes and addressof preserve callback flow across released ASTs");
  check(solve(intrinsics).statistics == intrinsic.statistics,
        "repeated finalization does no solver/model work");
  bool rejected = false;
  try {
    add(intrinsics, "void later() {}", "late.cpp");
  } catch (const std::logic_error &) {
    rejected = true;
  }
  check(rejected,
        "finalized unknown effects cannot be followed by further imports");
}

void evaluatedOperands() {
  CallGraphSession unevaluated;
  add(unevaluated, R"cpp(
    void actual() {} void ghost() {}
    int missing();
    void entry() {
      using T = decltype(missing());
      (void)sizeof(missing()); (void)noexcept(missing());
      void (*p)() = &actual;
      (void)sizeof(p = &ghost);
      (void)__builtin_constant_p((ghost(), 1));
      p();
    }
  )cpp");
  const auto evaluated = solve(unevaluated);
  if (evaluated.calls.size() != 2 || !evaluated.closedWithinInputs()) {
    for (const auto &site : evaluated.calls)
      for (const auto target : site.targets)
        std::cerr << "evaluated: "
                  << evaluated.metadata.text(evaluated.names.at(target))
                  << '\n';
    for (const auto limitation : evaluated.limitations)
      std::cerr << evaluated.metadata.text(limitation) << '\n';
  }
  check(evaluated.calls.size() == 2 && edge(evaluated, "entry", "actual") &&
            edge(evaluated, "entry", "__builtin_constant_p") &&
            !edge(evaluated, "entry", "ghost") &&
            evaluated.closedWithinInputs(),
        "unevaluated syntax creates neither calls nor pointer facts");

  CallGraphSession exceptions;
  add(exceptions, R"cpp(
    #include <typeinfo>
    int bound() { return 2; }
    struct Base { virtual ~Base() {} };
    Base &object();
    void entry() {
      int array[bound()]; (void)array;
      (void)sizeof(int[bound()]);
      __typeof__(*(int (*)[bound()])nullptr) another; (void)another;
      (void)typeid(object());
    }
  )cpp");
  const auto evaluatedExceptions = solve(exceptions);
  unsigned bounds = 0;
  for (const auto &site : evaluatedExceptions.calls)
    for (const auto target : site.targets)
      bounds += evaluatedExceptions.metadata.text(
                    evaluatedExceptions.names.at(target)) == "bound";
  check(
      edge(evaluatedExceptions, "entry", "bound") && bounds == 3 &&
          edge(evaluatedExceptions, "entry", "object"),
      "evaluated VLA/typeof bounds and polymorphic typeid operands remain calls");
}

void virtualDispatch() {
  CallGraphSession virtuals;
  add(virtuals, R"cpp(
    struct Base { virtual void run() = 0; };
    void Base::run() {}
    struct Derived : Base { void run() override {} };
    void qualified(Base *p) { p->Base::run(); }
    void entry(Base *p) { p->run(); qualified(p); }
  )cpp");
  const auto dispatch = solve(virtuals);
  check(
      !edge(dispatch, "entry", "Base::run") &&
          edge(dispatch, "entry", "Derived::run") &&
          edge(dispatch, "qualified", "Base::run"),
      "pure virtual slots are traversed but only qualified calls execute their bodies");
}

void memoryMovement() {
  CallGraphSession copying;
  add(copying, R"cpp(
    #include <cstring>
    void copied() {}
    void entry() {
      void (*source)() = &copied;
      void (*destination)() = nullptr;
      auto copy = &memcpy;
      copy(&destination, &source, sizeof(source));
      destination();
    }
  )cpp");
  const auto copied = solve(copying);
  check(
      edge(copied, "entry", "copied") && !copied.closedWithinInputs() &&
          copied.statistics.modeledCalls > 0 &&
          copied.statistics.unknownCalls > 0,
      "indirect memory API model discovers callbacks without claiming complete byte effects");

  CallGraphSession allocation;
  add(allocation, R"cpp(
    #include <cstdlib>
    using Fn = void (*)();
    void moved() {}
    void entry() {
      auto old = static_cast<Fn *>(malloc(sizeof(Fn)));
      *old = &moved;
      auto next = static_cast<Fn *>(realloc(old, sizeof(Fn)));
      (*next)(); free(next);
    }
  )cpp");
  const auto allocated = solve(allocation);
  check(
      edge(allocated, "entry", "moved") && !allocated.closedWithinInputs(),
      "allocation/reallocation preserve stored callback targets and unknown runtime effects");
}

void unknownEffects() {
  CallGraphSession fallback;
  add(fallback, R"cpp(
    using Fn = void (*)();
    void original() {} void input() {} void global() {}
    Fn saved = &global;
    Fn external(Fn **out, Fn in);
    void entry() {
      Fn p = &original; Fn *nested = &p;
      Fn result = external(&nested, &input);
      result(); p(); saved();
    }
  )cpp");
  const auto beforeFallback = solve(fallback, false);
  check(!edge(beforeFallback, "entry", "input") &&
            beforeFallback.statistics.unknownCalls == 0,
        "missing-body effects remain provisional before finalization");
  const auto conservative = solve(fallback);
  check(
      edge(conservative, "entry", "input") &&
          edge(conservative, "entry", "original") &&
          edge(conservative, "entry", "global") &&
          !conservative.closedWithinInputs(),
      "unknown calls preserve input/return aliases, nested storage and global targets");
  bool hasFallback = false;
  for (const auto &site : conservative.calls)
    hasFallback |= site.externalEffects.has(CallEffect::Callback) &&
                   site.externalEffects.has(CallEffect::InputToMemory) &&
                   site.externalEffects.has(CallEffect::WriteGlobals);
  check(
      hasFallback && solve(fallback).statistics == conservative.statistics,
      "unknown callback/memory effects are explicit and converge incrementally");
}

void allocationPrecedence() {
  CallGraphSession lateRuntime;
  add(lateRuntime, R"cpp(
    extern "C" void *malloc(unsigned long);
    void entry() { (void)malloc(8); }
  )cpp");
  check(solve(lateRuntime, false).statistics.modeledCalls == 0,
        "replaceable runtime models wait until imports finish");
  add(lateRuntime,
      "extern \"C\" void *malloc(unsigned long) { return nullptr; }",
      "replacement.cpp");
  const auto replacement = solve(lateRuntime);
  check(replacement.closedWithinInputs() &&
            replacement.statistics.modeledCalls == 0,
        "a late supplied implementation takes precedence over a runtime model");
}
} // namespace

int main() {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(R"cpp(
    #include <utility>
    #include <memory>
    #include <cstdlib>
    #include <cstring>
    namespace user {
      void *malloc(unsigned long); int *move(int *);
      unsigned long strlen(const char *); int isalpha(int);
    }
    struct Empty { Empty() = default; };
    struct Nonempty { int *pointer; Nonempty() = default; };
    struct UserConstructor { UserConstructor() {} };
    void entry() {
      int *p = nullptr;
      (void)std::move(p); (void)std::forward<int *>(p);
      (void)std::as_const(p); (void)std::addressof(p);
      (void)__builtin_expect(0, 1);
      Empty a; Nonempty b; UserConstructor c;
    }
  )cpp",
                                               {"-std=c++20"}, "catalog.cpp");
  check(ast != nullptr, "parse real standard library declarations");
  Declarations declarations;
  declarations.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  for (const auto *name : {"std::move", "std::forward", "std::as_const"})
    check(declarations.models.at(name) == ExternalModel::ForwardReference,
          "Clang standard-library intrinsic recognition");
  check(declarations.models.at("std::addressof") == ExternalModel::AddressOf &&
            declarations.models.at("malloc") == ExternalModel::CAllocate &&
            declarations.models.at("realloc") == ExternalModel::Reallocate &&
            declarations.models.at("memcpy") == ExternalModel::CopyMemory &&
            declarations.models.at("free") == ExternalModel::CFree,
        "runtime declarations select reviewed families");
  check(declarations.models.at("user::malloc") == ExternalModel::Unknown &&
            declarations.models.at("user::move") == ExternalModel::Unknown &&
            declarations.models.at("user::strlen") == ExternalModel::Unknown &&
            declarations.models.at("user::isalpha") == ExternalModel::Unknown,
        "same-spelled user functions do not receive library models");
  check(
      declarations.models.at("Empty::Empty") ==
              ExternalModel::EmptyConstructor &&
          declarations.models.at("Nonempty::Nonempty") ==
              ExternalModel::TrivialDefaultConstructor &&
          declarations.models.at("UserConstructor::UserConstructor") ==
              ExternalModel::Unknown,
      "only trivial empty default constructors receive the lifetime-only model");
  ast.reset();
  check(ExternalModules::summary(ExternalModel::ForwardReference).complete(),
        "intrinsic contract is owned after AST destruction");
  const auto unknown = ExternalModules::summary(ExternalModel::Unknown);
  check(!unknown.complete() && unknown.has(CallEffect::WriteArguments) &&
            unknown.has(CallEffect::WriteGlobals) &&
            unknown.has(CallEffect::Callback) &&
            unknown.has(CallEffect::Escape) &&
            unknown.has(CallEffect::InputToReturn),
        "unknown contract retains memory, alias, escape and callback effects");
  check(
      !ExternalModules::summary(ExternalModel::Allocate).complete() &&
          ExternalModules::summary(ExternalModel::CopyMemory).complete() &&
          ExternalModules::summary(ExternalModel::CAllocate).complete(),
      "standard C contracts and replaceable C++ allocation have distinct coverage");
  forwardingAndFinalization();
  evaluatedOperands();
  virtualDispatch();
  memoryMovement();
  unknownEffects();
  allocationPrecedence();
  builtinIntrinsics();
  readOnlyRuntime();
  emptyConstruction();
  readRuntimePrecedence();
  memoryContracts();
  runtimeCatalog();
  runtimeLookupGuards();
  mathRuntime();
  outputBuffers();
  callbackRuntime();
  streamRuntime();
  vectorIntrinsics();
}
