#include "CallGraphPTA.h"
#include "DataFlowGraph.h"
#include "ExternalEffects.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

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
      code, {"-std=c++17", "-fno-builtin"}, "external-pointer-summary.cpp");
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

void borrowedTraversal() {
  const auto result = analyze(R"cpp(
    #include <link.h>
    #include <unwind.h>
    int modules(dl_phdr_info *info, size_t, void *) { return info->dlpi_phnum; }
    _Unwind_Reason_Code frames(_Unwind_Context *, void *) { return _URC_NO_REASON; }
    void entry() { dl_iterate_phdr(&modules, nullptr); _Unwind_Backtrace(&frames, nullptr); }
  )cpp");
  check(
      result.calls.size() == 2 && result.statistics.modeledCalls == 2 &&
          result.statistics.unknownCalls == 0 && result.reachable.size() == 1,
      "borrowed traversals summarize read-only callbacks without activating bodies");
  for (const auto &call : result.calls)
    check(!call.unresolved && call.externalEffects.complete() &&
              call.externalEffects.has(CallEffect::Callback) &&
              !call.externalEffects.has(CallEffect::WriteArguments),
          "enumeration preserves caller pointer storage");
}

void mutatingTraversal() {
  const auto result = analyze(R"cpp(
    #include <link.h>
    #include <unwind.h>
    int modules(dl_phdr_info *, size_t, void *data) { ++*static_cast<int *>(data); return 0; }
    _Unwind_Reason_Code frames(_Unwind_Context *, void *data) {
      ++*static_cast<int *>(data); return _URC_NO_REASON;
    }
    void entry(int *data) { dl_iterate_phdr(&modules, data); _Unwind_Backtrace(&frames, data); }
  )cpp");
  check(
      result.calls.size() == 2 && result.statistics.unknownCalls == 2 &&
          result.reachable.size() == 1,
      "mutating callbacks keep their effects open without adding callback bodies");
}

void traversalBoundaries() {
  const auto result = analyze(R"cpp(
    # 1 "unsupported-system.h" 1 3
    struct dl_phdr_info; struct _Unwind_Context;
    extern "C" int dl_iterate_phdr(int (*)(dl_phdr_info *, void *, unsigned long), void *);
    extern "C" int _Unwind_Backtrace(int (*)(_Unwind_Context *, void *), void *);
    # 1 "user.cpp" 2
    int modules(dl_phdr_info *, void *, unsigned long) { return 0; }
    int frames(_Unwind_Context *, void *) { return 0; }
    void entry() { dl_iterate_phdr(&modules, nullptr); _Unwind_Backtrace(&frames, nullptr); }
  )cpp");
  check(result.calls.size() == 2 && result.statistics.unknownCalls == 2,
        "incompatible traversal signatures cannot gain a bounded contract");
}

void streamBuffer() {
  const auto result = analyze(R"cpp(
    #include <sstream>
    struct Buffer : std::stringbuf { using std::stringbuf::_M_sync; };
    void entry(Buffer &buffer, char *data) { buffer._M_sync(data, 3, 0); }
  )cpp");
  check(result.calls.size() == 1 && !result.calls.front().unresolved &&
            result.statistics.modeledCalls == 1,
        "string-buffer synchronization has bounded pointer effects");
  const auto &effects = result.calls.front().externalEffects;
  check(
      effects.has(CallEffect::WriteArguments) &&
          !effects.has(CallEffect::Allocate) &&
          !effects.has(CallEffect::Callback),
      "buffer synchronization updates pointers without allocation or callbacks");
}

void bufferPointerEffects() {
  Andersen solver;
  ExternalEffects models(solver);
  const auto receiver = solver.addNode(), object = solver.addNode();
  const auto input = solver.addNode(), buffer = solver.addNode();
  const auto saved = solver.addNode();
  solver.address(receiver, object);
  solver.address(input, buffer);
  solver.address(buffer, saved);
  std::array<CallArgument, 3> args{{{input, ParamKind::Pointer},
                                    {0, ParamKind::Other},
                                    {0, ParamKind::Other}}};
  bool unresolved = false;
  ExternalSummary effects;
  models.apply(
      {args, solver.addNode(), receiver, false, 0, unresolved, effects},
      ExternalModel::ReceiverPointerUpdate);
  check(
      solver.solve() && solver.mayPointTo(object, buffer) &&
          solver.isUnknown(object) && !solver.isUnknown(buffer) &&
          solver.mayPointTo(buffer, saved) && !unresolved && effects.complete(),
      "receiver retains the base/interior aliases without corrupting buffer contents");
}

