#include "ClangCFGLifter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/Error.h"
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

namespace {

class FunctionFinder final : public clang::RecursiveASTVisitor<FunctionFinder> {
public:
  bool VisitFunctionDecl(clang::FunctionDecl *decl) {
    if (decl->getIdentifier() && decl->getName() == "sample" &&
        decl->isThisDeclarationADefinition())
      found = decl;
    return true;
  }

  clang::FunctionDecl *found = nullptr;
};

int fail(const char *message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

bool hasCycle(const wpgkit::FunctionCFG &graph) {
  std::vector<unsigned char> colors(graph.blocks().size());
  std::function<bool(wpgkit::BlockId)> visit = [&](auto id) {
    const auto index = id.value() - 1;
    if (colors[index] == 1)
      return true;
    if (colors[index] == 2)
      return false;
    colors[index] = 1;
    for (const auto &edge : graph.block(id)->successors)
      if (edge.reachable && visit(*edge.reachable))
        return true;
    colors[index] = 2;
    return false;
  };
  return visit(graph.entry());
}

} // namespace

int main() {
  std::optional<wpgkit::FunctionCFG> graph;
  {
    auto ast = clang::tooling::buildASTFromCodeWithArgs(
        "struct Guard { ~Guard(); };"
        "int sample(int n) { Guard g; if (n < 0) return n; int sum = 0;"
        "while (n > 0) { if (n & 1) sum += n; else sum -= n; --n; }"
        "if (false) sum = 42; return sum; }",
        {"-std=c++20"}, "owned_cfg.cpp");
    if (!ast)
      return fail("parse C++ fixture");
    FunctionFinder finder;
    finder.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
    if (!finder.found)
      return fail("find function definition");

    wpgkit::ClangCFGLifter lifter;
    auto invalid =
        lifter.lift(*finder.found, ast->getASTContext(), wpgkit::FunctionId{});
    if (invalid)
      return fail("reject invalid function ID");
    llvm::consumeError(invalid.takeError());

    auto result =
        lifter.lift(*finder.found, ast->getASTContext(), wpgkit::FunctionId{7});
    if (!result) {
      std::cerr << llvm::toString(result.takeError()) << '\n';
      return fail("lift Clang CFG");
    }
    graph.emplace(std::move(*result));
  } // AST and Clang CFG have both been destroyed.

  if (graph->function() != wpgkit::FunctionId{7} ||
      !graph->block(graph->entry()) || !graph->block(graph->exit()) ||
      graph->entry() == graph->exit() || graph->blocks().size() < 5)
    return fail("retain entry, exit and block identity after AST release");
  if (graph->block(wpgkit::BlockId{}) ||
      graph->block(wpgkit::BlockId{graph->blocks().size() + 1}))
    return fail("reject missing block IDs");

  bool branch = false, pruned = false;
  unsigned destructors = 0, exitEdges = 0;
  std::set<std::uint64_t> points;
  for (const auto &block : graph->blocks()) {
    if (graph->block(block.id) != &block)
      return fail("block lookup must preserve ID mapping");
    if (block.terminator && block.successors.size() >= 2)
      branch = true;
    for (const auto &event : block.events) {
      if (event.point.function() != graph->function() ||
          !points.insert(event.point.position()).second)
        return fail("events need unique owned point IDs");
      if (event.kind == wpgkit::CFGEventKind::AutomaticObjectDtor)
        ++destructors;
    }
    for (const auto &edge : block.successors) {
      if (edge.reachable && !graph->block(*edge.reachable))
        return fail("reachable successor target missing");
      if (edge.pruned && !graph->block(*edge.pruned))
        return fail("pruned successor target missing");
      if (edge.pruned)
        pruned = true;
      if (edge.reachable == graph->exit())
        ++exitEdges;
    }
  }
  if (!branch || !hasCycle(*graph) || destructors < 2 || exitEdges < 2 ||
      !pruned)
    return fail(
        "retain branches, loop, early returns, destructors, and pruned edge");
  return 0;
}
