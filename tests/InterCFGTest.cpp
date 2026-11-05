#include "InterCFG.h"
#include "CallGraphPTA.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>
#include <type_traits>

using namespace wpgkit;

static_assert(std::is_trivially_copyable_v<CFGPosition>);
static_assert(std::is_trivially_copyable_v<CallConnection>);
static_assert(std::is_trivially_copyable_v<CallSummary>);
static_assert(std::is_trivially_copyable_v<InterCFGEdge>);

namespace {
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void add(CallGraphSession &session, const char *code, const char *file) {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"}, file);
  check(ast != nullptr, "parse inter-CFG fixture");
  session.addUnit(ast->getASTContext());
} // No AST survives extraction.

CallGraphResult finish(CallGraphSession &session) {
  auto result = session.finalize({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError());
    std::exit(1);
  }
  return std::move(*result);
}

BodyId body(const CallGraphResult &input, const char *name) {
  for (std::size_t i = 0; i < input.cfgs.size(); ++i)
    if (input.metadata.text(input.names.at(input.cfgs[i]->function)) == name)
      return BodyId{i + 1};
  return {};
}

bool hasGap(const InterCFG &graph, InterCFGGapKind kind) {
  return std::any_of(graph.gaps().begin(), graph.gaps().end(),
                     [&](const auto &gap) { return gap.kind == kind; });
}

// Walk the actual overlay with a bounded call stack, matching returns. Bounds
// keep this test finite for recursion; the production graph itself is
// unbounded.
std::set<CFGPosition> walk(const InterCFG &graph, CFGPosition start) {
  using State = std::pair<CFGPosition, std::vector<CallConnectionId>>;
  std::queue<State> work;
  std::set<State> visited;
  std::set<CFGPosition> positions;
  work.push({start, {}});
  while (!work.empty()) {
    auto state = std::move(work.front());
    work.pop();
    if (!visited.insert(state).second)
      continue;
    check(visited.size() < 100000, "bounded inter-CFG traversal terminates");
    positions.insert(state.first);
    for (const auto &edge : graph.successors(state.first)) {
      auto stack = state.second;
      if (edge.kind == InterCFGEdgeKind::Call) {
        if (stack.size() == 5)
          continue;
        stack.push_back(edge.connection);
      } else if (edge.kind == InterCFGEdgeKind::ReturnOrUnwind) {
        if (stack.empty() || stack.back() != edge.connection)
          continue;
        stack.pop_back();
      }
      work.push({edge.target, std::move(stack)});
    }
  }
  return positions;
}
} // namespace

