#include "Andersen.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wpgkit {

Andersen::Node Andersen::addNode() {
  // Sparse bitmap indices are 32-bit. Fail explicitly before an ID can wrap.
  if (nodes_.size() == std::numeric_limits<Node>::max())
    throw std::overflow_error("Andersen node ID space exhausted");
  nodes_.emplace_back();
  users_.emplace_back();
  changedFlags_.push_back(false);
  return nodes_.size() - 1;
}

void Andersen::address(Node destination, Node object) {
  // Taking an address exposes the object's contents, not the address value.
  markExternallyAffected(object);
  if (capturing_) {
    captured_.push_back({FactKind::Address, destination, object});
    return;
  }
  auto &index = nodes_.at(object).objectIndex;
  if (index == NoObject) {
    objects_.push_back(object);
    index = objects_.size() - 1;
  }
  auto &state = nodes_.at(destination);
  if (state.targets.test_and_set(index)) {
    ++state.targetCount;
    ++targetRelations_;
    largestTargetSet_ =
        std::max<std::size_t>(largestTargetSet_, state.targetCount);
    enqueueUsers(destination);
  }
}

bool Andersen::mayPointTo(Node node, Node object) const {
  const auto index = nodes_.at(object).objectIndex;
  return index != NoObject && nodes_.at(node).targets.test(index);
}

void Andersen::copy(Node destination, Node source) {
  if (destination == source)
    return;
  if (capturing_) {
    captured_.push_back({FactKind::Copy, destination, source});
    return;
  }
  addConstraint(Kind::Copy, destination, source);
}

void Andersen::load(Node destination, Node pointer) {
  markExternallyAffected(destination);
  if (capturing_) {
    captured_.push_back({FactKind::Load, destination, pointer});
    return;
  }
  addConstraint(Kind::Load, destination, pointer);
}

void Andersen::store(Node pointer, Node source) {
  if (capturing_) {
    captured_.push_back({FactKind::Store, pointer, source});
    return;
  }
  addConstraint(Kind::Store, pointer, source);
}

void Andersen::taintUnknown(Node destination, Node source) {
  if (capturing_) {
    captured_.push_back({FactKind::TaintUnknown, destination, source});
    return;
  }
  addConstraint(Kind::TaintUnknown, destination, source);
}

void Andersen::markUnknown(Node node) {
  if (capturing_) {
    captured_.push_back({FactKind::Unknown, node, 0});
    return;
  }
  if (!nodes_.at(node).unknown) {
    nodes_[node].unknown = true;
    enqueueUsers(node);
  }
}

void Andersen::markExternallyAffected(Node node) {
  // This describes possible origins, not an execution effect. Record it
  // even when the associated constraints wait for body activation.
  if (!nodes_.at(node).externallyAffected) {
    nodes_[node].externallyAffected = true;
    enqueueUsers(node);
  }
}

void Andersen::havoc(Node pointer) {
  if (capturing_) {
    captured_.push_back({FactKind::Havoc, pointer, 0});
    return;
  }
  if (nodes_.at(pointer).havoc)
    return;
  nodes_[pointer].havoc = true;
  addConstraint(Kind::Havoc, pointer, pointer);
}

void Andersen::beginCapture() {
  capturing_ = true;
  captured_.clear();
}

std::vector<Andersen::Fact> Andersen::endCapture() {
  capturing_ = false;
  return std::exchange(captured_, {});
}

void Andersen::replay(std::span<const Fact> facts) {
  for (const auto &fact : facts) {
    switch (fact.kind) {
    case FactKind::Address:
      address(fact.destination, fact.source);
      break;
    case FactKind::Copy:
      copy(fact.destination, fact.source);
      break;
    case FactKind::Load:
      load(fact.destination, fact.source);
      break;
    case FactKind::Store:
      store(fact.destination, fact.source);
      break;
    case FactKind::TaintUnknown:
      taintUnknown(fact.destination, fact.source);
      break;
    case FactKind::Unknown:
      markUnknown(fact.destination);
      break;
    case FactKind::Havoc:
      havoc(fact.destination);
      break;
    }
  }
}

