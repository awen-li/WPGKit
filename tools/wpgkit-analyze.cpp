#include "CallGraphPTA.h"
#include "InterCFG.h"
#include "DataFlowGraph.h"
#include "Profiling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

using wpgkit::CallGraphPTA;
using wpgkit::Profiler;
using wpgkit::CallGraphResult;

static llvm::json::Object dataFlowReport(const wpgkit::DataFlowGraph &graph) {
  std::map<wpgkit::DataGapKind, std::size_t> gaps;
  for (const auto &gap : graph.gaps())
    ++gaps[gap.kind];
  llvm::json::Array kinds;
  for (const auto &[kind, count] : gaps)
    kinds.push_back(llvm::json::Object{{"kind", static_cast<int64_t>(kind)},
                                       {"count", static_cast<int64_t>(count)}});
  return llvm::json::Object{
      {"node_count", static_cast<int64_t>(graph.nodes().size())},
      {"edge_count", static_cast<int64_t>(graph.edges().size())},
      {"gap_count", static_cast<int64_t>(graph.gaps().size())},
      {"gap_kinds", std::move(kinds)},
      {"memory_flow_sensitive", false},
      {"control_dependencies", false}};
}

static llvm::json::Object profilingReport(const Profiler &profiler) {
  llvm::json::Array components;
  for (const auto &value : profiler.statistics())
    components.push_back(
        llvm::json::Object{{"name", value.component},
                           {"invocations", value.invocations},
                           {"elapsed_ns", value.elapsedNanoseconds},
                           {"rss_available", value.rssAvailable},
                           {"start_rss_bytes", value.startRssBytes},
                           {"end_rss_bytes", value.endRssBytes},
                           {"peak_rss_bytes", value.peakRssBytes}});
  return llvm::json::Object{{"schema_version", 1},
                            {"memory_source", "/proc/self/statm"},
                            {"sample_interval_ms", Profiler::sampleIntervalMs},
                            {"components", std::move(components)}};
}

static llvm::json::Object interCFGReport(const wpgkit::InterCFG &interCFG) {
  std::size_t unknownSummaries = 0;
  for (const auto &summary : interCFG.summaries())
    unknownSummaries += !summary.effects.complete();
  std::map<wpgkit::InterCFGGapKind, std::size_t> gapCounts;
  for (const auto &gap : interCFG.gaps())
    ++gapCounts[gap.kind];
  llvm::json::Array gapReport;
  for (const auto &[kind, count] : gapCounts)
    gapReport.push_back(
        llvm::json::Object{{"kind", static_cast<int64_t>(kind)},
                           {"count", static_cast<int64_t>(count)}});

  return llvm::json::Object{
      {"body_count", static_cast<int64_t>(interCFG.bodyCount())},
      {"call_connections", static_cast<int64_t>(interCFG.connections().size())},
      {"return_connections",
       static_cast<int64_t>(interCFG.connections().size())},
      {"summary_count", static_cast<int64_t>(interCFG.summaries().size())},
      {"unknown_summary_count", static_cast<int64_t>(unknownSummaries)},
      {"gap_count", static_cast<int64_t>(interCFG.gaps().size())},
      {"gap_kinds", std::move(gapReport)},
      {"input_finalized", interCFG.inputFinalized()},
      {"input_closed", interCFG.inputClosed()},
      {"exception_flow_refined", false}};
}

static void writeCalls(llvm::json::OStream &report,
                       const CallGraphResult &result,
                       std::set<wpgkit::FunctionId> &usedFunctions) {
  report.attributeArray("calls", [&] {
    for (const auto &call : result.calls) {
      usedFunctions.insert(call.caller);
      report.object([&] {
        report.attribute("id", static_cast<int64_t>(call.id.value()));
        report.attribute("caller", static_cast<int64_t>(call.caller.value()));
        report.attribute("body", static_cast<int64_t>(call.body.value()));
        report.attribute("unresolved", call.unresolved);
        report.attribute("external_effects",
                         static_cast<int64_t>(call.externalEffects.effects));
        report.attributeArray("targets", [&] {
          for (const auto id : call.targets) {
            usedFunctions.insert(id);
            report.value(static_cast<int64_t>(id.value()));
          }
        });
      });
    }
  });
}