void returnedStream() {
  const auto result = analyze(R"cpp(
    #include <istream>
    #include <string>
    extern std::istream input;
    extern std::string output;
    void entry() {
      auto *address = reinterpret_cast<void **>(&std::getline(input, output, '\n'));
      *address = nullptr;
    }
  )cpp");
  check(
      result.calls.size() == 1 && result.calls.front().unresolved &&
          result.statistics.modeledCalls == 1,
      "getline preserves its returned stream alias despite callback uncertainty");
  for (const auto &gap : result.dataFlow->gaps()) {
    if (gap.kind != wpgkit::DataGapKind::UnknownAddress ||
        !gap.position.block.isValid())
      continue;
    check(std::any_of(result.dataFlow->nodes().begin(),
                      result.dataFlow->nodes().end(),
                      [&](const auto &node) {
                        return node.kind == wpgkit::DataNodeKind::CallResult &&
                               node.position == gap.position;
                      }),
          "the store through getline's returned reference has a known address");
  }
  for (const auto target : result.calls.front().targets)
    check(result.externalModels.at(target) == ExternalModel::StreamGetline,
          "getline uses its reviewed reference-return contract");
}

class MissingLibraryBodies final
    : public clang::RecursiveASTVisitor<MissingLibraryBodies> {
public:
  bool VisitCallExpr(clang::CallExpr *call) {
    auto *callee = call->getDirectCallee();
    if (!callee ||
        !callee->getASTContext().getSourceManager().isWrittenInMainFile(
            call->getExprLoc()))
      return true;
    // Keep actual libstdc++ declarations/types, simulating a missing compiled
    // definition. This visitor is a catalog test, not an analysis input.
    for (auto *decl : callee->redecls())
      decl->setBody(nullptr);
    models.push_back(ExternalModules{}.query(callee).model);
    return true;
  }

  std::vector<ExternalModel> models;
};

void stringCatalog() {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
    #include <string>
    struct Custom : std::char_traits<char> {};
    void query(const std::string &s) {
      s.size(); s.length(); s.capacity(); s.max_size(); s.empty(); s.data(); s.c_str();
    }
    void update(std::string &s) {
      s.append("a"); s.append("ab", 2); s.append(2, 'a');
      s.assign("a"); s.assign("ab", 2); s.assign(2, 'a');
      s.insert(0, "a"); s.insert(0, "ab", 2); s.insert(0, 2, 'a');
      s.replace(0, 1, "a"); s.replace(0, 1, "ab", 2); s.replace(0, 1, 2, 'a');
    }
    void custom(std::basic_string<char, Custom> &s) { s.append("a"); }
  )cpp",
      {"-std=c++17", "-Wno-unused-result"}, "string-catalog.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse real string prototypes");
  MissingLibraryBodies catalog;
  catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  check(catalog.models.size() == 20, "inspect all concrete string overloads");
  for (unsigned i = 0; i < 7; ++i)
    check(ExternalModules::summary(catalog.models[i]).complete() &&
              !ExternalModules::summary(catalog.models[i])
                   .has(CallEffect::WriteArguments),
          "metadata and buffer accessors preserve pointer storage");
  for (unsigned i = 7; i < 19; ++i)
    check(ExternalModules::contract(catalog.models[i]).guaranteed ==
              RuntimeProperty::ReceiverResult,
          "character-only mutations retain their receiver return alias");
  check(!ExternalModules::summary(catalog.models.back()).complete(),
        "custom traits retain their possible user effects");
}

void streamCatalogAndABI() {
  for (const bool modern : {true, false}) {
    auto ast = clang::tooling::buildASTFromCodeWithArgs(
        R"cpp(
      #include <sstream>
      struct Buffer : std::stringbuf {
        using std::stringbuf::_M_sync; using std::stringbuf::_M_pbump;
        using std::stringbuf::setg; using std::stringbuf::setp;
      };
      void entry(Buffer &buffer, char *data, std::string &text, const std::string &constant) {
        buffer.setg(data, data, data); buffer.setp(data, data);
        buffer._M_pbump(data, data, 0); buffer._M_sync(data, 0, 0);
        text.data(); constant.data(); text.append("a");
      }
    )cpp",
        {"-std=c++17",
         modern ? "-D_GLIBCXX_USE_CXX11_ABI=1" : "-D_GLIBCXX_USE_CXX11_ABI=0"},
        "stream-catalog.cpp");
    check(ast && !ast->getDiagnostics().hasErrorOccurred(),
          "parse stream ABI fixture");
    MissingLibraryBodies catalog;
    catalog.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
    check(catalog.models.size() == 7,
          "inspect all buffer setters and data overloads");
    for (unsigned i = 0; i < 3; ++i)
      check(catalog.models[i] == ExternalModel::ReceiverPointerUpdate,
            "direct get/put pointer setters never change character contents");
    check(
        ExternalModules::summary(catalog.models[3]).complete() == modern &&
            ExternalModules::summary(catalog.models[4]).complete() == modern &&
            ExternalModules::summary(catalog.models[5]).complete(),
        "old COW ABI mutable data and synchronization can detach and allocate");
    check(ExternalModules::contract(catalog.models[6]).guaranteed ==
                  RuntimeProperty::ReceiverResult &&
              !ExternalModules::summary(catalog.models[6]).complete(),
          "both string ABIs return the receiver while allocation stays open");
  }
}
} // namespace

int main() {
  borrowedTraversal();
  mutatingTraversal();
  traversalBoundaries();
  streamBuffer();
  bufferPointerEffects();
  returnedStream();
  stringCatalog();
  streamCatalogAndABI();
}
