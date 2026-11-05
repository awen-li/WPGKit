#pragma once

#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/iterator_range.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

namespace wpgkit {

// Monotone points-to engine for CFG-derived definitions and shared memory.
// A node denotes a pointer value or an addressable object (e.g. *pp = &f).
class Andersen final {
public:
  using Node = std::uint32_t;
  enum class FactKind {
    Address,
    Copy,
    Load,
    Store,
    TaintUnknown,
    Unknown,
    Havoc
  };

  struct Fact final {
    FactKind kind;
    Node destination;
    Node source;
  };

private:
  // Translate compact bitmap positions without materializing another target
  // set.
  class TargetIterator final {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Node;
    using difference_type = std::ptrdiff_t;
    using reference = Node;
    using pointer = void;
    TargetIterator() = default;

    TargetIterator(llvm::SparseBitVector<>::iterator position,
                   const std::vector<Node> &objects)
        : position_(position), objects_(&objects) {}

    Node operator*() const {
      return (*objects_)[*position_];
    }

    TargetIterator &operator++() {
      ++position_;
      return *this;
    }

    TargetIterator operator++(int) {
      auto copy = *this;
      ++*this;
      return copy;
    }

    bool operator==(const TargetIterator &other) const {
      return objects_ == other.objects_ &&
             (!objects_ || position_ == other.position_);
    }

  private:
    llvm::SparseBitVector<>::iterator position_{};
    const std::vector<Node> *objects_ = nullptr;
  };

public:
  Node addNode();
  void address(Node destination, Node object); // p = &o
  void copy(Node destination, Node source);    // p = q
  void load(Node destination, Node pointer);   // p = *q
  void store(Node pointer, Node source);       // *p = q
  void taintUnknown(Node destination,
                    Node source); // unknown base may hide a field value
  void markUnknown(Node node);
  // Structural provenance: memory writes or unknown callers can supply this
  // value. Propagates through the target worklist, without another graph.
  void markExternallyAffected(Node node);

  bool isExternallyAffected(Node node) const {
    return nodes_.at(node).externallyAffected;
  }

  // Unknown code may modify transitively reachable memory. Watch new pointees
  // as they arrive, retain known targets, and install at most one watcher/node.
  void havoc(Node pointer);

  // Returns false if the pass budget is exhausted. Existing facts remain
  // available, but the client must not treat them as a fixed point.
  bool solve(std::size_t maxPasses = 10000);

  // Borrowed range of original node IDs; iteration order is unspecified.
  auto pointsTo(Node node) const {
    const auto &targets = nodes_.at(node).targets;
    return llvm::make_range(TargetIterator(targets.begin(), objects_),
                            TargetIterator(targets.end(), objects_));
  }

  bool mayPointTo(Node node, Node object) const;

  std::size_t targetCount(Node node) const {
    return nodes_.at(node).targetCount;
  }

  bool isUnknown(Node node) const {
    return nodes_.at(node).unknown;
  }

  bool hasUnknownStore() const {
    return unknownStore_;
  }

  // Changes since the last drain, for clients watching indirect callees.
  std::vector<Node> takeChangedNodes();

  std::size_t constraintCount() const noexcept {
    return constraints_.size();
  }

  std::size_t processedConstraints() const noexcept {
    return processedConstraints_;
  }

  std::size_t targetRelations() const noexcept {
    return targetRelations_;
  }

  std::size_t largestTargetSet() const noexcept {
    return largestTargetSet_;
  }

  std::size_t watcherCount() const noexcept {
    return watcherCount_;
  }

  // Capture constraints while a Clang AST is alive. The returned values hold
  // only numeric nodes and can be replayed after that AST is destroyed.
  void beginCapture();
  std::vector<Fact> endCapture();
  void replay(std::span<const Fact> facts);

  std::size_t nodeCount() const noexcept {
    return nodes_.size();
  }

  std::size_t objectCount() const noexcept {
    return objects_.size();
  }

private:
  enum class Kind { Copy, Load, Store, TaintUnknown, Havoc };

  struct Constraint {
    Kind kind;
    Node destination;
    Node source;
  };

  static constexpr Node NoObject = std::numeric_limits<Node>::max();

  struct State {
    llvm::SparseBitVector<>
        targets; // Dense object indexes, not pointer node IDs.
    unsigned targetCount = 0;
    Node objectIndex = NoObject;
    bool unknown = false;
    bool havoc = false;
    bool externallyAffected = false;
  };

  void include(Node destination, Node source);
  // Subscribe a load; return whether this source has new information to
  // deliver.
  bool watch(Node node, std::size_t constraint);
  void enqueue(std::size_t constraint);
  void enqueueUsers(Node node);
  void addConstraint(Kind kind, Node destination, Node source);
  void process(std::size_t constraint);
  // Growing the node inventory must not copy every existing points-to set.
  std::deque<State> nodes_;
  std::vector<Node>
      objects_; // Only nodes used as address targets need a bit index.
  std::vector<Constraint> constraints_;

  struct Subscription {
    std::size_t constraint;
    std::uint64_t deliveredState =
        0; // Used by loads; empty/known needs no union.
  };

  std::vector<std::vector<Subscription>> users_; // Sorted by constraint ID.
  std::deque<std::size_t> pending_;
  std::vector<bool> queued_;
  std::vector<Node> changed_;
  std::vector<bool> changedFlags_;
  std::size_t processedConstraints_ = 0;
  std::size_t targetRelations_ = 0;
  std::size_t largestTargetSet_ = 0;
  std::size_t watcherCount_ = 0;
  bool capturing_ = false;
  std::vector<Fact> captured_;
  bool unknownStore_ = false;
};

} // namespace wpgkit
