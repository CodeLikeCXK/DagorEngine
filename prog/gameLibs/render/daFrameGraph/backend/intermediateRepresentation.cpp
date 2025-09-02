// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "intermediateRepresentation.h"

#include <memory/dag_framemem.h>
#include <math/integer/dag_IPoint2.h>
#include <EASTL/bitvector.h>

#include <id/idRange.h>
#include <id/idExtentsFinder.h>


namespace dafg
{

namespace intermediate
{

void Graph::choseSubgraph(eastl::span<const NodeIndex> old_to_new_index_mapping)
{
  G_ASSERT(old_to_new_index_mapping.size() == nodes.size());

  // Jump through hoops in order to do this without any persistent
  // allocations

  FRAMEMEM_VALIDATE;

  size_t newNodeCount = 0;
  for (auto [i, _] : nodes.enumerate())
    if (old_to_new_index_mapping[i] != NODE_NOT_MAPPED)
      ++newNodeCount;

  // Sanity check: new indexing should not have gaps
  for (auto [i, _] : nodes.enumerate())
    if (old_to_new_index_mapping[i] != NODE_NOT_MAPPED)
      G_ASSERT(old_to_new_index_mapping[i] < newNodeCount);


  const auto choseSoa = [](auto &old_data, size_t new_count, const auto &mapping, const auto unmappedSentinel) {
    using Mapping = eastl::decay_t<decltype(old_data)>;
    IdIndexedMapping<typename Mapping::index_type, typename Mapping::value_type, framemem_allocator> newData;
    newData.resize(new_count);
    for (auto [i, data] : old_data.enumerate())
      if (mapping[i] != unmappedSentinel)
        newData[mapping[i]] = eastl::move(data);

    old_data.resize(new_count);
    eastl::move(newData.begin(), newData.end(), old_data.begin());
  };

  choseSoa(nodes, newNodeCount, old_to_new_index_mapping, NODE_NOT_MAPPED);
  choseSoa(nodeStates, newNodeCount, old_to_new_index_mapping, NODE_NOT_MAPPED);
  if (!nodeNames.empty())
    choseSoa(nodeNames, newNodeCount, old_to_new_index_mapping, NODE_NOT_MAPPED);

  // We need to fixup all node -> node references
  for (auto [i, node] : nodes.enumerate())
  {
    auto oldPreds = eastl::move(node.predecessors);
    node.predecessors.clear();
    for (const auto pred : oldPreds)
      if (old_to_new_index_mapping[pred] != NODE_NOT_MAPPED)
        node.predecessors.insert(old_to_new_index_mapping[pred]);
  }

  dag::Vector<ResourceIndex, framemem_allocator> resourceMapping(resources.size(), RESOURCE_NOT_MAPPED);

  {
    dag::Vector<Resource, framemem_allocator> newResources;
    newResources.reserve(resources.size());
    for (auto &node : nodes)
      for (auto &req : node.resourceRequests)
      {
        if (resourceMapping[req.resource] == RESOURCE_NOT_MAPPED)
        {
          resourceMapping[req.resource] = static_cast<ResourceIndex>(newResources.size());
          newResources.emplace_back(eastl::move(resources[req.resource]));
        }
        req.resource = resourceMapping[req.resource];
      }
    resources.resize(newResources.size());
    eastl::move(newResources.begin(), newResources.end(), resources.begin());
  }
  for (auto [resIdx, res] : resources.enumerate())
  {
    if (!res.isScheduled())
      continue;
    if (const auto dynamicClearValue = eastl::get_if<DynamicParameter>(&res.asScheduled().clearValue))
      dynamicClearValue->resource = resourceMapping[dynamicClearValue->resource];
  }

  if (!resourceNames.empty())
    choseSoa(resourceNames, resources.size(), resourceMapping, RESOURCE_NOT_MAPPED);

  const auto updateResourceIndex = [&resourceMapping](ResourceIndex &index) {
    G_ASSERT(resourceMapping[index] != RESOURCE_NOT_MAPPED);
    index = resourceMapping[index];
  };

  // We also need to fix up node -> resource references
  for (auto &state : nodeStates)
  {
    for (const auto &[id, binding] : state.bindings)
      if (binding.resource.has_value())
      {
        // FIXME: this is a dumb hack due to iteration being const only
        // for dag::FixedVectorMap, should probably fix it at some point...
        updateResourceIndex(*state.bindings[id].resource);
      }

    if (state.pass)
    {
      if (state.pass->depthAttachment.has_value())
        updateResourceIndex(state.pass->depthAttachment->resource);
      if (state.pass->vrsRateAttachment.has_value())
        updateResourceIndex(state.pass->vrsRateAttachment->resource);
      for (auto &color : state.pass->colorAttachments)
        if (color.has_value())
          updateResourceIndex(color->resource);
      const auto oldResolves = eastl::move(state.pass->resolves);
      state.pass->resolves.clear();
      for (const auto &[src, dst] : oldResolves)
      {
        G_ASSERT(resourceMapping[src] != RESOURCE_NOT_MAPPED);
        G_ASSERT(resourceMapping[dst] != RESOURCE_NOT_MAPPED);
        state.pass->resolves.insert({resourceMapping[src], resourceMapping[dst]});
      }
    }

    for (auto &vertexSource : state.vertexSources)
      if (vertexSource.has_value() && vertexSource->buffer.has_value())
        updateResourceIndex(*vertexSource->buffer);
    if (state.indexSource.has_value() && state.indexSource->buffer.has_value())
      updateResourceIndex(*state.indexSource->buffer);
  }
}

Mapping Graph::calculateMapping() const
{
  // Jump through hoops to ensure a single allocation

  IdExtentsFinder<MultiplexingIndex> multiIdxExtents;

  IdExtentsFinder<NodeNameId> nodeNameIdExtents;
  for (const auto &node : nodes)
  {
    multiIdxExtents.update(node.multiplexingIndex);
    if (node.frontendNode)
      nodeNameIdExtents.update(*node.frontendNode);
  }

  IdExtentsFinder<ResNameId> resNameIdExtents;
  for (const auto &res : resources)
  {
    multiIdxExtents.update(res.multiplexingIndex);
    for (auto resNameId : res.frontendResources)
      resNameIdExtents.update(resNameId);
  }

  Mapping result{nodeNameIdExtents.get(), resNameIdExtents.get(), multiIdxExtents.get()};

  for (auto [i, res] : resources.enumerate())
    for (auto resNameId : res.frontendResources)
      result.mapRes(resNameId, res.multiplexingIndex) = i;

  for (auto [i, node] : nodes.enumerate())
    if (node.frontendNode)
      result.mapNode(*node.frontendNode, node.multiplexingIndex) = i;

  // NOTE: This is a bit fragile. The frontend multiplexing code has to
  // guarantee that in case of undermultiplexed nodes/resources each
  // "empty" mapping slot should be mapped to the nearest previous mapped
  // slot.

  for (uint32_t i = 1; i < result.multiplexingExtent(); ++i)
  {
    const auto currMultiIdx = static_cast<MultiplexingIndex>(i);
    const auto prevMultiIdx = static_cast<MultiplexingIndex>(i - 1);

    for (auto resNameId : IdRange<ResNameId>(result.mappedResNameIdCount()))
      if (!result.wasResMapped(resNameId, currMultiIdx))
        result.mapRes(resNameId, currMultiIdx) = result.mapRes(resNameId, prevMultiIdx);

    for (auto nodeNameId : IdRange<NodeNameId>(result.mappedNodeNameIdCount()))
      if (!result.wasNodeMapped(nodeNameId, currMultiIdx))
        result.mapNode(nodeNameId, currMultiIdx) = result.mapNode(nodeNameId, prevMultiIdx);
  }

  return result;
}

void Graph::validate() const
{
#if DAGOR_DBGLEVEL > 0
  G_ASSERTF_RETURN(nodes.size() == nodeStates.size(), , //
    "Inconsistent IR: node count %d and node state count %d should be equal!", nodes.size(), nodeStates.size());
  G_ASSERTF_RETURN(nodeNames.empty() || nodes.size() == nodeNames.size(), , //
    "Inconsistent IR: node count %d and node name count %d should be equal if names are present at all!", nodes.size(),
    nodeNames.size());
  G_ASSERTF_RETURN(resourceNames.empty() || resources.size() == resourceNames.size(), , //
    "Inconsistent IR: resource count %d and resource name count %d should be equal if names are present at all!", resources.size(),
    resourceNames.size());

  for (auto &node : nodes)
  {
    for (const auto pred : node.predecessors)
      G_ASSERTF(pred < nodes.size(), "Inconsistent IR: missing node with index %d", pred);
    for (auto &req : node.resourceRequests)
      G_ASSERTF(req.resource < resources.size(), "Inconsistent IR: missing resource with index %d", req.resource);
  }

  for (auto [nodeIdx, state] : nodeStates.enumerate())
  {
    const auto validateRes = [this, nodeIdx = nodeIdx](ResourceIndex res_idx) {
      G_ASSERTF_RETURN(res_idx < resources.size(), , "Inconsistent IR: missing resource with index %d", res_idx);

      bool found = false;
      for (const auto &req : nodes[nodeIdx].resourceRequests)
        if (req.resource == res_idx)
          found = true;
      G_ASSERTF(found, "Inconsistent IR: resource %s used in node %s state, but was not requested by it!", resourceNames[res_idx],
        nodeNames[nodeIdx]);
    };

    for (const auto &[_, binding] : state.bindings)
      if (binding.resource.has_value())
        validateRes(*binding.resource);

    if (state.pass.has_value())
    {
      if (state.pass->depthAttachment.has_value())
        validateRes(state.pass->depthAttachment->resource);
      if (state.pass->vrsRateAttachment.has_value())
        validateRes(state.pass->vrsRateAttachment->resource);
      for (const auto &color : state.pass->colorAttachments)
        if (color.has_value())
          validateRes(color->resource);
    }
  }

  for (auto [resIdx, res] : resources.enumerate())
    G_ASSERT(!res.frontendResources.empty());
#endif
}

Mapping::Mapping(uint32_t node_count, uint32_t res_count, uint32_t multiplexing_extent) :
  mappedNodeNameIdCount_{node_count},
  mappedResNameIdCount_{res_count},
  multiplexingExtent_{multiplexing_extent},
  nodeNameIdMapping_(node_count * multiplexing_extent, NODE_NOT_MAPPED),
  resNameIdMapping_(res_count * multiplexing_extent, RESOURCE_NOT_MAPPED)
{}

bool Mapping::wasResMapped(ResNameId id, MultiplexingIndex multi_idx) const
{
  return eastl::to_underlying(multi_idx) < multiplexingExtent_ && eastl::to_underlying(id) < mappedResNameIdCount_ &&
         mapRes(id, multi_idx) != RESOURCE_NOT_MAPPED;
}

bool Mapping::wasNodeMapped(NodeNameId id, MultiplexingIndex multi_idx) const
{
  return eastl::to_underlying(multi_idx) < multiplexingExtent_ && eastl::to_underlying(id) < mappedNodeNameIdCount_ &&
         mapNode(id, multi_idx) != NODE_NOT_MAPPED;
}

uint32_t Mapping::multiplexingExtent() const { return multiplexingExtent_; }

ResourceIndex &Mapping::mapRes(ResNameId id, MultiplexingIndex multi_idx)
{
  return resNameIdMapping_[eastl::to_underlying(multi_idx) + multiplexingExtent_ * eastl::to_underlying(id)];
}

// Const cast is ok in below cases as we never mutate `this`, only copy a
// value from there. This trick avoids copy-pasta.
ResourceIndex Mapping::mapRes(ResNameId id, MultiplexingIndex multi_idx) const
{
  return const_cast<Mapping *>(this)->mapRes(id, multi_idx);
}

NodeIndex &Mapping::mapNode(NodeNameId id, MultiplexingIndex multi_idx)
{
  return nodeNameIdMapping_[eastl::to_underlying(multi_idx) + multiplexingExtent_ * eastl::to_underlying(id)];
}

NodeIndex Mapping::mapNode(NodeNameId id, MultiplexingIndex multi_idx) const
{
  return const_cast<Mapping *>(this)->mapNode(id, multi_idx);
}

} // namespace intermediate

} // namespace dafg
