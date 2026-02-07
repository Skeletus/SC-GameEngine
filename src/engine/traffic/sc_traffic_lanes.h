#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sc_world_partition.h"
#include "sc_traffic_common.h"

namespace sc
{
  class DebugDraw;

  struct LaneNode
  {
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float dir[3] = { 0.0f, 0.0f, 1.0f };
    float speedLimit = 12.0f;
    std::vector<uint32_t> connections;
  };

  struct LaneSegment
  {
    uint32_t startNode = 0;
    uint32_t endNode = 0;
    float width = 3.5f;
    SectorCoord owner{};
    float length = 0.0f;
    float dir[3] = { 0.0f, 0.0f, 1.0f };
    bool active = true;
  };

  struct LaneQuery
  {
    uint32_t laneId = kInvalidLaneId;
    float s = 0.0f;
    float distSq = 0.0f;
  };

  class TrafficLaneGraph
  {
  public:
    void reset();

    void buildProceduralForSector(const SectorCoord& coord, const AABB& bounds, uint32_t seed);
    void removeSector(const SectorCoord& coord);

    LaneQuery queryNearestLane(const float pos[3]) const;
    bool getLookAheadPoint(uint32_t laneId, float s, float distance, float outPoint[3]) const;
    bool advanceAlongLane(uint32_t& laneId, float& s, float distance, float outPos[3], float outDir[3]) const;

    void debugDrawLanes(DebugDraw& draw, bool activeOnly) const;

    const std::vector<uint32_t>* lanesForSector(const SectorCoord& coord) const;
    const LaneSegment* getLane(uint32_t laneId) const;
    const LaneNode* getNode(uint32_t nodeId) const;
    float laneSpeedLimit(uint32_t laneId) const;

    void setLaneWidth(float width) { m_laneWidth = width; }
    void setSpeedLimit(float speed) { m_speedLimit = speed; }
    float laneWidth() const { return m_laneWidth; }
    float speedLimit() const { return m_speedLimit; }

  private:
    struct LaneNodeKey
    {
      int32_t x = 0;
      int32_t y = 0;
      int32_t z = 0;
      int16_t dx = 0;
      int16_t dy = 0;
      int16_t dz = 0;

      bool operator==(const LaneNodeKey& o) const
      {
        return x == o.x && y == o.y && z == o.z && dx == o.dx && dy == o.dy && dz == o.dz;
      }
    };

    struct LaneNodeKeyHash
    {
      size_t operator()(const LaneNodeKey& k) const noexcept;
    };

    uint32_t addNode(const float pos[3], const float dir[3], float speedLimit);
    uint32_t addSegment(uint32_t startNode, uint32_t endNode, const float dir[3], const SectorCoord& owner);
    uint32_t chooseNextSegment(const float dir[3], const LaneNode& node) const;

  private:
    std::vector<LaneNode> m_nodes;
    std::vector<LaneSegment> m_segments;
    std::unordered_map<LaneNodeKey, uint32_t, LaneNodeKeyHash> m_nodeLookup;
    std::unordered_map<SectorCoord, std::vector<uint32_t>, SectorCoordHash> m_sectorSegments;
    float m_laneWidth = 3.5f;
    float m_speedLimit = 12.0f;
  };
}

