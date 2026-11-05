#include <CallGraphPTA.h>
#include <DataFlowGraph.h>
#include <InterCFG.h>
#include <Profiling.h>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: client COMPDB_DIR SOURCE\n";
    return 2;
  }
  wpgkit::Profiler profiler;
  auto result =
      wpgkit::CallGraphPTA::runFiles({argv[2]}, argv[1], {"entry"}, &profiler);
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    return 1;
  }
  // These graph queries run after the native AST has been destroyed.
  if (!result->finalized || !result->closedWithinInputs() ||
      result->reachable.size() != 3 || result->calls.size() != 2 ||
      !result->interCFG || result->interCFG->connections().size() != 2 ||
      !result->dataFlow || result->dataFlow->edges().empty() ||
      profiler.statistics().empty())
    return 1;
  std::cout << "functions=" << result->reachable.size()
            << " calls=" << result->calls.size()
            << " dfg_edges=" << result->dataFlow->edges().size() << '\n';
}
