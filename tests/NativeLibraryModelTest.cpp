#include "CallGraphPTA.h"
#include "ExternalEffects.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

CallGraphResult analyze(const char *code, const char *language = "-std=c++20") {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {language, "-fno-builtin"}, "native-library.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  CallGraphSession session;
  session.addUnit(ast->getASTContext());
  ast.reset();
  auto result = session.finalize({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  return std::move(*result);
}

void listLinks() {
  const auto result = analyze(R"cpp(
    #include <list>
    void entry(std::__detail::_List_node_base *first,
               std::__detail::_List_node_base *last) {
      first->_M_hook(last);
      first->_M_unhook();
      first->_M_transfer(first, last);
      first->_M_reverse();
      std::__detail::_List_node_base::swap(*first, *last);
    }
  )cpp");
  check(result.calls.size() == 5 && result.statistics.modeledCalls == 5 &&
            result.statistics.unknownCalls == 0,
        "all reviewed list-link operations use bounded models");
  for (const auto &call : result.calls)
    check(!call.unresolved &&
              call.externalEffects.has(CallEffect::WriteArguments) &&
              !call.externalEffects.has(CallEffect::Callback),
          "list-link rewrites do not invoke element callbacks");
}

void llvmBufferAllocation() {
  const auto result = analyze(R"cpp(
    # 1 "llvm/Support/MemAlloc.h"
    namespace llvm {
      __attribute__((malloc, returns_nonnull))
      void *allocate_buffer(__SIZE_TYPE__, __SIZE_TYPE__);
    }
    # 1 "user.cpp"
    void entry() {
      auto value = static_cast<void **>(llvm::allocate_buffer(sizeof(void *), 16));
      *value = nullptr;
    }
  )cpp");
  check(result.calls.size() == 1 && result.statistics.modeledCalls == 1 &&
            result.calls.front().unresolved,
        "LLVM allocation preserves its incomplete runtime boundary");
  for (const auto target : result.calls.front().targets)
    check(result.externalModels.at(target) == ExternalModel::DefaultAllocate,
          "the annotated LLVM allocator supplies a fresh return contract");
  check(
      !result.hasLimitation("store through unknown address"),
      "unknown allocator callbacks do not obscure the fresh returned pointer");
}

void llvmBufferBoundaries() {
  for (
      const auto *declaration :
      {"# 1 \"llvm/Support/MemAlloc.h\"\nnamespace llvm { void *allocate_buffer(__SIZE_TYPE__, __SIZE_TYPE__); }",
       "# 1 \"other.h\"\nnamespace llvm { __attribute__((malloc, returns_nonnull)) void *allocate_buffer(__SIZE_TYPE__, __SIZE_TYPE__); }",
       "# 1 \"llvm/Support/MemAlloc.h\"\nnamespace llvm { __attribute__((malloc, returns_nonnull)) void *allocate_buffer(int, int); }"}) {
    const auto code =
        std::string(declaration) +
        "\n# 1 \"user.cpp\"\nvoid entry() { llvm::allocate_buffer(64, 16); }";
    const auto result = analyze(code.c_str());
    check(
        result.statistics.modeledCalls == 0 && result.calls.size() == 1 &&
            result.calls.front().unresolved,
        "changed allocator provenance, attributes and signatures remain open");
  }
  const auto supplied = analyze(R"cpp(
    # 1 "llvm/Support/MemAlloc.h"
    namespace llvm {
      void observe();
      __attribute__((malloc, returns_nonnull))
      void *allocate_buffer(__SIZE_TYPE__ size, __SIZE_TYPE__) {
        observe(); return __builtin_malloc(size);
      }
    }
    void entry() { llvm::allocate_buffer(64, 16); }
  )cpp");
  check(supplied.calls.size() == 3,
        "an available allocator body retains its own calls and effects");
}

void passwordLookup() {
  const auto result = analyze(R"cpp(
    #include <pwd.h>
    void entry(passwd *record, passwd **result, char *buffer) {
      getpwnam_r("user", record, buffer, 256, result);
      getpwuid_r(1, record, buffer, 256, result);
    }
  )cpp");
  check(result.calls.size() == 2 && result.statistics.modeledCalls == 2,
        "both password lookup signatures have a model");
  for (const auto &call : result.calls) {
    check(call.unresolved && call.externalEffects.has(CallEffect::Callback),
          "NSS callbacks and cancellation remain explicit");
    for (const auto id : call.targets)
      check(result.externalModels.at(id) == ExternalModel::PasswordLookup,
            "password lookup has a specific output contract");
  }
}

void passwordOutputAlias() {
  Andersen solver;
  ExternalEffects effects(solver);
  std::array<CallArgument, 5> args;
  for (auto &arg : args)
    arg = {solver.addNode(), ParamKind::Pointer};
  args[3].kind = ParamKind::Other;
  const auto record = solver.addNode();
  const auto output = solver.addNode();
  solver.address(args[1].node, record);
  solver.address(args[4].node, output);
  bool unresolved = false;
  ExternalSummary summary;
  effects.apply(
      {args, solver.addNode(), std::nullopt, false, 0, unresolved, summary},
      ExternalModel::PasswordLookup);
  check(solver.solve() && solver.mayPointTo(output, record),
        "successful lookup returns the caller's record through the output");
  check(!summary.complete() && summary.has(CallEffect::InputToMemory),
        "the known alias does not conceal unknown NSS effects");
}

void signatureBoundaries() {
  const auto result = analyze(R"cpp(
    # 1 "unsupported-system.h" 1 3
    struct passwd;
    extern "C" int getpwnam_r(const char *, passwd *, char *, unsigned long,
                              void **);
    namespace std::__detail {
    struct _List_node_base { void _M_hook(const _List_node_base *); };
    }
    # 1 "user.cpp" 2
    void entry(passwd *record, char *buffer, void **output,
               std::__detail::_List_node_base *node) {
      getpwnam_r("user", record, buffer, 128, output);
      node->_M_hook(node);
    }
  )cpp");
  check(result.calls.size() == 2 &&
            std::all_of(result.calls.begin(), result.calls.end(),
                        [](const auto &call) { return call.unresolved; }),
        "changed pointer output and list signatures stay open");
}

void directoryEffects() {
  const auto result = analyze(R"cpp(
    #include <dirent.h>
    void entry() {
      DIR *directory = opendir(".");
      if (directory) { readdir(directory); closedir(directory); }
    }
  )cpp");
  check(result.calls.size() == 3 && result.statistics.modeledCalls == 3 &&
            result.statistics.unknownCalls == 0,
        "glibc directory operations have bounded noncancelling effects");
  for (const auto &call : result.calls)
    check(call.externalEffects.complete() &&
              !call.externalEffects.has(CallEffect::Callback),
          "directory effects do not introduce cleanup callbacks");
  Andersen solver;
  ExternalEffects effects(solver);
  const auto directory = solver.addNode(), storage = solver.addNode();
  const auto returned = solver.addNode();
  solver.address(directory, storage);
  std::array<CallArgument, 1> args{{{directory, ParamKind::Pointer}}};
  bool unresolved = false;
  ExternalSummary summary;
  effects.apply({args, returned, std::nullopt, true, 0, unresolved, summary},
                ExternalModel::DirectoryRead);
  check(solver.solve() && solver.mayPointTo(returned, storage) &&
            solver.isUnknown(returned),
        "readdir preserves a buffer alias and unknown interior offset");
}

void directorySignatures() {
  const auto result = analyze(R"cpp(
    # 1 "unsupported-system.h" 1 3
    struct __dir; typedef __dir DIR; struct dirent;
    extern "C" void *opendir(const char *);
    extern "C" dirent *readdir(const DIR *);
    extern "C" long closedir(DIR *);
    # 1 "user.cpp" 2
    void entry(DIR *d) { opendir("."); readdir(d); closedir(d); }
  )cpp");
  check(result.calls.size() == 3 &&
            std::all_of(result.calls.begin(), result.calls.end(),
                        [](const auto &call) { return call.unresolved; }),
        "directory signature changes retain the unknown runtime boundary");
}

void processReplacement() {
  const auto result = analyze(R"cpp(
    #include <unistd.h>
    void target() {}
    void entry(const char *path, char *const *arguments, char *const *environment) {
      auto callback = &target;
      execv(path, arguments); execve(path, arguments, environment);
      callback();
    }
  )cpp");
  check(result.calls.size() == 3 && result.statistics.modeledCalls == 2 &&
            result.statistics.unknownCalls == 0,
        "exec failure paths do not clobber unrelated callback targets");
  unsigned replacements = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.externalModels.contains(target) &&
          result.externalModels.at(target) == ExternalModel::ProcessReplace) {
        ++replacements;
        check(call.externalEffects.has(CallEffect::NoReturn) &&
                  call.externalEffects.has(CallEffect::WriteErrno) &&
                  !call.externalEffects.has(CallEffect::WriteArguments),
              "replacement and failure effects retain readonly input vectors");
      }
  check(replacements == 2, "both exec signatures use the reviewed contract");
  const auto rejected = analyze(R"cpp(
    # 1 "unsupported-system.h" 1 3
    extern "C" long execv(const char *, char *const *);
    extern "C" int execve(const char *, void **, void **);
    # 1 "user.cpp" 2
    void entry() { execv("program", nullptr); execve("program", nullptr, nullptr); }
  )cpp");
  check(rejected.calls.size() == 2 && rejected.statistics.unknownCalls == 2,
        "unsupported exec signatures remain open");
}

void suppliedListBody() {
  const auto result = analyze(R"cpp(
    # 1 "system-list.h" 1 3
    namespace std::__detail {
    struct _List_node_base { void _M_hook(_List_node_base *); };
    }
    # 1 "user.cpp" 2
    void external();
    void std::__detail::_List_node_base::_M_hook(_List_node_base *) {
      external();
    }
    void entry(std::__detail::_List_node_base *node) { node->_M_hook(node); }
  )cpp");
  check(result.statistics.modeledCalls == 0 &&
            std::any_of(result.calls.begin(), result.calls.end(),
                        [](const auto &call) { return call.unresolved; }),
        "a supplied implementation overrides the catalog contract");
}

void llvmStreamDelegates() {
  const auto result = analyze(R"cpp(
    # 1 "llvm/Support/circular_raw_ostream.h"
    namespace llvm {
    struct circular_raw_ostream {
      void write_impl(const char *, __SIZE_TYPE__);
      void flushBufferWithBanner();
    };
    }
    # 1 "llvm/Support/raw_os_ostream.h"
    namespace llvm {
    struct raw_os_ostream {
      void write_impl(const char *, __SIZE_TYPE__);
      unsigned long current_pos() const;
      ~raw_os_ostream();
    };
    }
    # 1 "llvm/Object/Error.h"
    namespace llvm {
    struct raw_ostream;
    namespace object {
    struct GenericBinaryError { void log(raw_ostream &) const; };
    }
    }
    # 1 "user.cpp"
    void entry(llvm::circular_raw_ostream *c, llvm::raw_os_ostream *s,
               const llvm::object::GenericBinaryError *error,
               llvm::raw_ostream &out) {
      c->write_impl("text", 4); c->flushBufferWithBanner();
      s->write_impl("text", 4); s->current_pos(); s->~raw_os_ostream();
      error->log(out);
    }
  )cpp");
  check(result.calls.size() == 6 && result.statistics.modeledCalls == 6,
        "LLVM stream forwarding signatures use reviewed contracts");
  for (const auto &call : result.calls)
    check(call.unresolved && call.externalEffects.has(CallEffect::Callback),
          "unknown raw_ostream/streambuf overrides remain explicit");
}

void llvmReadOnlyQueries() {
  const auto result = analyze(R"cpp(
    namespace std { class error_category; }
    # 1 "llvm/ProfileData/InstrProf.h"
    namespace llvm { const std::error_category &instrprof_category(); }
    # 1 "llvm/Support/Error.h"
    namespace llvm {
    namespace object { struct GenericBinaryError; struct BinaryError; }
    template<class T, class P> struct ErrorInfo { bool isA(const void *) const; };
    }
    # 1 "user.cpp"
    void entry(const llvm::ErrorInfo<llvm::object::GenericBinaryError,
                                    llvm::object::BinaryError> *error) {
      llvm::instrprof_category();
      error->isA(nullptr);
    }
  )cpp");
  if (result.statistics.modeledCalls != 2 || result.statistics.unknownCalls)
    for (const auto &call : result.calls)
      for (const auto id : call.targets)
        std::cerr << result.metadata.text(result.names.at(id)) << " model "
                  << static_cast<unsigned>(result.externalModels.contains(id)
                                               ? result.externalModels.at(id)
                                               : ExternalModel::Unknown)
                  << " unresolved " << call.unresolved << '\n';
  check(result.calls.size() == 2 && result.statistics.modeledCalls == 2 &&
            result.statistics.unknownCalls == 0,
        "reviewed category and type queries have bounded effects");
}

void llvmIdentityBoundaries() {
  const auto result = analyze(R"cpp(
    namespace llvm {
    struct circular_raw_ostream { void flushBufferWithBanner(); };
    }
    # 1 "llvm/Support/raw_os_ostream.h"
    namespace llvm {
    struct raw_os_ostream { void write_impl(char **, __SIZE_TYPE__); };
    }
    # 1 "llvm/Support/Error.h"
    namespace llvm {
    namespace object { struct GenericBinaryError; }
    template<class T, class P> struct ErrorInfo { bool isA(const void *) const; };
    }
    # 1 "user.cpp"
    struct CustomParent {};
    void entry(llvm::circular_raw_ostream *c, llvm::raw_os_ostream *s,
               const llvm::ErrorInfo<llvm::object::GenericBinaryError,
                                    CustomParent> *error, char **text) {
      c->flushBufferWithBanner(); s->write_impl(text, 1); error->isA(nullptr);
    }
  )cpp");
  check(
      result.calls.size() == 3 && result.statistics.modeledCalls == 0,
      "non-LLVM headers, changed signatures and custom CRTP parents do not match");
}

void loaderAndFutex() {
  const auto result = analyze(R"cpp(
    #include <dlfcn.h>
    #include <bits/atomic_futex.h>
    void entry(const void *address, Dl_info *info, unsigned *state,
               std::__atomic_futex_unsigned_base *waiter) {
      dladdr(address, info);
      waiter->_M_futex_wait_until(state, 1, false, std::chrono::seconds(0),
                                  std::chrono::nanoseconds(0));
      waiter->_M_futex_wait_until_steady(state, 1, false, std::chrono::seconds(0),
                                         std::chrono::nanoseconds(0));
    }
  )cpp");
  unsigned checked = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto model = result.externalModels.find(target);
      if (model == result.externalModels.end() ||
          (model->second != ExternalModel::LoaderQuery &&
           model->second != ExternalModel::FutexWait))
        continue;
      ++checked;
      check(!call.unresolved &&
                call.externalEffects.has(CallEffect::Synchronize) &&
                !call.externalEffects.has(CallEffect::Callback),
            "loader metadata and native futex waits have bounded contracts");
      if (model->second == ExternalModel::FutexWait)
        check(!call.externalEffects.has(CallEffect::WriteArguments),
              "futex wait does not replace the observed pointer storage");
    }
  check(checked == 3, "all three reviewed native operations were recognized");
}

