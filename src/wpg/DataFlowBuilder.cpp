#include "DataFlowBuilder.h"
#include "InterCFG.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace wpgkit {
namespace {

class Builder final {
public:
  Builder(const CallGraphResult &program, const Andersen &pta)
      : program(program), pta(pta) {}

  std::shared_ptr<const DataFlowGraph>
  build(std::span<const DataFlowInput> functions, bool consume) {
    std::size_t nodes = 0, edges = 0;
    std::size_t extraNodes = program.calls.size() + 3;
    if (program.interCFG)
      extraNodes += program.interCFG->gaps().size();
    for (const auto &input : functions)
      if (input.facts) {
        const auto *cfg = program.cfg(input.body);
        if (cfg && program.reachable.contains(cfg->function) &&
            !program.externalModels.contains(cfg->function)) {
          nodes += input.facts->nodes.size();
          edges += input.facts->values.size() + input.facts->uses.size() +
                   input.facts->memory.size();
          extraNodes += input.facts->memory.size();
          for (const auto &call : input.facts->calls)
            extraNodes += call.pointers.size() + 1;
        } else if (consume)
          *input.facts = {};
      }
    graph->reserve(nodes + extraNodes, edges);
    for (const auto &input : functions) {
      addFunction(input);
      if (consume && input.facts)
        releaseLocalFacts(*input.facts);
    }
    connectCalls();
    for (const auto &site : program.calls)
      summarize(site);
    if (program.unknownMemoryEffects)
      unknown({}, {}, DataGapKind::UnsupportedEffect);
    bodies.clear();
    calls.clear();
    incomplete.clear();
    if (consume)
      for (const auto &input : functions)
        if (input.facts)
          *input.facts = {};
    connectMemory();
    std::vector<Memory>().swap(memory);
    graph->prepare();
    return graph;
  }

private:
  static void releaseLocalFacts(FunctionDataFlow &facts) {
    std::vector<DataNode>().swap(facts.nodes);
    decltype(facts.values)().swap(facts.values);
    decltype(facts.uses)().swap(facts.uses);
    decltype(facts.memory)().swap(facts.memory);
    decltype(facts.gaps)().swap(facts.gaps);
  }

  void unknown(CFGPosition position, CallSiteId site,
               DataGapKind kind = DataGapKind::UnmappedCall) {
    const auto node = graph->addNode({position, DataNodeKind::Unknown, 0});
    graph->addGap({position, kind, site});
    memory.push_back({node, NoDataIndex, true, false});
    memory.push_back({node, NoDataIndex, true, true});
  }

  void addFunction(const DataFlowInput &input) {
    const auto *captured = program.cfg(input.body);
    if (!captured || !program.reachable.contains(captured->function) ||
        program.externalModels.contains(captured->function))
      return;
    if (!captured->graph || !input.facts) {
      graph->addGap({{input.body, {}, 0}, DataGapKind::MissingCFG, {}});
      return;
    }
    const auto &facts = *input.facts;
    const auto base = graph->nodes().size() + 1;
    bodies.emplace(input.body, Body{&facts, base});
    graph->addBody(input.body, facts.nodes);
    for (const auto &[source, target] : facts.values)
      graph->addEdge(DataNodeId{base + source}, DataNodeId{base + target},
                     DataEdgeKind::Value);
    for (const auto &gap : facts.gaps)
      graph->addGap(gap);
    for (const auto &access : facts.memory)
      memory.push_back({DataNodeId{base + access.node}, access.pointer,
                        access.indirect, access.write});
    for (const auto &call : facts.calls)
      calls.emplace(call.site, Call{&call, base});
    for (const auto &[source, target] : facts.uses)
      graph->addEdge(DataNodeId{base + source}, DataNodeId{base + target},
                     DataEdgeKind::DefUse);
  }

