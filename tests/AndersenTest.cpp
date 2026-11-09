#include "Andersen.h"
#include <algorithm>
#include <iostream>
#include <random>

using wpgkit::Andersen;

static int basicTests() {
  Andersen pta;
  const auto unchanged = pta.addNode();
  pta.beginCapture();
  pta.copy(unchanged, unchanged);
  const auto captured = pta.endCapture();
  pta.copy(unchanged, unchanged);
  if (!captured.empty() || pta.constraintCount() || pta.watcherCount() ||
      !pta.solve(0)) {
    std::cerr
        << "FAIL: self copies must not allocate facts, watchers or work\n";
    return 1;
  }
  const auto object = pta.addNode();
  const auto function = pta.addNode();
  const auto pointer = pta.addNode();
  const auto pointerToPointer = pta.addNode();
  const auto source = pta.addNode();
  const auto loaded = pta.addNode();
  const auto copied = pta.addNode();
  pta.address(pointer, object);
  pta.address(pointerToPointer, pointer);
  pta.address(source, function);
  pta.store(pointerToPointer, source);
  pta.load(loaded, pointerToPointer);
  pta.copy(copied, loaded);
  if (!pta.solve() || !pta.mayPointTo(pointer, function) ||
      !pta.mayPointTo(loaded, function) || !pta.mayPointTo(copied, object) ||
      !pta.mayPointTo(copied, function)) {
    std::cerr << "FAIL: address/copy/load/store fixed point\n";
    return 1;
  }
  const auto external = pta.addNode();
  pta.markUnknown(external);
  pta.copy(pointerToPointer, external);
  pta.load(loaded, pointerToPointer);
  pta.store(pointerToPointer, source);
  const auto field = pta.addNode();
  pta.taintUnknown(field, pointerToPointer);
  if (!pta.solve() || !pta.isUnknown(loaded) || !pta.isUnknown(field) ||
      !pta.pointsTo(field).empty() || !pta.hasUnknownStore()) {
    std::cerr << "FAIL: unknown values and stores stay explicit\n";
    return 1;
  }

  Andersen growing;
  const auto first = growing.addNode();
  const auto second = growing.addNode();
  const auto firstTarget = growing.addNode();
  const auto secondTarget = growing.addNode();
  const auto lateTarget = growing.addNode();
  const auto pointerVariable = growing.addNode();
  const auto indirect = growing.addNode();
  growing.address(pointerVariable, first);
  growing.address(first, firstTarget);
  growing.load(indirect, pointerVariable);
  if (!growing.solve() || !growing.mayPointTo(indirect, firstTarget)) {
    std::cerr << "FAIL: first indirect target\n";
    return 1;
  }
  const auto changed = growing.takeChangedNodes();
  const auto processed = growing.processedConstraints();
  if (std::find(changed.begin(), changed.end(), indirect) == changed.end() ||
      !growing.solve(0) || growing.processedConstraints() != processed ||
      !growing.takeChangedNodes().empty()) {
    std::cerr << "FAIL: converged state survives a no-op solve without work\n";
    return 1;
  }
  growing.address(first, secondTarget);
  if (!growing.solve() || !growing.mayPointTo(indirect, secondTarget)) {
    std::cerr << "FAIL: newly discovered pointee updates existing load\n";
    return 1;
  }
  growing.address(pointerVariable, second);
  growing.address(second, lateTarget);
  if (!growing.solve() || !growing.mayPointTo(indirect, lateTarget)) {
    std::cerr << "FAIL: newly discovered pointer target updates load\n";
    return 1;
  }
  const auto storedValue = growing.addNode();
  const auto storedTarget = growing.addNode();
  growing.address(storedValue, storedTarget);
  growing.store(pointerVariable, storedValue);
  if (!growing.solve() || !growing.mayPointTo(first, storedTarget) ||
      !growing.mayPointTo(second, storedTarget)) {
    std::cerr << "FAIL: store reaches all discovered targets\n";
    return 1;
  }
  const auto third = growing.addNode();
  growing.address(pointerVariable, third);
  if (!growing.solve() || !growing.mayPointTo(third, storedTarget)) {
    std::cerr << "FAIL: store reaches late pointer target\n";
    return 1;
  }
  // Several pointees can change before a shared reader runs. Unknown-only
  // updates must propagate even when the concrete target count is unchanged.
  growing.address(first, first);
  growing.address(second, second);
  growing.markUnknown(third);
  if (!growing.solve() || !growing.mayPointTo(indirect, first) ||
      !growing.mayPointTo(indirect, second) || !growing.isUnknown(indirect)) {
    std::cerr
        << "FAIL: load retains coalesced pointee and unknown-only updates\n";
    return 1;
  }

  // Older loads may discover a shared cell after newer constraints already
  // subscribe to it. Every subscription must remain unique and receive updates.
  Andersen subscriptions;
  const auto sharedCell = subscriptions.addNode();
  std::vector<Andersen::Node> pointers, loadedValues;
  for (unsigned i = 0; i < 64; ++i) {
    pointers.push_back(subscriptions.addNode());
    loadedValues.push_back(subscriptions.addNode());
    subscriptions.load(loadedValues.back(), pointers.back());
  }
  const auto copyOfCell = subscriptions.addNode();
  subscriptions.copy(copyOfCell, sharedCell);
  for (auto it = pointers.rbegin(); it != pointers.rend(); ++it) {
    subscriptions.address(*it, sharedCell);
    if (!subscriptions.solve())
      return 1;
  }
  const auto subscriptionsBefore = subscriptions.watcherCount();
  for (unsigned i = 0; i < 4; ++i) {
    const auto target = subscriptions.addNode();
    subscriptions.address(sharedCell, target);
    if (!subscriptions.solve() || !subscriptions.mayPointTo(copyOfCell, target))
      return 1;
    for (const auto read : loadedValues)
      if (!subscriptions.mayPointTo(read, target))
        return 1;
    if (subscriptions.watcherCount() != subscriptionsBefore) {
      std::cerr << "FAIL: revisiting dynamic loads duplicates subscriptions\n";
      return 1;
    }
  }

  Andersen unknownCall;
  const auto arg = unknownCall.addNode();
  const auto cell = unknownCall.addNode();
  const auto nested = unknownCall.addNode();
  const auto late = unknownCall.addNode();
  const auto read = unknownCall.addNode();
  unknownCall.address(arg, cell);
  unknownCall.address(cell, nested);
  unknownCall.address(nested, cell); // A cycle must terminate.
  unknownCall.load(read, arg);
  unknownCall.beginCapture();
  unknownCall.havoc(arg);
  auto effects = unknownCall.endCapture();
  unknownCall.solve();
  if (unknownCall.isUnknown(cell))
    return 1; // Capture is not activation.
  unknownCall.replay(effects);
  if (!unknownCall.solve() || !unknownCall.isUnknown(cell) ||
      !unknownCall.isUnknown(nested) || !unknownCall.isUnknown(read) ||
      unknownCall.isUnknown(arg) || !unknownCall.mayPointTo(cell, nested)) {
    std::cerr
        << "FAIL: unknown call modifies reachable storage, preserving known aliases\n";
    return 1;
  }
  unknownCall.address(cell, late);
  if (!unknownCall.solve() || !unknownCall.isUnknown(late)) {
    std::cerr << "FAIL: unknown call effects follow late pointees\n";
    return 1;
  }
  const auto constraints = unknownCall.constraintCount();
  const auto work = unknownCall.processedConstraints();
  unknownCall.havoc(arg);
  if (!unknownCall.solve() || unknownCall.constraintCount() != constraints ||
      unknownCall.processedConstraints() != work) {
    std::cerr << "FAIL: repeated unknown effects do no work\n";
    return 1;
  }

  // Cross bitmap words/chunks, including a late, far-away target. Store/load
  // and unknown propagation must retain every concrete target after growth.
  Andersen dense;
  const auto values = dense.addNode();
  const auto cells = dense.addNode();
  const auto result = dense.addNode();
  for (unsigned i = 3; i < 2048; ++i)
    dense.addNode();
  for (unsigned i = 64; i < 1088; ++i)
    dense.address(values, i);
  for (auto cell : {128u, 255u, 256u, 1024u})
    dense.address(cells, cell);
  dense.store(cells, values);
  dense.load(result, cells);
  if (!dense.solve() || dense.targetCount(result) != 1024) {
    std::cerr
        << "FAIL: dense targets survive store/load across bitmap chunks\n";
    return 1;
  }
  for (unsigned i = 2048; i <= 8192; ++i)
    dense.addNode();
  dense.address(values, 8192);
  dense.markUnknown(values);
  dense.address(cells, 8000);
  if (!dense.solve() || !dense.isUnknown(result) || !dense.isUnknown(8000) ||
      dense.targetCount(result) != 1025 ||
      dense.targetCount(8000) != dense.targetCount(result)) {
    std::cerr
        << "FAIL: late sparse targets and unknown values preserve dense sets\n";
    return 1;
  }
  for (unsigned target = 64; target < 1088; ++target)
    if (!dense.mayPointTo(result, target) || !dense.mayPointTo(8000, target))
      return 1;
  if (!dense.mayPointTo(result, 8192) || !dense.mayPointTo(8000, 8192))
    return 1;
  std::size_t relations = 0;
  for (unsigned i = 0; i < dense.nodeCount(); ++i)
    relations += dense.targetCount(i);
  if (dense.targetRelations() != relations ||
      dense.largestTargetSet() != 1025 || dense.objectCount() != 1026 ||
      dense.nodeCount() != 8193 || dense.mayPointTo(result, 8191)) {
    std::cerr << "FAIL: points-to counters count distinct relations\n";
    return 1;
  }

  // A store can expand its own destination set while visiting it. New bits
  // before and after the current iterator must reach the eventual fixed point.
  Andersen aliasing;
  for (unsigned i = 0; i < 1025; ++i)
    aliasing.addNode();
  aliasing.address(256, 256);
  for (auto target : {0u, 127u, 128u, 511u, 1024u})
    aliasing.address(1, target);
  aliasing.store(256, 1);
  if (!aliasing.solve())
    return 1;
  for (auto target : {0u, 127u, 128u, 511u, 1024u}) {
    const auto actual = aliasing.pointsTo(target);
    const auto expected = aliasing.pointsTo(1);
    if (!std::is_permutation(actual.begin(), actual.end(), expected.begin(),
                             expected.end())) {
      std::cerr << "FAIL: self-aliasing store reaches newly added targets\n";
      return 1;
    }
  }
  return 0;
}

