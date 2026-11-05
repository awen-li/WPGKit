#include "DataFlowGraph.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace wpgkit {

DataNodeId DataFlowGraph::addNode(DataNode node) {
  nodes_.push_back(node);
  return DataNodeId{nodes_.size()};
}

void DataFlowGraph::addBody(BodyId body, std::span<const DataNode> nodes) {
  if (!body.isValid() || bodies_.contains(body) ||
      std::any_of(nodes.begin(), nodes.end(), [body](const auto &node) {
        return node.position.body != body;
      }))
    throw std::invalid_argument("invalid or duplicate DFG body");
  const auto first = nodes_.size() + 1;
  nodes_.insert(nodes_.end(), nodes.begin(), nodes.end());
  bodies_.emplace(body, BodyRange{first, nodes.size()});
}

DataNodeId DataFlowGraph::localNode(BodyId body, std::uint32_t index) const {
  const auto found = bodies_.find(body);
  return found != bodies_.end() && index < found->second.size
             ? DataNodeId{found->second.first + index}
             : DataNodeId{};
}

std::span<const DataNode> DataFlowGraph::bodyNodes(BodyId body) const {
  const auto found = bodies_.find(body);
  if (found == bodies_.end())
    return {};
  return std::span{nodes_}.subspan(found->second.first - 1, found->second.size);
}

void DataFlowGraph::addEdge(DataNodeId source, DataNodeId target,
                            DataEdgeKind kind, CallConnectionId connection) {
  if (!node(source) || !node(target))
    throw std::invalid_argument("invalid DFG node");
  edges_.push_back({source, target, kind, connection});
}

const DataNode *DataFlowGraph::node(DataNodeId id) const noexcept {
  return id.isValid() && id.value() <= nodes_.size() ? &nodes_[id.value() - 1]
                                                     : nullptr;
}

void DataFlowGraph::prepare() {
  std::sort(edges_.begin(), edges_.end());
  edges_.erase(std::unique(edges_.begin(), edges_.end()), edges_.end());
  std::sort(gaps_.begin(), gaps_.end());
  gaps_.erase(std::unique(gaps_.begin(), gaps_.end()), gaps_.end());
  incoming_.resize(edges_.size());
  std::iota(incoming_.begin(), incoming_.end(), 0);
  std::sort(incoming_.begin(), incoming_.end(), [&](auto a, auto b) {
    return std::pair{edges_[a].target, a} < std::pair{edges_[b].target, b};
  });
}

std::span<const DataEdge> DataFlowGraph::outgoing(DataNodeId id) const {
  const auto first = std::lower_bound(
      edges_.begin(), edges_.end(), id,
      [](const auto &edge, auto key) { return edge.source < key; });
  const auto last =
      std::upper_bound(first, edges_.end(), id, [](auto key, const auto &edge) {
        return key < edge.source;
      });
  return {first, last};
}

std::vector<DataEdge> DataFlowGraph::incoming(DataNodeId id) const {
  std::vector<DataEdge> result;
  auto found = std::lower_bound(
      incoming_.begin(), incoming_.end(), id,
      [&](auto index, auto key) { return edges_[index].target < key; });
  for (; found != incoming_.end() && edges_[*found].target == id; ++found)
    result.push_back(edges_[*found]);
  return result;
}

} // namespace wpgkit
