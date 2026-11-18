#include "CallGraphPTA.h"
#include "ClangDataFlow.h"
#include "clang/Analysis/CFG.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

using namespace wpgkit;

namespace {
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(R"cpp(
    struct Iterator {
      int n;
      bool operator!=(const Iterator &other) const { return n != other.n; }
      Iterator &operator++() { ++n; return *this; }
      int operator*() const { return n; }
    };
    struct Range {
      Iterator begin() { return {0}; }
      Iterator end() { return {2}; }
    };
    int entry() { Range range; int sum=0; for(auto value: range) sum+=value; return sum; }
  )cpp",
                                               {"-std=c++20"}, "range.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse range fixture");
  unsigned conditions = 0;
  CallGraphSession session(
      nullptr,
      [&](const clang::FunctionDecl &decl, const clang::CFG &cfg, BodyId,
          const ClangDataFlow &data, const LocalDataFlow &) -> CFGFactObserver {
        if (decl.getNameAsString() == "entry")
          for (const auto *block : cfg)
            if (llvm::isa_and_nonnull<clang::CXXForRangeStmt>(
                    block->getTerminatorStmt())) {
              const auto *condition = llvm::dyn_cast_or_null<clang::Expr>(
                  block->getTerminatorCondition());
              check(condition && data.expressionNode(condition) != NoDataIndex,
                    "implicit range condition owns a data node");
              ++conditions;
            }
        return {};
      });
  session.addUnit(ast->getASTContext());
  ast.reset();
  auto result = session.finalize({"entry"});
  check(bool(result), "analyze after AST release");
  check(conditions == 1, "observe one range branch");
  std::set<std::string> calls;
  for (const auto &call : result->calls)
    if (result->metadata.text(result->names.at(call.caller)) == "entry")
      for (auto target : call.targets)
        calls.insert(result->metadata.text(result->names.at(target)));
  for (const auto *name : {"Range::begin", "Range::end", "Iterator::operator!=",
                           "Iterator::operator++", "Iterator::operator*"}) {
    if (!calls.contains(name)) {
      std::cerr << "Missing: " << name << "\nObserved:";
      for (const auto &found : calls)
        std::cerr << " " << found;
      std::cerr << '\n';
    }
    check(calls.contains(name), "implicit iterator call retained");
  }
}