void loaderAndFutexSignatures() {
  const auto result = analyze(R"cpp(
    # 1 "unsupported-system.h" 1 3
    typedef struct { void *symbol; } Dl_info;
    extern "C" int dladdr(const void *, const Dl_info *);
    namespace std {
    struct __atomic_futex_unsigned_base {
      bool _M_futex_wait_until(unsigned *, unsigned, bool, long, long);
    };
    }
    # 1 "user.cpp" 2
    void entry(Dl_info *info, unsigned *state,
               std::__atomic_futex_unsigned_base *waiter) {
      dladdr(state, info);
      waiter->_M_futex_wait_until(state, 1, false, 0, 0);
    }
  )cpp");
  check(result.calls.size() == 2 &&
            std::all_of(result.calls.begin(), result.calls.end(),
                        [](const auto &call) { return call.unresolved; }),
        "loader const output and unsupported duration types stay open");
}

void stringAndThreadState() {
  const auto result = analyze(R"cpp(
    #define _GLIBCXX_THREAD_IMPL
    #include <thread>
    #include <string>
    void entry(std::string &a, std::string &b, std::thread::_State *state) {
      a.swap(b);
      a.append(2, 'x');
      state->std::thread::_State::~_State();
    }
  )cpp",
                              "-std=c++17");
  unsigned checked = 0;
  bool replace = false;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto model = result.externalModels.find(target);
      if (model == result.externalModels.end())
        continue;
      replace |= model->second == ExternalModel::StringReturnSelf &&
                 result.metadata.text(result.names.at(target))
                     .ends_with("::_M_replace_aux");
      if (model->second == ExternalModel::StringSwap ||
          model->second == ExternalModel::RuntimeDestructor) {
        ++checked;
        check(
            !call.unresolved &&
                !call.externalEffects.has(CallEffect::Allocate) &&
                !call.externalEffects.has(CallEffect::Callback),
            "standard string swap and the empty thread base destructor are bounded");
      }
    }
  check(
      checked == 2 && replace,
      "string swap, repeated-character replacement and base destruction are modeled");
}