int main() {
  CallGraphResult input;
  {
    CallGraphSession session;
    add(session,
        "void leaf() noexcept; void second() noexcept; void recur(int);"
        "void alpha() { leaf(); leaf(); } void beta() { leaf(); }"
        "void dead() { leaf(); }"
        "void entry(bool b) { alpha(); beta();"
        "void (*f)() noexcept = b ? leaf : second; f(); recur(2); }",
        "caller.cpp");
    add(session,
        "void leaf() noexcept {} void second() noexcept {}"
        "void recur(int n) { if (n) recur(n-1); }",
        "callee.cpp");
    input = finish(session);
  } // Session, ASTs and solver have been destroyed before graph queries.
  auto snapshot = input.interCFG;
  const auto &graph = *snapshot;
  check(graph.inputClosed() && graph.inputFinalized() && graph.gaps().empty(),
        "supported finalized calls have no extraction gaps");
  check(graph.bodyCount() == 6 && graph.connections().size() == 9 &&
            graph.summaries().empty(),
        "direct, multi-target indirect and recursive calls are all connected");
  const auto entryBody = body(input, "entry"), leafBody = body(input, "leaf");
  const auto secondBody = body(input, "second"),
             recurBody = body(input, "recur");
  check(!graph.cfg(body(input, "dead")) && !graph.entry({}) &&
            graph.successors({entryBody, BlockId{99999}, 0}).empty(),
        "inactive and invalid graph positions do not produce edges");
  check(graph.cfg(entryBody) == &*input.cfg(entryBody)->graph,
        "reuse the exact immutable CFG allocation");
  check(!graph.connection({}) && !graph.connection(CallConnectionId{99999}),
        "reject invalid call-connection identities");
  unsigned leafCallers = 0, multiTargets = 0;
  for (std::size_t i = 0; i < graph.connections().size(); ++i) {
    const auto &call = graph.connections()[i];
    const CallConnectionId id{i + 1};
    const auto edges = graph.successors(call.source);
    check(std::none_of(edges.begin(), edges.end(),
                       [](const auto &edge) {
                         return edge.kind == InterCFGEdgeKind::Step;
                       }),
          "known call cannot bypass its callee through an ordinary step");
    check(std::any_of(edges.begin(), edges.end(),
                      [&](const auto &edge) {
                        return edge.kind == InterCFGEdgeKind::Call &&
                               edge.connection == id &&
                               edge.target == graph.entry(call.callee);
                      }),
          "call edge enters the selected body");
    const auto returns = graph.successors(*graph.exit(call.callee));
    auto resume = call.source;
    ++resume.offset;
    check(std::count_if(returns.begin(), returns.end(),
                        [&](const auto &edge) {
                          return edge.kind ==
                                     InterCFGEdgeKind::ReturnOrUnwind &&
                                 edge.connection == id &&
                                 edge.site == call.site &&
                                 edge.target == resume;
                        }) == 1,
          "every target returns to its own occurrence, including recursion");
    if (call.callee == leafBody)
      ++leafCallers;
    if (call.source.body == entryBody &&
        (call.callee == leafBody || call.callee == secondBody)) {
      check(edges.size() == 2,
            "one indirect occurrence enters both possible callees");
      ++multiTargets;
    }
  }
  check(leafCallers == 4 && multiTargets == 2,
        "several callers share one callee CFG");
  const auto alphaReachable = walk(graph, *graph.entry(body(input, "alpha")));
  check(std::none_of(alphaReachable.begin(), alphaReachable.end(),
                     [&](const auto &position) {
                       return position.body == body(input, "beta") ||
                              position.body == entryBody;
                     }),
        "balanced returns cannot jump into another caller's continuation");
  {
    auto failedLeaf = std::make_shared<CapturedCFG>(*input.cfg(leafBody));
    failedLeaf->graph.reset();
    InterCFG missingGraph;
    missingGraph.addBody(leafBody, failedLeaf);
    check(hasGap(missingGraph, InterCFGGapKind::MissingCFG) &&
              !missingGraph.entry(leafBody),
          "a failed callee CFG has no entry and remains an explicit gap");
  }
  {
    InterCFG summaries;
    const auto source = graph.connections().front().source;
    summaries.addBody(source.body, input.cfgs[source.body.value() - 1]);
    const CallSiteId first{1}, second{2};
    const auto unknown = ExternalModules::summary(ExternalModel::Unknown);
    const ExternalSummary empty{};
    summaries.setSummary(source, second, unknown);
    summaries.setSummary(source, first, empty);
    check(summaries.summary(source, second) == unknown,
          "pending summary is queryable before prepare");
    summaries.prepare();
    check(summaries.summaries().size() == 2 &&
              summaries.summaries().front().site == first,
          "out-of-order summary insertions publish in query order");
    const auto *storage = summaries.summaries().data();
    summaries.setSummary(source, first, unknown);
    summaries.setSummary(source, first, empty);
    summaries.setSummary(source, second, unknown);
    summaries.setSummary(source, CallSiteId{99}, std::nullopt);
    summaries.prepare();
    check(
        summaries.summaries().data() == storage &&
            summaries.summaries().size() == 2,
        "unchanged, absent and reverted updates do not reconstruct summary storage");
    const auto snapshot = summaries;
    summaries.setSummary(source, first, std::nullopt);
    summaries.setSummary(source, second, empty);
    summaries.prepare();
    check(summaries.summaries().size() == 1 &&
              !summaries.summary(source, first) &&
              summaries.summary(source, second) == empty &&
              snapshot.summary(source, first) == empty &&
              snapshot.summary(source, second) == unknown,
          "sparse removal and replacement preserve other snapshots");
    summaries.setSummary(source, second, std::nullopt);
    summaries.prepare();
    check(summaries.summaries().empty(),
          "the final pending removal empties the summaries");
  }
  input = {}; // The overlay owns its graph references independently of reports.
  const auto reached = walk(graph, *graph.entry(entryBody));
  check(
      reached.contains(*graph.exit(entryBody)) &&
          reached.contains(*graph.entry(recurBody)) &&
          reached.contains(*graph.entry(leafBody)) &&
          reached.contains(*graph.entry(secondBody)),
      "post-release balanced traversal crosses files, recursion and both targets");

  CallGraphSession effects;
  add(effects,
      "extern int external(int *); void known() noexcept {}"
      "namespace std { template<class T> T&& move(T&) noexcept; }"
      "void entry(bool b, void (*other)()) { int x = 0; external(&x);"
      "void (*f)() = b ? &known : other; f(); (void)std::move(x); }",
      "effects.cpp");
  const auto effectsInput = finish(effects);
  const auto &effectsGraph = *effectsInput.interCFG;
  bool unknown = false, modeled = false, mixed = false;
  for (const auto &summary : effectsGraph.summaries()) {
    if (summary.effects.has(CallEffect::Unknown)) {
      unknown = true;
      check(
          summary.effects.has(CallEffect::InputToReturn) &&
              summary.effects.has(CallEffect::WriteArguments) &&
              summary.effects.has(CallEffect::WriteGlobals) &&
              summary.effects.has(CallEffect::Throw),
          "unknown summaries retain conservative input/output and exception effects");
    } else
      modeled = true;
    const auto edges = effectsGraph.successors(summary.source);
    mixed |= std::any_of(edges.begin(), edges.end(), [](const auto &edge) {
      return edge.kind == InterCFGEdgeKind::Call;
    });
    check(
        std::any_of(edges.begin(), edges.end(),
                    [&](const auto &edge) {
                      return edge.kind == InterCFGEdgeKind::Summary &&
                             edge.target.offset == summary.source.offset + 1;
                    }),
        "may-not-return unknown effects must still retain a normal continuation");
  }
  check(unknown && modeled && mixed,
        "unknown, intrinsic and known-plus-unknown targets coexist");

  CallGraphSession lifetime;
  add(lifetime,
      "struct Guard { ~Guard() noexcept {} }; int leaf() { return 1; }"
      "void catch_path() noexcept {} void after() noexcept {}"
      "void entry(bool b) { Guard g; try { leaf(); } catch (...) { catch_path(); }"
      "if (b) return; (void)noexcept(leaf()); while (b) { after(); b=false; } }",
      "lifetime.cpp");
  const auto lifetimeInput = finish(lifetime);
  const auto &lifetimeGraph = *lifetimeInput.interCFG;
  const auto lifetimeEntry = body(lifetimeInput, "entry");
  const auto lifetimeReached =
      walk(lifetimeGraph, *lifetimeGraph.entry(lifetimeEntry));
  check(lifetimeReached.contains(
            *lifetimeGraph.entry(body(lifetimeInput, "catch_path"))),
        "native exceptional branch remains reachable after the call");
  std::map<CallSiteId, std::set<CFGPosition>> destructorOccurrences;
  bool skipped = false;
  for (const auto &call : lifetimeGraph.connections())
    if (call.callee == body(lifetimeInput, "Guard::~Guard"))
      destructorOccurrences[call.site].insert(call.source);
  for (const auto &position : lifetimeReached)
    for (const auto &edge : lifetimeGraph.successors(position))
      skipped |= edge.kind == InterCFGEdgeKind::Unevaluated;
  check(skipped, "Clang statements from noexcept are effect-free steps");
  check(std::any_of(destructorOccurrences.begin(), destructorOccurrences.end(),
                    [](const auto &entry) { return entry.second.size() >= 2; }),
        "one destructor call fact has distinct matched return occurrences");

  // Extraction gaps are rare on small fixtures: construct them explicitly from
  // an owned snapshot, without making the production builder reparse anything.
  const auto index = lifetimeEntry.value() - 1;
  auto changed = std::make_shared<CapturedCFG>(*lifetimeInput.cfgs[index]);
  check(!changed->calls.empty(), "gap fixture has a mapped call");
  const auto missing = changed->calls.front();
  std::erase_if(changed->calls, [&](const auto &mapping) {
    return mapping.call == missing.call;
  });
  changed->unmodeledCalls.push_back(missing.point);
  changed->unmappedCalls.push_back(missing.call);
  InterCFG gaps;
  gaps.addBody(lifetimeEntry, changed);
  gaps.prepare();
  check(hasGap(gaps, InterCFGGapKind::UnmodeledCall) &&
            hasGap(gaps, InterCFGGapKind::UnmappedCall),
        "preserve both directions of call-mapping gaps");
  check(std::any_of(gaps.summaries().begin(), gaps.summaries().end(),
                    [](const auto &summary) {
                      return !summary.site.isValid() &&
                             summary.effects.has(CallEffect::Unknown);
                    }),
        "unmodeled CFG calls use unknown summaries instead of plain steps");
  auto failedBody = std::make_shared<CapturedCFG>(*changed);
  failedBody->graph.reset();
  InterCFG failed;
  failed.addBody(lifetimeEntry, failedBody);
  check(hasGap(failed, InterCFGGapKind::MissingCFG) &&
            !failed.entry(lifetimeEntry),
        "failed reachable CFGs remain explicit gaps");

  auto overlap = effectsInput;
  const auto overlapBody = body(overlap, "entry");
  auto overlapping = std::make_shared<CapturedCFG>(*overlap.cfg(overlapBody));
  check(overlapping->calls.size() > 1, "overlap fixture has multiple facts");
  overlapping->calls[1].point = overlapping->calls[0].point;
  InterCFG ambiguous;
  ambiguous.addBody(overlapBody, overlapping);
  check(hasGap(ambiguous, InterCFGGapKind::AmbiguousCallOrder),
        "multiple facts at one event do not invent an evaluation order");

  CallGraphSession terminating;
  add(terminating,
      "[[noreturn]] void halt() noexcept; void after() noexcept {}"
      "void entry() { halt(); after(); }",
      "noreturn.cpp");
  const auto terminatingInput = finish(terminating);
  const auto &terminatingGraph = *terminatingInput.interCFG;
  const auto terminatingReached =
      walk(terminatingGraph,
           *terminatingGraph.entry(body(terminatingInput, "entry")));
  check(!terminatingReached.contains(
            *terminatingGraph.entry(body(terminatingInput, "after"))),
        "native pruned continuation after a noreturn call stays unreachable");
  return 0;
}
