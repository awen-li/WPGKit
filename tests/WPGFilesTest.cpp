#include "CallGraphPTA.h"
#include <iostream>

using wpgkit::CallGraphPTA;
using wpgkit::CallGraphResult;

namespace {
bool hasEdge(const CallGraphResult &graph, const std::string &caller,
             const std::string &callee) {
  for (const auto &site : graph.calls) {
    if (graph.metadata.text(graph.names.at(site.caller)) != caller)
      continue;
    for (const auto &target : site.targets)
      if (graph.metadata.text(graph.names.at(target)) == callee)
        return true;
  }
  return false;
}

bool check(bool condition, const char *message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  const std::string directory = argv[1];
  const std::string entry = argv[2];
  const std::string helper = argv[3];
  const std::string mode = argv[4];
  if (mode == "invalid") {
    auto invalid =
        CallGraphPTA::runFiles({entry, helper}, directory, {"entry"});
    if (invalid)
      return check(false, "invalid translation unit accepted") ? 0 : 1;
    llvm::consumeError(invalid.takeError());
    return 0;
  }

  auto graph = CallGraphPTA::runFiles({entry, helper}, directory, {"entry"});
  if (!graph) {
    std::cerr << llvm::toString(graph.takeError()) << '\n';
    return 1;
  }
  if (!check(graph->sourceFiles.size() == 2 &&
                 graph->metadata.text(graph->sourceFiles[0]) == entry &&
                 graph->metadata.text(graph->sourceFiles[1]) == helper,
             "explicit source boundary is retained") ||
      !check(graph->closedWithinInputs(),
             "all calls close within supplied units") ||
      !check(hasEdge(*graph, "entry", "relay") &&
                 hasEdge(*graph, "relay", "target"),
             "cross-file callback edges resolve"))
    return 1;
  // The inline header definition is compiled in both units but stored once.
  if (!check(graph->cfgs.size() == 3,
             "file runner retains one CFG per function"))
    return 1;
  std::set<wpgkit::SourceUnitId> targetUnits;
  for (const auto &body : graph->cfgs)
    if (!check(body->graph && body->graph->block(body->graph->entry()) &&
                   body->graph->block(body->graph->exit()) &&
                   body->unmappedCalls.empty() && body->unmodeledCalls.empty(),
               "file-runner CFG remains usable after AST and session release"))
      return 1;
    else if (graph->metadata.text(graph->names.at(body->function)) == "target")
      targetUnits.insert(body->unit);
  if (!check(targetUnits.size() == 1,
             "shared header CFG retains its representative source unit"))
    return 1;

  auto missingUnit = CallGraphPTA::runFiles({entry}, directory, {"entry"});
  if (!missingUnit) {
    std::cerr << llvm::toString(missingUnit.takeError()) << '\n';
    return 1;
  }
  if (!check(
          !missingUnit->closedWithinInputs() &&
              missingUnit->hasLimitation("call target definition unavailable"),
          "omitted definition leaves an open edge"))
    return 1;

  auto missingCommand = CallGraphPTA::runFiles(
      {entry, directory + "/not-in-database.cpp"}, directory, {"entry"});
  if (!check(!missingCommand, "missing compilation command is rejected"))
    return 1;
  llvm::consumeError(missingCommand.takeError());

  auto duplicate = CallGraphPTA::runFiles({entry, entry}, directory, {"entry"});
  if (!check(!duplicate, "duplicate input is rejected"))
    return 1;
  llvm::consumeError(duplicate.takeError());
  return 0;
}