// Independent dense relation oracle: rescan all facts to convergence, with no
// dependency lists, sparse bitmaps, or incremental scheduling from Andersen.
class Reference final {
public:
  explicit Reference(unsigned size)
      : targets_(size, std::vector<bool>(size)), unknown_(size), havoc_(size) {}

  void add(Andersen::Fact fact) {
    facts_.push_back(fact);
  }

  void solve() {
    bool changed;
    do {
      changed = false;
      const auto set = [&](auto &&bit) {
        if (!bit) {
          bit = true;
          changed = true;
        }
      };
      const auto include = [&](unsigned destination, unsigned source) {
        for (unsigned object = 0; object < targets_.size(); ++object)
          if (targets_[source][object])
            set(targets_[destination][object]);
        if (unknown_[source])
          set(unknown_[destination]);
      };
      for (const auto &fact : facts_) {
        const auto d = fact.destination, s = fact.source;
        switch (fact.kind) {
        case Andersen::FactKind::Address:
          set(targets_[d][s]);
          break;
        case Andersen::FactKind::Copy:
          include(d, s);
          break;
        case Andersen::FactKind::Load:
          for (unsigned object = 0; object < targets_.size(); ++object)
            if (targets_[s][object])
              include(d, object);
          if (unknown_[s])
            set(unknown_[d]);
          break;
        case Andersen::FactKind::Store:
          for (unsigned object = 0; object < targets_.size(); ++object)
            if (targets_[d][object])
              include(object, s);
          if (unknown_[d])
            unknownStore_ = true;
          break;
        case Andersen::FactKind::TaintUnknown:
          if (unknown_[s])
            set(unknown_[d]);
          break;
        case Andersen::FactKind::Unknown:
          set(unknown_[d]);
          break;
        case Andersen::FactKind::Havoc:
          set(havoc_[d]);
          break;
        case Andersen::FactKind::Project:
          std::abort(); // Field projections have separate object-aware tests.
        }
      }
      for (unsigned pointer = 0; pointer < targets_.size(); ++pointer) {
        if (!havoc_[pointer])
          continue;
        if (unknown_[pointer])
          unknownStore_ = true;
        for (unsigned object = 0; object < targets_.size(); ++object)
          if (targets_[pointer][object]) {
            set(unknown_[object]);
            set(havoc_[object]);
          }
      }
    } while (changed);
  }