static void writeMetadata(llvm::json::OStream &report,
                          const CallGraphResult &result,
                          const std::set<wpgkit::FunctionId> &usedFunctions) {
  report.attributeArray("function_metadata", [&] {
    for (const auto id : usedFunctions)
      report.object([&] {
        report.attribute("id", static_cast<int64_t>(id.value()));
        report.attribute("usr", result.metadata.text(result.usrs.at(id)));
        report.attribute("name", result.metadata.text(result.names.at(id)));
        const auto model = result.externalModels.find(id);
        report.attribute("external_model",
                         model == result.externalModels.end()
                             ? 0
                             : static_cast<int64_t>(model->second));
      });
  });
  report.attributeArray("call_site_metadata", [&] {
    for (const auto &call : result.calls)
      report.object([&] {
        report.attribute("id", static_cast<int64_t>(call.id.value()));
        report.attribute("location",
                         result.metadata.format(result.locations.at(call.id)));
      });
  });
}

static void writeStatistics(llvm::json::OStream &report,
                            const CallGraphResult &result) {
  report.attribute(
      "analysis_work",
      llvm::json::Object{
          {"activated_body_variants",
           static_cast<int64_t>(result.statistics.activatedBodies)},
          {"connected_call_targets",
           static_cast<int64_t>(result.statistics.connectedCalls)},
          {"pointer_constraints",
           static_cast<int64_t>(result.statistics.pointerConstraints)},
          {"processed_pointer_constraints",
           static_cast<int64_t>(result.statistics.processedPointerConstraints)},
          {"function_worklist_visits",
           static_cast<int64_t>(result.statistics.updatedFunctions)},
          {"indirect_call_visits",
           static_cast<int64_t>(result.statistics.resolvedIndirectCalls)},
          {"virtual_targets_visited",
           static_cast<int64_t>(result.statistics.resolvedVirtualTargets)},
          {"body_state_refreshes",
           static_cast<int64_t>(result.statistics.refreshedBodyStates)},
          {"modeled_call_targets",
           static_cast<int64_t>(result.statistics.modeledCalls)},
          {"applied_model_summaries",
           static_cast<int64_t>(result.statistics.appliedModelSummaries)},
          {"unknown_call_fallbacks",
           static_cast<int64_t>(result.statistics.unknownCalls)}});
  report.attribute(
      "pointer_storage",
      llvm::json::Object{
          {"nodes", static_cast<int64_t>(result.pointerStorage.nodes)},
          {"addressed_objects",
           static_cast<int64_t>(result.pointerStorage.objects)},
          {"target_relations",
           static_cast<int64_t>(result.pointerStorage.targetRelations)},
          {"largest_target_set",
           static_cast<int64_t>(result.pointerStorage.largestTargetSet)},
          {"watchers", static_cast<int64_t>(result.pointerStorage.watchers)},
          {"cfg_local_variables",
           static_cast<int64_t>(result.pointerStorage.cfgLocalVariables)},
          {"cfg_local_fallbacks",
           static_cast<int64_t>(result.pointerStorage.cfgLocalFallbacks)}});
}

