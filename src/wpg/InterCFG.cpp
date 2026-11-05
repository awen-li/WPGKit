#include "InterCFG.h"
#include "CapturedCFG.h"
#include <algorithm>
#include <stdexcept>

namespace wpgkit {
namespace {

CFGPosition continuation(CFGPosition source) {
  ++source.offset;
  return source;
}

} // namespace

std::vector<std::pair<CallSiteId, CFGPosition>>
InterCFG::addBody(BodyId id, std::shared_ptr<const CapturedCFG> body) {
  if (!id.isValid() || !body)
    throw std::invalid_argument("invalid inter-CFG body");
  if (bodies_.size() < id.value())
    bodies_.resize(id.value());
  auto &slot = bodies_[id.value() - 1];
  if (slot) {
    if (slot != body)
      throw std::logic_error("inter-CFG body identity changed");
    return {};
  }
  slot = body;
  if (slot->graph)
    ++bodyCount_;
  else
    gaps_.push_back({InterCFGGapKind::MissingCFG, slot->function, id, {}});
  std::vector<std::pair<CallSiteId, CFGPosition>> calls;
  std::vector<std::pair<PointId, CFGPosition>> positions;
  if (body->graph && (!body->calls.empty() || !body->unmodeledCalls.empty()))
    for (const auto &block : body->graph->blocks())
      for (std::size_t offset = 0; offset < block.events.size(); ++offset)
        positions.emplace_back(block.events[offset].point,
                               CFGPosition{id, block.id, offset});
  std::sort(positions.begin(), positions.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  const auto locate = [&](PointId point) -> std::optional<CFGPosition> {
    const auto found = std::lower_bound(
        positions.begin(), positions.end(), point,
        [](const auto &position, auto key) { return position.first < key; });
    return found != positions.end() && found->first == point
               ? std::optional{found->second}
               : std::nullopt;
  };
  for (const auto &mapping : body->calls) {
    if (std::find(body->unevaluatedPoints.begin(),
                  body->unevaluatedPoints.end(),
                  mapping.point) != body->unevaluatedPoints.end())
      continue;
    const auto position = locate(mapping.point);
    if (!position) {
      gaps_.push_back({InterCFGGapKind::InvalidCallMapping, body->function, id,
                       mapping.call});
      continue;
    }
    calls.emplace_back(mapping.call, *position);
  }
  const auto unknown = ExternalSummary::unknown();
  for (const auto point : body->unmodeledCalls) {
    gaps_.push_back({InterCFGGapKind::UnmodeledCall, body->function, id, {}});
    if (const auto position = locate(point)) {
      setSummary(*position, {}, unknown);
      calls.emplace_back(CallSiteId{}, *position);
    }
  }
  for (const auto site : body->unmappedCalls)
    gaps_.push_back({InterCFGGapKind::UnmappedCall, body->function, id, site});
  std::sort(calls.begin(), calls.end(), [](const auto &a, const auto &b) {
    return std::pair{a.second, a.first} < std::pair{b.second, b.first};
  });
  calls.erase(std::unique(calls.begin(), calls.end()), calls.end());
  for (std::size_t i = 1; i < calls.size(); ++i)
    if (calls[i - 1].second == calls[i].second &&
        (i == 1 || calls[i - 2].second != calls[i].second)) {
      gaps_.push_back(
          {InterCFGGapKind::AmbiguousCallOrder, body->function, id, {}});
      setSummary(calls[i].second, {}, unknown);
    }
  std::erase_if(calls, [](const auto &call) { return !call.first.isValid(); });
  return calls;
}

CallConnectionId InterCFG::addConnection(CFGPosition source, CallSiteId site,
                                         BodyId callee) {
  const auto *graph = cfg(source.body);
  const auto *block = graph ? graph->block(source.block) : nullptr;
  if (!block || source.offset >= block->events.size() || !cfg(callee) ||
      !site.isValid())
    throw std::invalid_argument("invalid inter-CFG connection");
  calls_.push_back({source, site, callee});
  return CallConnectionId{calls_.size()};
}

std::optional<ExternalSummary> InterCFG::summary(CFGPosition source,
                                                 CallSiteId site) const {
  const auto key = std::pair{source, site};
  if (const auto pending = pendingSummaries_.find(key);
      pending != pendingSummaries_.end())
    return pending->second;
  return publishedSummary(source, site);
}

std::optional<ExternalSummary>
InterCFG::publishedSummary(CFGPosition source, CallSiteId site) const {
  const auto key = std::pair{source, site};
  const auto found =
      std::lower_bound(summaries_.begin(), summaries_.end(), key,
                       [](const auto &summary, auto key) {
                         return std::pair{summary.source, summary.site} < key;
                       });
  return found != summaries_.end() &&
                 std::pair{found->source, found->site} == key
             ? std::optional{found->effects}
             : std::nullopt;
}

void InterCFG::setSummary(CFGPosition source, CallSiteId site,
                          std::optional<ExternalSummary> effects) {
  const auto key = std::pair{source, site};
  if (publishedSummary(source, site) == effects)
    pendingSummaries_.erase(
        key); // A change reverted before publication is a no-op.
  else
    pendingSummaries_.insert_or_assign(key, effects);
}

void InterCFG::prepare() {
  const auto extendIndex = [&](auto &index, auto less) {
    const auto oldSize = index.size();
    if (oldSize == calls_.size())
      return;
    for (std::size_t i = oldSize; i < calls_.size(); ++i)
      index.push_back(CallConnectionId{i + 1});
    const auto middle = index.begin() + oldSize;
    std::sort(middle, index.end(), less);
    std::inplace_merge(index.begin(), middle, index.end(), less);
  };
  extendIndex(outgoing_, [&](auto a, auto b) {
    return std::pair{connection(a)->source, a} <
           std::pair{connection(b)->source, b};
  });
  extendIndex(returns_, [&](auto a, auto b) {
    return std::pair{connection(a)->callee, a} <
           std::pair{connection(b)->callee, b};
  });
  if (pendingSummaries_.empty())
    return;
  std::vector<CallSummary> merged;
  merged.reserve(summaries_.size() + pendingSummaries_.size());
  auto current = summaries_.begin();
  for (const auto &[key, effects] : pendingSummaries_) {
    while (current != summaries_.end() &&
           std::pair{current->source, current->site} < key)
      merged.push_back(*current++);
    if (current != summaries_.end() &&
        std::pair{current->source, current->site} == key)
      ++current;
    if (effects)
      merged.push_back({key.first, key.second, *effects});
  }
  merged.insert(merged.end(), current, summaries_.end());
  summaries_ = std::move(merged);
  pendingSummaries_.clear();
}

const FunctionCFG *InterCFG::cfg(BodyId body) const noexcept {
  if (!body.isValid() || body.value() > bodies_.size())
    return nullptr;
  const auto &captured = bodies_[body.value() - 1];
  return captured && captured->graph ? &*captured->graph : nullptr;
}

std::optional<CFGPosition> InterCFG::entry(BodyId body) const noexcept {
  const auto *graph = cfg(body);
  return graph ? std::optional<CFGPosition>{{body, graph->entry(), 0}}
               : std::nullopt;
}

std::optional<CFGPosition> InterCFG::exit(BodyId body) const noexcept {
  const auto *graph = cfg(body);
  return graph ? std::optional<CFGPosition>{{body, graph->exit(),
                                             graph->block(graph->exit())
                                                 ->events.size()}}
               : std::nullopt;
}

const CallConnection *InterCFG::connection(CallConnectionId id) const noexcept {
  return id.isValid() && id.value() <= calls_.size() ? &calls_[id.value() - 1]
                                                     : nullptr;
}

std::vector<InterCFGEdge> InterCFG::successors(CFGPosition source) const {
  std::vector<InterCFGEdge> edges;
  const auto *graph = cfg(source.body);
  const auto *block = graph ? graph->block(source.block) : nullptr;
  if (!block || source.offset > block->events.size())
    return edges;
  if (source.offset < block->events.size()) {
    const auto &body = *bodies_[source.body.value() - 1];
    const auto point = block->events[source.offset].point;
    if (std::find(body.unevaluatedPoints.begin(), body.unevaluatedPoints.end(),
                  point) != body.unevaluatedPoints.end()) {
      edges.push_back(
          {continuation(source), InterCFGEdgeKind::Unevaluated, {}, {}, {}});
      return edges;
    }
    auto call = std::lower_bound(
        outgoing_.begin(), outgoing_.end(), source,
        [&](auto id, auto key) { return connection(id)->source < key; });
    for (; call != outgoing_.end() && connection(*call)->source == source;
         ++call)
      edges.push_back({*entry(connection(*call)->callee),
                       InterCFGEdgeKind::Call,
                       *call,
                       connection(*call)->site,
                       {}});
    auto summary = std::lower_bound(
        summaries_.begin(), summaries_.end(), source,
        [](const auto &value, auto key) { return value.source < key; });
    for (; summary != summaries_.end() && summary->source == source; ++summary)
      edges.push_back({continuation(source),
                       InterCFGEdgeKind::Summary,
                       {},
                       summary->site,
                       summary->effects});
    if (edges.empty())
      edges.push_back(
          {continuation(source), InterCFGEdgeKind::Step, {}, {}, {}});
    return edges;
  }
  for (const auto &edge : block->successors)
    if (edge.reachable)
      edges.push_back({{source.body, *edge.reachable, 0},
                       InterCFGEdgeKind::Branch,
                       {},
                       {},
                       {}});
  if (source == exit(source.body)) {
    auto returned = std::lower_bound(
        returns_.begin(), returns_.end(), source.body,
        [&](auto id, auto key) { return connection(id)->callee < key; });
    for (; returned != returns_.end() &&
           connection(*returned)->callee == source.body;
         ++returned) {
      const auto &call = *connection(*returned);
      edges.push_back({continuation(call.source),
                       InterCFGEdgeKind::ReturnOrUnwind,
                       *returned,
                       call.site,
                       {}});
    }
  }
  return edges;
}

} // namespace wpgkit