void trivialPointerConstruction() {
  const auto result = analyze(R"cpp(
    #include <new>
    struct Object { void (*callback)(); Object() = default; };
    void target() {}
    void entry() {
      Object object;
      object.callback = &target;
      // Default construction reuses the pointer value left in this storage.
      auto *reused = new (&object) Object;
      reused->callback();
      Object zeroed{};
    }
  )cpp");
  check(result.statistics.unknownCalls == 0 &&
            std::any_of(result.reachable.begin(), result.reachable.end(),
                        [&](auto id) {
                          return result.metadata.text(result.names.at(id)) ==
                                 "target";
                        }),
        "trivial construction preserves existing targets under storage reuse");
  const auto uninitialized = analyze(R"cpp(
    struct Object { void (*callback)(); Object() = default; };
    void entry() { Object object; object.callback(); }
  )cpp");
  check(
      uninitialized.statistics.unknownCalls == 1,
      "a missing pointer definition remains unknown after default construction");
}

void futureResultInitialization() {
  const auto result = analyze(R"cpp(
    #include <future>
    struct Result : std::__future_base::_Result_base {
      void _M_destroy() override {}
    };
    void entry(Result *storage) { new (storage) Result; }
  )cpp");
  bool found = false;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto model = result.externalModels.find(target);
      if (model == result.externalModels.end() ||
          model->second != ExternalModel::NullObjectInitialization)
        continue;
      found = true;
      check(
          !call.unresolved &&
              call.externalEffects.has(CallEffect::BeginLifetime),
          "future result initialization has no hidden allocation or callback");
    }
  check(found, "the actual libstdc++ result layout is recognized");
}
} // namespace

int main() {
  llvmBufferAllocation();
  llvmBufferBoundaries();
  directoryEffects();
  directorySignatures();
  processReplacement();
  listLinks();
  passwordLookup();
  passwordOutputAlias();
  signatureBoundaries();
  suppliedListBody();
  llvmStreamDelegates();
  llvmReadOnlyQueries();
  llvmIdentityBoundaries();
  loaderAndFutex();
  loaderAndFutexSignatures();
  stringAndThreadState();
  trivialPointerConstruction();
  futureResultInitialization();
}