  void connectCalls() {
    if (program.interCFG) {
      for (std::size_t i = 0; i < program.interCFG->connections().size(); ++i) {
        const auto &connection = program.interCFG->connections()[i];
        const auto call = calls.find(connection.site);
        const auto callee = bodies.find(connection.callee);
        if (call == calls.end() || callee == bodies.end()) {
          unknown(connection.source, connection.site);
          incomplete.insert(connection.site);
          continue;
        }
        const auto &[actual, callerBase] = call->second;
        const auto &[formal, calleeBase] = callee->second;
        if (formal->receiver != NoDataIndex) {
          if (actual->receiverValue == NoDataIndex) {
            unknown(connection.source, connection.site);
            incomplete.insert(connection.site);
          } else {
            graph->addEdge(DataNodeId{callerBase + actual->receiverValue},
                           DataNodeId{calleeBase + formal->receiver},
                           DataEdgeKind::ActualFormal, CallConnectionId{i + 1});
          }
        }
        if (actual->arguments.size() != formal->parameters.size())
          graph->addGap({connection.source, DataGapKind::ArgumentMismatch,
                         connection.site});
        for (std::size_t p = 0;
             p < std::min(actual->arguments.size(), formal->parameters.size());
             ++p)
          graph->addEdge(DataNodeId{callerBase + actual->arguments[p]},
                         DataNodeId{calleeBase + formal->parameters[p]},
                         DataEdgeKind::ActualFormal, CallConnectionId{i + 1});
        for (const auto returned : formal->returns)
          graph->addEdge(DataNodeId{calleeBase + returned},
                         DataNodeId{callerBase + actual->result},
                         DataEdgeKind::Return, CallConnectionId{i + 1});
      }
      for (const auto &gap : program.interCFG->gaps())
        unknown({gap.body, {}, 0}, gap.site);
    }
  }

  void summarize(const CallSiteResult &site) {
    const auto found = calls.find(site.id);
    if (found == calls.end()) {
      unknown({site.body, {}, 0}, site.id);
      return;
    }
    const auto &[call, base] = found->second;
    const DataNodeId result{base + call->result};
    auto effects = site.externalEffects;
    if (site.unresolved || incomplete.contains(site.id)) {
      effects.effects |= ExternalModules::query().summary.effects;
    }
    if (site.unresolved) {
      graph->addGap(
          {graph->node(result)->position, DataGapKind::UnknownCall, site.id});
    }
    // Resource effects stay in the owned call summary. Do not silently claim
    // that ordinary value/memory edges model streams, errno or the FP state.
    if (effects.has(CallEffect::ReadExternalState) ||
        effects.has(CallEffect::WriteExternalState) ||
        effects.has(CallEffect::WriteStream) ||
        effects.has(CallEffect::WriteErrno) ||
        effects.has(CallEffect::FloatingEnvironment) ||
        effects.has(CallEffect::Synchronize))
      graph->addGap(
          {graph->node(result)->position, DataGapKind::ExternalState, site.id});
    for (std::size_t a = 0; a < call->arguments.size(); ++a)
      summarizeInput(DataNodeId{base + call->arguments[a]},
                     a < call->pointers.size() ? call->pointers[a]
                                               : NoDataIndex,
                     result, effects);
    if (effects.has(CallEffect::ReadGlobals))
      memory.push_back({result, NoDataIndex, true, false});
    if (effects.has(CallEffect::WriteGlobals))
      memory.push_back({result, NoDataIndex, true, true});
    summarizeInput(call->receiverValue != NoDataIndex
                       ? DataNodeId{base + call->receiverValue}
                       : DataNodeId{},
                   call->receiver, result, effects);
  }

  void summarizeInput(DataNodeId input, std::uint32_t pointer,
                      DataNodeId result, ExternalSummary effects) {
    if (input.isValid() && (effects.has(CallEffect::InputToReturn) ||
                            effects.has(CallEffect::InputToMemory)))
      graph->addEdge(input, result, DataEdgeKind::Summary);
    if (pointer != NoDataIndex) {
      if (effects.has(CallEffect::ReadArguments))
        memory.push_back({result, pointer, true, false});
      if (effects.has(CallEffect::WriteArguments))
        memory.push_back({result, pointer, true, true});
    }
  }

  struct MemoryPartition {
    std::vector<std::uint32_t> parents;
    std::map<std::uint32_t, std::uint32_t> pointees;
    std::set<std::uint32_t> singletonPointers, summaries;

    std::uint32_t root(std::uint32_t node) {
      auto result = node;
      while (parents.at(result) != result)
        result = parents[result];
      while (node != result) {
        const auto next = parents[node];
        parents[node] = result;
        node = next;
      }
      return result;
    }
  };