  bool matches(const Andersen &actual) const {
    if (actual.nodeCount() != targets_.size() ||
        actual.hasUnknownStore() != unknownStore_)
      return false;
    for (unsigned node = 0; node < targets_.size(); ++node) {
      if (unknown_[node] != actual.isUnknown(node))
        return false;
      for (unsigned object = 0; object < targets_.size(); ++object)
        if (targets_[node][object] != actual.mayPointTo(node, object))
          return false;
    }
    return true;
  }

private:
  std::vector<std::vector<bool>> targets_;
  std::vector<bool> unknown_, havoc_;
  std::vector<Andersen::Fact> facts_;
  bool unknownStore_ = false;
};

static int fieldTests() {
  Andersen pta({.fieldSensitive = true});
  const auto a = pta.addNode(), b = pta.addNode(), field = pta.addNode();
  const auto pa = pta.addNode(), pb = pta.addNode(), alias = pta.addNode();
  const auto f = pta.addNode(), g = pta.addNode(), value = pta.addNode();
  pta.beginCapture();
  pta.address(pa, a);
  pta.address(pb, b);
  pta.copy(alias, pa);
  const auto left = pta.project(pa, field), right = pta.project(pb, field);
  const auto throughAlias = pta.project(alias, field);
  pta.address(value, f);
  pta.store(left, value);
  const auto read = pta.addNode(), other = pta.addNode();
  pta.load(read, throughAlias);
  pta.load(other, right);
  const auto facts = pta.endCapture();
  pta.replay(facts);
  if (!pta.solve() || !pta.mayPointTo(read, f) || pta.mayPointTo(other, f))
    return 1;
  pta.address(alias, b);
  pta.address(value, g);
  pta.store(right, value);
  if (!pta.solve() || !pta.mayPointTo(read, g) ||
      pta.targetCount(throughAlias) != 2)
    return 1;
  pta.havoc(pa);
  if (!pta.solve() || !pta.isUnknown(read) || pta.isUnknown(other))
    return 1;
  const auto nested = pta.project(left, field);
  const auto loaded = pta.addNode();
  pta.load(loaded, nested);
  if (!pta.solve() || !pta.isUnknown(loaded))
    return 1; // A subobject created after havoc inherits its effects.
  return 0;
}

