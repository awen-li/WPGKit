#include <CallGraphPTA.h>
#include <DataFlowGraph.h>
#include <InterCFG.h>
#include <Profiling.h>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: client COMPDB_DIR SOURCE\n";
    return 2;
  }
  wpgkit::Profiler profiler;
  const auto map = std::string{argv[1]} + "/linked.map";
  for (const auto input : {0, 1, 2})
    for (bool sensitive : {false, true}) {
      const wpgkit::AnalysisOptions options{.fieldSensitive = sensitive};
      auto result =
          input == 0
              ? wpgkit::CallGraphPTA::runFiles({argv[2]}, argv[1], {"entry"},
                                               &profiler, {}, {}, options)
          : input == 1
              ? wpgkit::CallGraphPTA::runLinkMap(map, argv[1], {"entry"},
                                                 &profiler, {}, {}, options)
              : wpgkit::CallGraphPTA::runLinkMaps({map}, argv[1], {"entry"},
                                                  &profiler, {}, {}, options);
      if (!result) {
        std::cerr << llvm::toString(result.takeError()) << '\n';
        return 1;
      }
      // These graph queries run after the native AST has been destroyed.
      if (!result->finalized || (input == 0 && !result->closedWithinInputs()) ||
          result->reachable.size() != (sensitive ? 3u : 4u) ||
          result->calls.size() != 2 || !result->interCFG ||
          result->interCFG->connections().size() != (sensitive ? 2u : 3u) ||
          !result->dataFlow || result->dataFlow->edges().empty() ||
          profiler.statistics().empty())
        return 1;
      std::cout << "input=" << input << " field_sensitive=" << sensitive
                << " functions=" << result->reachable.size()
                << " calls=" << result->calls.size()
                << " dfg_edges=" << result->dataFlow->edges().size() << '\n';
    }
}