  MemoryPartition partitionMemory() const {
    MemoryPartition partition;
    partition.parents.resize(pta.nodeCount());
    std::iota(partition.parents.begin(), partition.parents.end(), 0);
    for (const auto &access : memory) {
      if (!access.indirect || access.pointer == NoDataIndex ||
          partition.pointees.contains(access.pointer))
        continue;
      auto representative = NoDataIndex;
      std::size_t count = 0;
      if (!pta.isUnknown(access.pointer))
        for (const auto object : pta.pointsTo(access.pointer)) {
          ++count;
          if (representative == NoDataIndex)
            representative = object;
          else
            partition.parents[partition.root(object)] =
                partition.root(representative);
        }
      if (count == 1)
        partition.singletonPointers.insert(access.pointer);
      partition.pointees.emplace(access.pointer, representative);
    }
    for (std::uint32_t object = 0; object < partition.parents.size(); ++object)
      if (partition.parents[object] != object)
        partition.summaries.insert(partition.root(object));
    return partition;
  }

  void connectMemory() {
    // Preserve compact dependency partitions and singleton locations
    // separately.
    auto partition = partitionMemory();
    std::map<std::uint32_t, DataNodeId> regions;
    std::map<std::uint32_t, DataNodeId> exact;
    DataNodeId unknownRead, unknownWrite;
    for (const auto &access : memory) {
      auto object = access.pointer;
      if (object != NoDataIndex && access.indirect)
        object = partition.pointees.at(object);
      DataNodeId region;
      if (object == NoDataIndex) {
        auto &unknown = access.write ? unknownWrite : unknownRead;
        if (!unknown.isValid())
          unknown = graph->addNode({{}, DataNodeKind::Unknown, 0});
        region = unknown;
        graph->addGap({graph->node(access.node)->position,
                       DataGapKind::UnknownAddress,
                       {}});
      } else {
        auto [found, added] = regions.try_emplace(partition.root(object));
        if (added)
          found->second = graph->addNode(
              {{}, DataNodeKind::Memory, partition.root(object)});
        region = found->second;
        if (!access.indirect ||
            partition.singletonPointers.contains(access.pointer)) {
          auto [cell, added] = exact.try_emplace(object);
          if (added) {
            cell->second =
                !partition.summaries.contains(partition.root(object))
                    ? region
                    : graph->addNode({{}, DataNodeKind::Memory, object});
            if (cell->second != region)
              graph->addEdge(region, cell->second, DataEdgeKind::Location);
          }
          graph->addEdge(access.write ? access.node : cell->second,
                         access.write ? cell->second : access.node,
                         DataEdgeKind::Location);
        }
      }
      graph->addEdge(access.write ? access.node : region,
                     access.write ? region : access.node, DataEdgeKind::Memory);
    }
    for (const auto &[object, node] : regions) {
      if (unknownRead.isValid())
        graph->addEdge(node, unknownRead, DataEdgeKind::Memory);
      if (unknownWrite.isValid())
        graph->addEdge(unknownWrite, node, DataEdgeKind::Memory);
    }
    if (unknownRead.isValid() && unknownWrite.isValid())
      graph->addEdge(unknownWrite, unknownRead, DataEdgeKind::Memory);
  }

  const CallGraphResult &program;
  const Andersen &pta;
  std::shared_ptr<DataFlowGraph> graph = std::make_shared<DataFlowGraph>();

  struct Body {
    const FunctionDataFlow *facts;
    std::uint64_t base;
  };

  std::map<BodyId, Body> bodies;

  struct Call {
    const DataCall *facts;
    std::uint64_t base;
  };

  std::map<CallSiteId, Call> calls;
  std::set<CallSiteId> incomplete;

  struct Memory {
    DataNodeId node;
    std::uint32_t pointer;
    bool indirect, write;
  };

  std::vector<Memory> memory;
};
} // namespace

std::shared_ptr<const DataFlowGraph>
DataFlowBuilder::build(const CallGraphResult &program,
                       std::span<const DataFlowInput> functions,
                       const Andersen &pta, bool consume) {
  return Builder(program, pta).build(functions, consume);
}

} // namespace wpgkit
