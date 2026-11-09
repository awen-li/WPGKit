#pragma once

#include "GraphIds.h"
#include <map>
#include <span>
#include <vector>

namespace wpgkit {

using DataNodeId = Id<class DataNodeIdTag>;
enum class DataNodeKind : std::uint8_t {
  Value,
  Definition,
  Join,
  Parameter,
  Argument,
  Return,
  CallResult,
  Memory,
  Unknown,
  Receiver
};
enum class DataEdgeKind : std::uint8_t {
  Value,
  DefUse,
  ActualFormal,
  Return,
  Memory,
  Summary,
  // A singleton storage access, or a partition-to-subobject relation, emitted
  // when field sensitivity is enabled. Dependency edges still use Memory.
  // Collapsed fields must not be mistaken for distinct update locations.
  Location
};
enum class DataGapKind : std::uint8_t {
  MissingCFG,
  UnsupportedEffect,
  UninitializedRead,
  UnknownAddress,
  UnmappedCall,
  UnknownCall,
  ArgumentMismatch,
  ExternalState
};

struct DataNode final {
  CFGPosition position;
  DataNodeKind kind = DataNodeKind::Value;
  // Parameter/argument index, body-local variable for Definition/Join, or PTA
  // object ID for Memory. Variable identity is distinct from definition sites.
  // Receiver holds a direct object's PTA storage ID, or UINT32_MAX otherwise.
  std::uint32_t slot = 0;
};

struct DataEdge final {
  DataNodeId source, target;
  DataEdgeKind kind = DataEdgeKind::Value;
  CallConnectionId
      connection; // Actual/formal and return edges match inter-CFG IDs.
  auto operator<=>(const DataEdge &) const = default;
};

struct DataGap final {
  CFGPosition position;
  DataGapKind kind;
  CallSiteId site;
  auto operator<=>(const DataGap &) const = default;
};

// Owned MAY data dependencies, not control dependence or execution semantics.
// Local nonescaping variables use CFG reaching definitions. Memory partitions
// conservatively merge overlapping PTA targets and are flow/context
// insensitive. A graph can be cyclic (loops/recursion). Clients must balance
// connection IDs for context-sensitive traversal and inspect gaps and the input
// boundary.
class DataFlowGraph final {
public:
  DataNodeId addNode(DataNode node);
  // Preserve the extraction-to-DFG identity map without a second node table.
  void addBody(BodyId body, std::span<const DataNode> nodes);
  [[nodiscard]] DataNodeId localNode(BodyId body, std::uint32_t index) const;
  [[nodiscard]] std::span<const DataNode> bodyNodes(BodyId body) const;
  void addEdge(DataNodeId source, DataNodeId target, DataEdgeKind kind,
               CallConnectionId connection = {});

  void addGap(DataGap gap) {
    gaps_.push_back(gap);
  }

  void prepare(); // Sort/deduplicate and build the reverse query index once.

  void reserve(std::size_t nodes, std::size_t edges) {
    nodes_.reserve(nodes);
    edges_.reserve(edges);
  }

  [[nodiscard]] const DataNode *node(DataNodeId id) const noexcept;

  [[nodiscard]] std::span<const DataNode> nodes() const {
    return nodes_;
  }

  [[nodiscard]] std::span<const DataEdge> edges() const {
    return edges_;
  }

  [[nodiscard]] std::span<const DataGap> gaps() const {
    return gaps_;
  }

  [[nodiscard]] std::span<const DataEdge> outgoing(DataNodeId id) const;
  [[nodiscard]] std::vector<DataEdge> incoming(DataNodeId id) const;

private:
  struct BodyRange {
    std::uint64_t first;
    std::size_t size;
  };

  std::map<BodyId, BodyRange> bodies_;
  std::vector<DataNode> nodes_;
  std::vector<DataEdge> edges_;
  std::vector<std::size_t> incoming_;
  std::vector<DataGap> gaps_;
};

} // namespace wpgkit