void Andersen::include(Node destination, Node source) {
  if (destination == source)
    return;
  const auto &from = nodes_.at(source);
  auto &state = nodes_.at(destination);
  const bool targetsChanged = state.targets |= from.targets;
  if (targetsChanged) {
    const auto count = state.targets.count();
    targetRelations_ += count - state.targetCount;
    state.targetCount = count;
    largestTargetSet_ = std::max<std::size_t>(largestTargetSet_, count);
  }
  const bool changed = targetsChanged || (from.unknown && !state.unknown) ||
                       (from.externallyAffected && !state.externallyAffected);
  state.unknown |= from.unknown;
  state.externallyAffected |= from.externallyAffected;
  if (changed)
    enqueueUsers(destination);
}

bool Andersen::watch(Node node, std::size_t constraint) {
  auto &users = users_.at(node);
  const auto &state = nodes_.at(node);
  // Target sets and both flags only grow. Include provenance-only changes:
  // reading the same function address from memory can reopen a call.
  const auto deliveredState = (std::uint64_t{state.targetCount} << 2) |
                              (std::uint64_t{state.externallyAffected} << 1) |
                              state.unknown;
  const auto found =
      std::lower_bound(users.begin(), users.end(), constraint,
                       [](const Subscription &user, std::size_t id) {
                         return user.constraint < id;
                       });
  if (found == users.end() || found->constraint != constraint) {
    users.insert(found, {constraint, deliveredState});
    ++watcherCount_;
    return deliveredState != 0;
  }
  if (found->deliveredState == deliveredState)
    return false;
  found->deliveredState = deliveredState;
  return true;
}

void Andersen::enqueue(std::size_t constraint) {
  if (queued_[constraint])
    return;
  queued_[constraint] = true;
  pending_.push_back(constraint);
}

void Andersen::enqueueUsers(Node node) {
  if (!changedFlags_[node]) {
    changedFlags_[node] = true;
    changed_.push_back(node);
  }
  for (const auto &user : users_.at(node))
    enqueue(user.constraint);
}

std::vector<Andersen::Node> Andersen::takeChangedNodes() {
  for (const auto node : changed_)
    changedFlags_[node] = false;
  return std::exchange(changed_, {});
}

void Andersen::addConstraint(Kind kind, Node destination, Node source) {
  const auto index = constraints_.size();
  constraints_.push_back({kind, destination, source});
  queued_.push_back(false);
  users_.at(source).push_back({index});
  ++watcherCount_;
  if (kind == Kind::Store && destination != source) {
    users_.at(destination).push_back({index});
    ++watcherCount_;
  }
  enqueue(index);
}

void Andersen::process(std::size_t index) {
  ++processedConstraints_;
  const auto constraint = constraints_[index];
  switch (constraint.kind) {
  case Kind::Copy:
    include(constraint.destination, constraint.source);
    break;
  case Kind::Load:
    for (const auto object : pointsTo(constraint.source)) {
      if (watch(object, index))
        include(constraint.destination, object);
    }
    if (nodes_.at(constraint.source).unknown)
      markUnknown(constraint.destination);
    break;
  case Kind::Store:
    for (const auto object : pointsTo(constraint.destination))
      include(object, constraint.source);
    if (nodes_.at(constraint.destination).unknown)
      unknownStore_ = true;
    break;
  case Kind::TaintUnknown:
    if (nodes_.at(constraint.source).externallyAffected)
      markExternallyAffected(constraint.destination);
    if (nodes_.at(constraint.source).unknown)
      markUnknown(constraint.destination);
    break;
  case Kind::Havoc:
    for (const auto object : pointsTo(constraint.source)) {
      markUnknown(object);
      havoc(object);
    }
    if (nodes_.at(constraint.source).unknown)
      unknownStore_ = true;
    break;
  }
}

bool Andersen::solve(std::size_t maxPasses) {
  for (std::size_t pass = 0; pass < maxPasses && !pending_.empty(); ++pass) {
    const auto count = pending_.size();
    for (std::size_t i = 0; i < count; ++i) {
      const auto index = pending_.front();
      pending_.pop_front();
      queued_[index] = false;
      process(index);
    }
  }
  return pending_.empty();
}

} // namespace wpgkit
