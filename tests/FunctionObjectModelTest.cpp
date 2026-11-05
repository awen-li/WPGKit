#include "CallGraphPTA.h"
#include "ExternalEffects.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

CallGraphResult analyze(const std::string &code) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {"-std=c++20"}, "function-object-model.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  CallGraphSession session;
  session.addUnit(ast->getASTContext());
  ast.reset();
  auto result = session.finalize({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  auto again = session.finalize({"entry"});
  check(again && again->interCFG == result->interCFG &&
            again->statistics == result->statistics,
        "summaries remain incremental after AST release");
  return std::move(*result);
}

void inlineManagers() {
  const auto result = analyze(R"cpp(
    #include <functional>
    struct Small { int *value; int operator()() const { return *value; } };
    struct Manager : std::_Function_base::_Base_manager<Small> {
      using std::_Function_base::_Base_manager<Small>::_M_get_pointer;
    };
    using Handler = std::_Function_handler<int(), Small>;
    void entry(std::_Any_data &dest, const std::_Any_data &source, Small &f) {
      Manager::_M_init_functor(dest, f);
      Manager::_M_manager(dest, source, std::__clone_functor);
      Handler::_M_manager(dest, source, std::__destroy_functor);
      (void)Manager::_M_get_pointer(source);
    }
  )cpp");
  check(
      result.calls.size() == 4 && result.statistics.modeledCalls == 4 &&
          result.statistics.unknownCalls == 0 && result.reachable.size() == 1,
      "inline managers summarize storage operations without invoking targets");
  check(std::count_if(
            result.externalModels.begin(), result.externalModels.end(),
            [](const auto &entry) {
              return entry.second == ExternalModel::InlineFunctionPointer;
            }) == 1,
        "inline pointer lookup uses the instantiated storage decision");
  for (const auto &call : result.calls)
    check(!call.unresolved && call.externalEffects.complete() &&
              !call.externalEffects.has(CallEffect::Callback) &&
              !call.externalEffects.has(CallEffect::Allocate),
          "trivial inline storage has no allocator or user-code effects");
}

void storageTransfers() {
  for (const auto model :
       {ExternalModel::InlineFunctionCopy, ExternalModel::InlineFunctionManager,
        ExternalModel::FunctionTargetPointer,
        ExternalModel::FunctionObjectPointer,
        ExternalModel::InlineFunctionPointer}) {
    Andersen pta;
    ExternalEffects models(pta);
    const auto dst = pta.addNode(), dstObject = pta.addNode();
    const auto src = pta.addNode(), srcObject = pta.addNode();
    const auto capture = pta.addNode(), nested = pta.addNode();
    const auto result = pta.addNode();
    pta.address(dst, dstObject);
    pta.address(src, srcObject);
    pta.address(srcObject, capture);
    pta.address(capture, nested);
    std::array<CallArgument, 2> args{
        {{dst, ParamKind::Reference}, {src, ParamKind::Reference}}};
    const bool target = model == ExternalModel::FunctionTargetPointer;
    const bool local = model == ExternalModel::InlineFunctionPointer;
    const bool pointer =
        target || local || model == ExternalModel::FunctionObjectPointer;
    if (pointer && !target)
      args[0].node = src;
    bool unresolved = false;
    ExternalSummary summary;
    models.apply({args, result, target ? std::optional{src} : std::nullopt,
                  pointer, 0, unresolved, summary},
                 model);
    check(pta.solve() && !unresolved && summary.complete(),
          "numeric summary converges without function-body expansion");
    check(!pta.isUnknown(srcObject) && !pta.isUnknown(capture) &&
              pta.mayPointTo(capture, nested),
          "copying callable storage does not havoc captured pointees");
    if (pointer)
      check(pta.mayPointTo(result, capture) == !local &&
                pta.isUnknown(result) &&
                pta.mayPointTo(result, srcObject) == (target || local),
            "lookup distinguishes inline storage from a retained heap pointer");
    else
      check(
          pta.mayPointTo(dstObject, capture) && pta.isUnknown(dstObject) &&
              (model != ExternalModel::InlineFunctionManager ||
               pta.mayPointTo(dstObject, srcObject)),
          "manager retains copy/get-pointer aliases and type-info uncertainty");
  }
}

void managerTransfers() {
  using O = FunctionManagerOperation;
  for (const auto operation :
       {O::TypeInfo, O::Pointer, O::Clone, O::Destroy, O::Unknown}) {
    Andersen pta;
    ExternalEffects models(pta);
    const auto dst = pta.addNode(), dstObject = pta.addNode();
    const auto src = pta.addNode(), srcObject = pta.addNode();
    const auto capture = pta.addNode(), result = pta.addNode();
    pta.address(dst, dstObject);
    pta.address(src, srcObject);
    pta.address(srcObject, capture);
    std::array<CallArgument, 2> args{
        {{dst, ParamKind::Reference}, {src, ParamKind::Reference}}};
    bool unresolved = false;
    ExternalSummary summary;
    models.apply(
        {args, result, std::nullopt, false, 0, unresolved, summary, operation},
        ExternalModel::InlineFunctionManager);
    check(pta.solve() && !unresolved && summary.complete(),
          "operation-specific manager effects converge");
    check(pta.mayPointTo(dstObject, capture) ==
              (operation == O::Clone || operation == O::Unknown),
          "only cloning transfers captured pointer contents");
    check(pta.mayPointTo(dstObject, srcObject) ==
              (operation == O::Pointer || operation == O::Unknown),
          "only pointer lookup transfers the callable storage address");
    check(!pta.isUnknown(srcObject) && !pta.isUnknown(capture),
          "manager operations preserve source storage and captured pointees");
    if (operation == O::Destroy)
      check(pta.constraintCount() == 0 && !pta.isUnknown(dstObject) &&
                summary.has(CallEffect::EndLifetime) &&
                !summary.has(CallEffect::WriteArguments),
            "trivial destruction adds no copy, load, store or havoc");
  }
}

void indirectManagerOperations() {
  const auto result = analyze(R"cpp(
    #include <functional>
    struct F { int *value; void operator()() const {} };
    using Handler = std::_Function_handler<void(), F>;
    void entry(std::_Any_data &dest, const std::_Any_data &source,
               std::_Manager_operation operation) {
      auto manager = &Handler::_M_manager;
      manager(dest, source, std::__destroy_functor);
      manager(dest, source, std::__clone_functor);
      manager(dest, source, std::__get_functor_ptr);
      manager(dest, source, std::__get_type_info);
      manager(dest, source, operation);
    }
  )cpp");
  check(result.calls.size() == 5 && result.statistics.modeledCalls == 5 &&
            result.statistics.unknownCalls == 0 && result.reachable.size() == 1,
        "indirect calls retain their selector after AST release");
  const auto &destroy = result.calls[0].externalEffects;
  check(destroy.has(CallEffect::EndLifetime) &&
            !destroy.has(CallEffect::WriteArguments) &&
            !destroy.has(CallEffect::InputToMemory),
        "a destruction site does not inherit cloning effects");
  const auto &clone = result.calls[1].externalEffects;
  check(clone.has(CallEffect::BeginLifetime) &&
            clone.has(CallEffect::InputToMemory) &&
            !clone.has(CallEffect::EndLifetime),
        "cloning retains its actual storage effects");
  const auto &dynamic = result.calls[4].externalEffects;
  check(dynamic.has(CallEffect::BeginLifetime) &&
            dynamic.has(CallEffect::EndLifetime) &&
            dynamic.has(CallEffect::InputToMemory),
        "a dynamic selector keeps the union of manager effects");
}

void sharedManagerSummary() {
  const auto run = [](unsigned targets) {
    std::string code = "#include <functional>\n";
    for (unsigned i = 0; i < targets; ++i)
      code += "struct F" + std::to_string(i) +
              " { int *value; void operator()() const {} };\n";
    code += R"cpp(
      void entry(unsigned choice, std::_Any_data &dest,
                 const std::_Any_data &source, std::_Manager_operation op) {
        auto manager = &std::_Function_handler<void(), F0>::_M_manager;
    )cpp";
    for (unsigned i = 1; i < targets; ++i)
      code += "if (choice == " + std::to_string(i) +
              ") manager = &std::_Function_handler<void(), F" +
              std::to_string(i) + ">::_M_manager;\n";
    return analyze(code + "manager(dest, source, op); }");
  };
  const auto single = run(1), multiple = run(16);
  check(multiple.calls.size() == 1 &&
            multiple.calls.front().targets.size() == 16 &&
            !multiple.calls.front().unresolved &&
            multiple.statistics.modeledCalls == 16,
        "sharing a summary preserves every discovered call-graph edge");
  check(single.statistics.appliedModelSummaries == 1 &&
            multiple.statistics.appliedModelSummaries == 1,
        "apply a shared model once while retaining all call targets");
}

void distinctSummaries() {
  const auto result = analyze(R"cpp(
    #include <stdlib.h>
    #include <string.h>
    void entry(bool choose, const char *text) {
      auto function = choose ? &strdup : &getenv;
      (void)function(text);
    }
  )cpp");
  check(result.calls.size() == 1 && result.calls.front().targets.size() == 2 &&
            result.statistics.modeledCalls == 2 &&
            result.statistics.appliedModelSummaries == 2,
        "different models at one site retain both targets");
  const auto effects = result.calls.front().externalEffects;
  check(effects.has(CallEffect::Allocate) &&
            effects.has(CallEffect::ReadGlobals),
        "a shared call retains distinct allocation and global-read summaries");
}

void rejectedStorage() {
  for (
      const auto *definition :
      {"struct F { char payload[128]; void operator()() const {} };",
       "struct alignas(64) F { void operator()() const {} };",
       "struct F { F(const F&); void operator()() const {} };",
       "struct F { ~F(); void operator()() const {} };",
       "struct F { void operator()() const {} }; namespace std { template<> struct __is_location_invariant<F> : false_type {}; }"}) {
    const auto result =
        analyze(std::string("#include <functional>\n") + definition + R"cpp(
      using Manager = std::_Function_base::_Base_manager<F>;
      void entry(std::_Any_data &dest, const std::_Any_data &source) {
        Manager::_M_manager(dest, source, std::__clone_functor);
      }
    )cpp");
    for (const auto &call : result.calls)
      for (const auto id : call.targets) {
        const auto model = result.externalModels.find(id);
        check(
            model == result.externalModels.end() ||
                (model->second != ExternalModel::InlineFunctionManager &&
                 model->second != ExternalModel::InlineFunctionCopy),
            "large, aligned, nontrivial and forced-heap functors retain actual effects");
      }
  }
}

void convertedInitializer() {
  const auto result = analyze(R"cpp(
    #include <functional>
    struct F { int value; F(int); void operator()() const {} };
    void entry(std::_Any_data &dest) {
      std::_Function_base::_Base_manager<F>::_M_init_functor(dest, 7);
    }
  )cpp");
  check(std::any_of(result.calls.begin(), result.calls.end(),
                    [&](const auto &call) {
                      return call.unresolved &&
                             std::any_of(call.targets.begin(),
                                         call.targets.end(), [&](auto id) {
                                           return result.metadata.text(
                                                      result.names.at(id)) ==
                                                  "F::F";
                                         });
                    }),
        "converting constructors are not disguised as trivial copies");
}

void callableInvocation() {
  const auto result = analyze(R"cpp(
    #include <functional>
    extern void observe();
    struct F { void operator()() const { observe(); } };
    void entry(const std::_Any_data &source) {
      std::_Function_handler<void(), F>::_M_invoke(source);
    }
  )cpp");
  check(std::any_of(result.calls.begin(), result.calls.end(),
                    [&](const auto &call) {
                      return call.unresolved &&
                             std::any_of(call.targets.begin(),
                                         call.targets.end(), [&](auto id) {
                                           return result.metadata.text(
                                                      result.names.at(id)) ==
                                                  "observe";
                                         });
                    }),
        "ordinary invocation still analyzes the callable and its effects");
}

void forwardingConstructor() {
  const auto result = analyze(R"cpp(
    #include <functional>
    struct F {
      F(const F&) = default;
      template<class T> F(T&&) noexcept;
      void operator()() const {}
    };
    static_assert(std::is_trivially_copyable_v<F>);
    void entry(std::_Any_data &dest, F &source) {
      std::_Function_base::_Base_manager<F>::_M_init_functor(dest, source);
    }
  )cpp");
  check(
      std::any_of(result.calls.begin(), result.calls.end(),
                  [&](const auto &call) {
                    return call.unresolved &&
                           std::any_of(call.targets.begin(), call.targets.end(),
                                       [&](auto id) {
                                         return result.metadata.text(
                                                    result.names.at(id)) ==
                                                "F::F";
                                       });
                  }),
      "a constructor template may override trivial copying of a forwarded lvalue");
}

void heapPointerLookup() {
  for (
      const auto *definition :
      {"struct F { char buffer[128]; ~F(); };",
       "struct F {}; namespace std { template<> struct __is_location_invariant<F> : false_type {}; }"}) {
    const auto result =
        analyze(std::string("#include <functional>\n") + definition + R"cpp(
    struct Manager : std::_Function_base::_Base_manager<F> {
      using std::_Function_base::_Base_manager<F>::_M_get_pointer;
    };
    void entry(const std::_Any_data &source) {
      (void)Manager::_M_get_pointer(source);
    }
  )cpp");
    check(result.calls.size() == 1 && !result.calls.front().unresolved &&
              result.reachable.size() == 1 &&
              result.externalModels.at(*result.calls.front().targets.begin()) ==
                  ExternalModel::FunctionObjectPointer,
          "heap and forced-heap lookup read only the retained pointer");
  }
}

void suppliedSpecialization() {
  const auto result = analyze(R"cpp(
    #include <functional>
    struct F { void operator()() const {} };
    void observe();
    namespace std {
    template<> bool _Function_base::_Base_manager<F>::_M_manager(
        _Any_data &, const _Any_data &, _Manager_operation) {
      observe(); return false;
    }
    }
    void entry(std::_Any_data &dest, const std::_Any_data &source) {
      std::_Function_base::_Base_manager<F>::_M_manager(dest, source, std::__clone_functor);
    }
  )cpp");
  check(
      result.calls.size() == 2 && result.statistics.modeledCalls == 0 &&
          result.statistics.unknownCalls == 1,
      "an explicit supplied specialization retains its body and unknown call");
}

void functionQueries() {
  const auto result = analyze(R"cpp(
    #include <functional>
    struct F { void operator()() const {} };
    void entry(std::function<void()> &function,
               const std::function<void()> &constant, std::_Function_base &base) {
      (void)static_cast<bool>(constant);
      (void)base._M_empty();
      (void)function.target<F>();
      (void)constant.target<F>();
    }
  )cpp");
  check(
      result.calls.size() == 4 && result.statistics.unknownCalls == 0 &&
          result.reachable.size() == 1,
      "empty-state and target queries summarize without invoking the callable");
  for (const auto &call : result.calls)
    check(!call.externalEffects.has(CallEffect::WriteArguments) &&
              !call.externalEffects.has(CallEffect::Callback),
          "public function queries preserve captured storage");
}

void emptyInitialization() {
  const auto result = analyze(R"cpp(
    #include <functional>
    void entry(void *storage) {
      ::new (storage) std::function<void()>();
      ::new (storage) std::function<void()>(nullptr);
    }
  )cpp");
  unsigned count = 0;
  for (const auto &call : result.calls)
    for (const auto target : call.targets) {
      const auto model = result.externalModels.find(target);
      if (model != result.externalModels.end() &&
          model->second == ExternalModel::EmptyFunctionObject) {
        ++count;
        check(!call.unresolved &&
                  !call.externalEffects.has(CallEffect::Allocate) &&
                  call.externalEffects.has(CallEffect::BeginLifetime),
              "empty initialization creates no heap allocation or callback");
      }
    }
  check(count == 2,
        "default and nullptr initialization have reviewed contracts");
}
} // namespace

int main(int argc, char **argv) {
  storageTransfers();
  managerTransfers();
  if (argc == 2 && std::string_view(argv[1]) == "--storage-only")
    return 0;
  inlineManagers();
  indirectManagerOperations();
  sharedManagerSummary();
  distinctSummaries();
  rejectedStorage();
  convertedInitializer();
  forwardingConstructor();
  callableInvocation();
  heapPointerLookup();
  suppliedSpecialization();
  functionQueries();
  emptyInitialization();
}
