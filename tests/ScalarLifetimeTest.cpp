#include "CallGraphPTA.h"
#include "DataFlowGraph.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
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
  auto ast = clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"},
                                                      "scalar-lifetime.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(), "parse fixture");
  CallGraphSession session;
  session.addUnit(ast->getASTContext());
  ast.reset();
  auto result = session.finalize({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError()) << '\n';
    std::exit(1);
  }
  auto repeated = session.finalize({"entry"});
  check(repeated && repeated->statistics == result->statistics &&
            repeated->interCFG == result->interCFG &&
            repeated->dataFlow == result->dataFlow,
        "finalization remains incremental and reuses its graphs");
  return std::move(*result);
}

unsigned lifetimes(const CallGraphResult &result) {
  unsigned count = 0;
  for (const auto &body : result.cfgs) {
    if (!body->graph)
      continue;
    check(body->unmodeledCalls.empty(),
          "lifetime events are not missing calls");
    for (const auto &block : body->graph->blocks())
      for (const auto &event : block.events)
        count += event.kind == CFGEventKind::LifetimeEnds;
  }
  return count;
}

const CallSiteResult &callTo(const CallGraphResult &result, const char *name) {
  for (const auto &call : result.calls)
    for (const auto target : call.targets)
      if (result.metadata.text(result.names.at(target)) == name)
        return call;
  check(false, "expected ordinary call is retained");
  std::abort();
}

bool reachesLifetime(const CallGraphResult &result, DataNodeId start) {
  std::vector<DataNodeId> pending{start};
  std::set<DataNodeId> seen;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (!seen.insert(id).second)
      continue;
    const auto *node = result.dataFlow->node(id);
    if (node && node->position.body.isValid() &&
        node->position.block.isValid()) {
      const auto &position = node->position;
      const auto &body = result.cfgs.at(position.body.value() - 1);
      const auto *block = body->graph->block(position.block);
      if (block && position.offset < block->events.size() &&
          block->events[position.offset].kind == CFGEventKind::LifetimeEnds)
        return true;
    }
    for (const auto &edge : result.dataFlow->outgoing(id))
      pending.push_back(edge.target);
  }
  return false;
}

void scalarOperations() {
  for (const auto *code : {R"cpp(
    using Scalar = int;
    void entry() { Scalar value = 1; value.~Scalar(); }
  )cpp",
                           R"cpp(
    using Scalar = int;
    void entry(Scalar *value) { value->~Scalar(); }
  )cpp",
                           R"cpp(
    using Pointer = int *;
    void entry() { Pointer value = nullptr; value.~Pointer(); }
  )cpp"}) {
    const auto result = analyze(code);
    check(result.calls.empty() && lifetimes(result) == 1,
          "scalar destruction retains one lifetime event without a callee");
    check(result.closedWithinInputs() && !result.unknownMemoryEffects,
          "scalar destruction introduces no unknown function or memory write");
    check(result.dataFlow &&
              std::none_of(result.dataFlow->gaps().begin(),
                           result.dataFlow->gaps().end(),
                           [](const auto &gap) {
                             return gap.kind == DataGapKind::UnknownCall ||
                                    gap.kind == DataGapKind::UnmappedCall;
                           }),
          "DFG has no artificial unknown-call gap");
  }
}

void evaluatedOperands() {
  const auto result = analyze(R"cpp(
    using Scalar = int;
    Scalar *next();
    void entry() { next()->~Scalar(); }
  )cpp");
  check(result.calls.size() == 1 && callTo(result, "next").unresolved &&
            lifetimes(result) == 1,
        "an unknown operand call remains open and executes before destruction");
  const auto &call = result.calls.front();
  const auto callNode = std::find_if(
      result.dataFlow->nodes().begin(), result.dataFlow->nodes().end(),
      [](const auto &node) { return node.kind == DataNodeKind::CallResult; });
  check(callNode != result.dataFlow->nodes().end() && call.body.isValid(),
        "operand evaluation remains represented in the data-flow graph");
  const DataNodeId id{
      static_cast<std::uint64_t>(callNode - result.dataFlow->nodes().begin()) +
      1};
  check(reachesLifetime(result, id),
        "the operand call result feeds the lifetime-end event through the DFG");
}

void templateDestruction() {
  const auto result = analyze(R"cpp(
    template<class T> void destroy(T *value) { value->~T(); }
    struct Record { ~Record() {} };
    void entry(Record *object) {
      int scalar = 0;
      destroy(&scalar);
      destroy(object);
    }
  )cpp");
  check(
      lifetimes(result) == 1 && !callTo(result, "Record::~Record").unresolved,
      "scalar specialization is a lifetime event; class destructor remains a call");
  check(std::all_of(result.calls.begin(), result.calls.end(),
                    [](const auto &call) { return !call.targets.empty(); }),
        "template pseudo-destructors do not create empty indirect target sets");
}

void callbackLifetimeEffect() {
  const auto result = analyze(R"cpp(
    #include <cstdlib>
    using Scalar = int;
    int compare(const void *first, const void *) {
      const_cast<Scalar *>(static_cast<const Scalar *>(first))->~Scalar();
      return 0;
    }
    void entry() {
      Scalar values[2] = {1, 2};
      qsort(values, 2, sizeof(Scalar), &compare);
    }
  )cpp");
  check(callTo(result, "qsort").unresolved && lifetimes(result) == 1,
        "ending an operand lifetime cannot prove a read-only comparator");
  for (const auto function : result.reachable)
    check(result.metadata.text(result.names.at(function)) != "compare",
          "the runtime summary never activates an additional callback body");
}
} // namespace

int main() {
  scalarOperations();
  evaluatedOperands();
  templateDestruction();
  callbackLifetimeEffect();
}