static void writeReport(const CallGraphResult &result, const char *scope,
                        const Profiler *profiler) {
  std::set<wpgkit::FunctionId> usedFunctions = result.reachable;
  std::size_t capturedCFGs = 0, unmodeledCFGCalls = 0, unmappedCallFacts = 0;
  for (const auto &cfg : result.cfgs) {
    capturedCFGs += cfg->graph.has_value();
    unmodeledCFGCalls += cfg->unmodeledCalls.size();
    unmappedCallFacts += cfg->unmappedCalls.size();
  }
  // Stream graph records so exporting millions of calls does not retain a
  // second graph of JSON objects and duplicated location strings.
  llvm::json::OStream report(llvm::outs(), 2);
  auto textArray = [&](llvm::StringRef name, const auto &values) {
    report.attributeArray(name, [&] {
      for (const auto value : values)
        report.value(result.metadata.text(value));
    });
  };
  report.object([&] {
    report.attribute("scope", scope);
    report.attribute("linked_program_closed", false);
    report.attribute("closed_within_inputs", result.closedWithinInputs());
    report.attribute("external_effects_finalized", result.finalized);
    report.attribute("data_flow", dataFlowReport(*result.dataFlow));
    report.attribute("inter_cfg", interCFGReport(*result.interCFG));
    report.attribute("mapped_linked_objects",
                     static_cast<int64_t>(result.mappedLinkedObjects));
    report.attribute("analyzed_linked_objects",
                     static_cast<int64_t>(result.analyzedLinkedObjects));
    report.attribute("captured_cfg_count", static_cast<int64_t>(capturedCFGs));
    report.attribute("failed_cfg_count",
                     static_cast<int64_t>(result.cfgs.size() - capturedCFGs));
    report.attribute("unmodeled_cfg_call_count",
                     static_cast<int64_t>(unmodeledCFGCalls));
    report.attribute("unmapped_call_fact_count",
                     static_cast<int64_t>(unmappedCallFacts));
    report.attribute("peak_rss_mib", static_cast<int64_t>(result.peakRssMiB));
    report.attribute("current_rss_mib",
                     static_cast<int64_t>(result.currentRssMiB));
    textArray("link_maps", result.linkMaps);
    textArray("source_files", result.sourceFiles);
    textArray("unresolved_linked_symbols", result.unresolvedLinkedSymbols);
    textArray("unmapped_linked_objects", result.unmappedLinkedObjects);
    textArray("shared_libraries", result.sharedLibraries);
    textArray("limitations", result.limitations);
    report.attributeArray("reachable_functions", [&] {
      for (const auto id : result.reachable)
        report.value(static_cast<int64_t>(id.value()));
    });
    writeCalls(report, result, usedFunctions);
    writeMetadata(report, result, usedFunctions);
    writeStatistics(report, result);
    if (profiler)
      report.attribute("profiling", profilingReport(*profiler));
  });
  llvm::outs() << '\n';
}

int main(int argc, char **argv) {
  bool profile = false;
  while (argc >= 2) {
    if (std::string_view(argv[1]) == "--profile")
      profile = true;
    else
      break;
    --argc;
    ++argv;
  }
  const bool oneMap = argc >= 2 && std::string(argv[1]) == "--link-map";
  const bool manyMaps = argc >= 2 && std::string(argv[1]) == "--link-maps";
  if ((!oneMap && !manyMaps &&
       (argc < 4 || std::string_view(argv[1]).starts_with("--"))) ||
      (oneMap && argc != 5) || (manyMaps && argc < 5)) {
    llvm::errs()
        << "usage: wpgkit-analyze [--profile] COMPDB_DIR QUALIFIED_ROOT SOURCE...\n"
        << "       wpgkit-analyze [--profile] --link-map COMPDB_DIR LINK_MAP QUALIFIED_ROOT\n"
        << "       wpgkit-analyze [--profile] --link-maps COMPDB_DIR QUALIFIED_ROOT LINK_MAP...\n";
    return 2;
  }
  const auto profiler = profile ? std::make_unique<Profiler>() : nullptr;
  auto analyze = [&]() -> llvm::Expected<wpgkit::CallGraphResult> {
    if (oneMap)
      return CallGraphPTA::runLinkMap(argv[3], argv[2], {argv[4]},
                                      profiler.get());
    if (manyMaps)
      return CallGraphPTA::runLinkMaps({argv + 4, argv + argc}, argv[2],
                                       {argv[3]}, profiler.get());
    return CallGraphPTA::runFiles({argv + 3, argv + argc}, argv[1], {argv[2]},
                                  profiler.get());
  };
  auto result = analyze();
  if (!result) {
    llvm::errs() << llvm::toString(result.takeError()) << '\n';
    if (profiler)
      llvm::outs() << llvm::formatv(
          "{0:2}\n", llvm::json::Value(llvm::json::Object{
                         {"analysis_failed", true},
                         {"profiling", profilingReport(*profiler)}}));
    return 1;
  }

  const auto *scope = oneMap     ? "gnu_linker_map"
                      : manyMaps ? "gnu_linker_maps"
                                 : "supplied_source_files";
  writeReport(*result, scope, profiler.get());
  return 0;
}