static bool boundedProjection() {
  Andersen pta({.fieldSensitive = true});
  const auto object = pta.addNode(), field = pta.addNode();
  auto pointer = pta.addNode();
  pta.address(pointer, object);
  for (unsigned depth = 0; depth < 12; ++depth)
    pointer = pta.project(pointer, field);
  return pta.solve() && pta.isUnknown(pointer);
}

static bool collapsedFields() {
  Andersen pta;
  const auto object = pta.addNode(), other = pta.addNode();
  const auto base = pta.addNode(), otherBase = pta.addNode();
  const auto field = pta.addNode(), secondField = pta.addNode();
  const auto function = pta.addNode(), input = pta.addNode();
  pta.beginCapture();
  pta.address(base, object);
  pta.address(otherBase, other);
  const auto first = pta.project(base, field);
  const auto second = pta.project(base, secondField);
  const auto nested = pta.project(second, field);
  const auto separate = pta.project(otherBase, field);
  const auto read = pta.addNode(), otherRead = pta.addNode();
  pta.address(input, function);
  pta.store(first, input);
  pta.load(read, nested);
  pta.load(otherRead, separate);
  pta.replay(pta.endCapture());
  if (pta.fieldSensitive() || !pta.solve() || pta.objectCount() != 3 ||
      !pta.mayPointTo(read, function) || pta.mayPointTo(otherRead, function) ||
      !pta.mayPointTo(second, object))
    return false;
  // Unbounded external field writes still affect collapsed object contents.
  pta.markUnknown(secondField);
  return pta.solve() && pta.isUnknown(read) && !pta.isUnknown(otherRead);
}

int main() {
  if (!collapsedFields()) {
    std::cerr << "FAIL: default field collapse, object separation or effects\n";
    return 1;
  }
  if (!boundedProjection())
    return 1;
  if (fieldTests()) {
    std::cerr << "FAIL: field/object separation, aliases, capture or havoc\n";
    return 1;
  }
  if (basicTests())
    return 1;

  // Differential checks across incremental batches: every pair and unknown bit,
  // including dynamic load subscriptions, self-aliasing stores, and havoc.
  for (unsigned seed = 1; seed <= 32; ++seed) {
    std::mt19937 random(seed);
    constexpr unsigned size = 48;
    Reference reference(size);
    Andersen incremental;
    for (unsigned i = 0; i < size; ++i)
      incremental.addNode();
    for (unsigned batch = 0; batch < 8; ++batch) {
      for (unsigned i = 0; i < 24; ++i) {
        // Delay unknown/havoc facts so earlier batches exercise concrete sets.
        const Andersen::Fact fact{
            static_cast<Andersen::FactKind>(random() % (batch < 4 ? 4 : 7)),
            static_cast<Andersen::Node>(random() % size),
            static_cast<Andersen::Node>(random() % size)};
        reference.add(fact);
        incremental.replay({&fact, 1});
      }
      incremental.solve(1);
      reference.solve();
      if (!incremental.solve() || !reference.matches(incremental)) {
        std::cerr
            << "FAIL: incremental result differs from the dense oracle at seed "
            << seed << ", batch " << batch << '\n';
        return 1;
      }
      const auto work = incremental.processedConstraints();
      if (!incremental.solve(0) || work != incremental.processedConstraints())
        return 1;
    }
  }
}
