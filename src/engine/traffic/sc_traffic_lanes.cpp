#include "sc_traffic_lanes.h"
#include "sc_debug_draw.h"

#include <algorithm>
#include <cmath>

namespace sc
{
  namespace
  {
    static float length3(const float v[3])
    {
      return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    static void normalize3(float v[3])
    {
      const float len = length3(v);
      if (len > 1e-6f)
      {
        const float inv = 1.0f / len;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
      }
    }

    static float dot3(const float a[3], const float b[3])
    {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    static int32_t quantPos(float v)
    {
      const float scaled = v * 100.0f;
      return static_cast<int32_t>(std::floor(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
    }

    static int16_t quantDir(float v)
    {
      const float scaled = v * 1000.0f;
      return static_cast<int16_t>(std::floor(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
    }
  }

  size_t TrafficLaneGraph::LaneNodeKeyHash::operator()(const LaneNodeKey& k) const noexcept
  {
    const uint64_t a = static_cast<uint32_t>(k.x);
    const uint64_t ay = static_cast<uint32_t>(k.y);
    const uint64_t b = static_cast<uint32_t>(k.z);
    const uint64_t c = static_cast<uint16_t>(k.dx);
    const uint64_t cy = static_cast<uint16_t>(k.dy);
    const uint64_t d = static_cast<uint16_t>(k.dz);
    return static_cast<size_t>((a * 73856093ull) ^ (ay * 83492791ull) ^ (b * 19349663ull) ^ (c * 2654435761ull) ^ (cy * 97531ull) ^ (d * 4256249ull));
  }

  void TrafficLaneGraph::reset()
  {
    m_nodes.clear();
    m_segments.clear();
    m_nodeLookup.clear();
    m_sectorSegments.clear();
  }

  uint32_t TrafficLaneGraph::addNode(const float pos[3], const float dir[3], float speedLimit)
  {
    LaneNodeKey key{};
    key.x = quantPos(pos[0]);
    key.y = quantPos(pos[1]);
    key.z = quantPos(pos[2]);
    key.dx = quantDir(dir[0]);
    key.dy = quantDir(dir[1]);
    key.dz = quantDir(dir[2]);

    auto it = m_nodeLookup.find(key);
    if (it != m_nodeLookup.end())
      return it->second;

    const uint32_t idx = static_cast<uint32_t>(m_nodes.size());
    LaneNode node{};
    node.pos[0] = pos[0];
    node.pos[1] = pos[1];
    node.pos[2] = pos[2];
    node.dir[0] = dir[0];
    node.dir[1] = dir[1];
    node.dir[2] = dir[2];
    node.speedLimit = speedLimit;
    m_nodes.push_back(node);
    m_nodeLookup.emplace(key, idx);
    return idx;
  }

  uint32_t TrafficLaneGraph::addSegment(uint32_t startNode,
                                        uint32_t endNode,
                                        const float dir[3],
                                        const SectorCoord& owner)
  {
    if (startNode >= m_nodes.size() || endNode >= m_nodes.size())
      return kInvalidLaneId;

    const LaneNode& a = m_nodes[startNode];
    const LaneNode& b = m_nodes[endNode];
    float segDir[3] = { b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2] };
    const float len = length3(segDir);
    if (len > 1e-6f)
    {
      const float inv = 1.0f / len;
      segDir[0] *= inv;
      segDir[1] *= inv;
      segDir[2] *= inv;
    }
    else
    {
      segDir[0] = dir[0];
      segDir[1] = dir[1];
      segDir[2] = dir[2];
      normalize3(segDir);
    }

    LaneSegment seg{};
    seg.startNode = startNode;
    seg.endNode = endNode;
    seg.width = m_laneWidth;
    seg.owner = owner;
    seg.length = len;
    seg.dir[0] = segDir[0];
    seg.dir[1] = segDir[1];
    seg.dir[2] = segDir[2];
    seg.active = true;

    const uint32_t idx = static_cast<uint32_t>(m_segments.size());
    m_segments.push_back(seg);
    m_nodes[startNode].connections.push_back(idx);
    return idx;
  }

  uint32_t TrafficLaneGraph::chooseNextSegment(const float dir[3], const LaneNode& node) const
  {
    uint32_t best = kInvalidLaneId;
    float bestDot = -1.0f;
    for (uint32_t segId : node.connections)
    {
      if (segId >= m_segments.size())
        continue;
      const LaneSegment& seg = m_segments[segId];
      if (!seg.active)
        continue;
      const float d = dot3(dir, seg.dir);
      if (d > bestDot)
      {
        bestDot = d;
        best = segId;
      }
    }
    return best;
  }

  void TrafficLaneGraph::buildProceduralForSector(const SectorCoord& coord,
                                                  const AABB& bounds,
                                                  uint32_t seed)
  {
    (void)seed;
    auto& list = m_sectorSegments[coord];
    if (!list.empty())
    {
      for (uint32_t segId : list)
      {
        if (segId < m_segments.size())
          m_segments[segId].active = true;
      }
      return;
    }

    list.clear();
    list.reserve(4);

    const float minX = bounds.min.x;
    const float maxX = bounds.max.x;
    const float minZ = bounds.min.z;
    const float maxZ = bounds.max.z;
    const float centerX = (minX + maxX) * 0.5f;
    const float centerZ = (minZ + maxZ) * 0.5f;
    const float y = 0.0f;
    const float offset = m_laneWidth * 0.5f;

    // X road (east/west)
    {
      const float dirPos[3] = { 1.0f, 0.0f, 0.0f };
      const float dirNeg[3] = { -1.0f, 0.0f, 0.0f };

      float start[3] = { minX, y, centerZ - offset };
      float end[3] = { maxX, y, centerZ - offset };
      uint32_t n0 = addNode(start, dirPos, m_speedLimit);
      uint32_t n1 = addNode(end, dirPos, m_speedLimit);
      if (uint32_t seg = addSegment(n0, n1, dirPos, coord); seg != kInvalidLaneId)
        list.push_back(seg);

      start[0] = maxX; start[2] = centerZ + offset;
      end[0] = minX; end[2] = centerZ + offset;
      n0 = addNode(start, dirNeg, m_speedLimit);
      n1 = addNode(end, dirNeg, m_speedLimit);
      if (uint32_t seg = addSegment(n0, n1, dirNeg, coord); seg != kInvalidLaneId)
        list.push_back(seg);
    }

    // Z road (north/south)
    {
      const float dirPos[3] = { 0.0f, 0.0f, 1.0f };
      const float dirNeg[3] = { 0.0f, 0.0f, -1.0f };

      float start[3] = { centerX + offset, y, minZ };
      float end[3] = { centerX + offset, y, maxZ };
      uint32_t n0 = addNode(start, dirPos, m_speedLimit);
      uint32_t n1 = addNode(end, dirPos, m_speedLimit);
      if (uint32_t seg = addSegment(n0, n1, dirPos, coord); seg != kInvalidLaneId)
        list.push_back(seg);

      start[0] = centerX - offset; start[2] = maxZ;
      end[0] = centerX - offset; end[2] = minZ;
      n0 = addNode(start, dirNeg, m_speedLimit);
      n1 = addNode(end, dirNeg, m_speedLimit);
      if (uint32_t seg = addSegment(n0, n1, dirNeg, coord); seg != kInvalidLaneId)
        list.push_back(seg);
    }
  }

  void TrafficLaneGraph::removeSector(const SectorCoord& coord)
  {
    auto it = m_sectorSegments.find(coord);
    if (it == m_sectorSegments.end())
      return;
    for (uint32_t segId : it->second)
    {
      if (segId < m_segments.size())
        m_segments[segId].active = false;
    }
  }

  LaneQuery TrafficLaneGraph::queryNearestLane(const float pos[3]) const
  {
    LaneQuery best{};
    best.laneId = kInvalidLaneId;
    best.distSq = 0.0f;

    float bestDist = 0.0f;
    bool hasBest = false;

    for (uint32_t i = 0; i < m_segments.size(); ++i)
    {
      const LaneSegment& seg = m_segments[i];
      if (!seg.active || seg.length <= 1e-5f)
        continue;

      const LaneNode& a = m_nodes[seg.startNode];
      const float toP[3] = { pos[0] - a.pos[0], pos[1] - a.pos[1], pos[2] - a.pos[2] };
      const float proj = toP[0] * seg.dir[0] + toP[1] * seg.dir[1] + toP[2] * seg.dir[2];
      const float s = std::max(0.0f, std::min(seg.length, proj));
      const float closest[3] = {
        a.pos[0] + seg.dir[0] * s,
        a.pos[1] + seg.dir[1] * s,
        a.pos[2] + seg.dir[2] * s
      };
      const float dx = pos[0] - closest[0];
      const float dy = pos[1] - closest[1];
      const float dz = pos[2] - closest[2];
      const float distSq = dx * dx + dy * dy + dz * dz;

      if (!hasBest || distSq < bestDist)
      {
        hasBest = true;
        bestDist = distSq;
        best.laneId = i;
        best.s = s;
        best.distSq = distSq;
      }
    }

    return best;
  }

  bool TrafficLaneGraph::getLookAheadPoint(uint32_t laneId, float s, float distance, float outPoint[3]) const
  {
    uint32_t id = laneId;
    float ss = s;
    float dir[3]{};
    if (!advanceAlongLane(id, ss, distance, outPoint, dir))
      return false;
    return true;
  }

  bool TrafficLaneGraph::advanceAlongLane(uint32_t& laneId,
                                          float& s,
                                          float distance,
                                          float outPos[3],
                                          float outDir[3]) const
  {
    if (laneId == kInvalidLaneId || laneId >= m_segments.size())
      return false;

    float remaining = distance;
    uint32_t current = laneId;
    float currentS = s;

    for (uint32_t guard = 0; guard < 8; ++guard)
    {
      const LaneSegment& seg = m_segments[current];
      if (!seg.active)
        return false;

      const float len = seg.length;
      if (len <= 1e-5f)
        return false;

      const float available = len - currentS;
      if (remaining <= available)
      {
        currentS += remaining;
        const LaneNode& a = m_nodes[seg.startNode];
        outPos[0] = a.pos[0] + seg.dir[0] * currentS;
        outPos[1] = a.pos[1] + seg.dir[1] * currentS;
        outPos[2] = a.pos[2] + seg.dir[2] * currentS;
        outDir[0] = seg.dir[0];
        outDir[1] = seg.dir[1];
        outDir[2] = seg.dir[2];
        laneId = current;
        s = currentS;
        return true;
      }

      remaining -= available;
      currentS = 0.0f;

      const LaneNode& endNode = m_nodes[seg.endNode];
      const uint32_t next = chooseNextSegment(seg.dir, endNode);
      if (next == kInvalidLaneId)
      {
        outPos[0] = endNode.pos[0];
        outPos[1] = endNode.pos[1];
        outPos[2] = endNode.pos[2];
        outDir[0] = seg.dir[0];
        outDir[1] = seg.dir[1];
        outDir[2] = seg.dir[2];
        laneId = current;
        s = len;
        return true;
      }

      current = next;
    }

    return false;
  }

  void TrafficLaneGraph::debugDrawLanes(DebugDraw& draw, bool activeOnly) const
  {
    const float activeColor[3] = { 0.2f, 0.8f, 0.9f };
    const float inactiveColor[3] = { 0.3f, 0.3f, 0.3f };

    for (const LaneSegment& seg : m_segments)
    {
      if (activeOnly && !seg.active)
        continue;
      const LaneNode& a = m_nodes[seg.startNode];
      const LaneNode& b = m_nodes[seg.endNode];
      const float* color = seg.active ? activeColor : inactiveColor;
      draw.addLine(a.pos, b.pos, color);
    }
  }

  const std::vector<uint32_t>* TrafficLaneGraph::lanesForSector(const SectorCoord& coord) const
  {
    auto it = m_sectorSegments.find(coord);
    if (it == m_sectorSegments.end())
      return nullptr;
    return &it->second;
  }

  const LaneSegment* TrafficLaneGraph::getLane(uint32_t laneId) const
  {
    if (laneId == kInvalidLaneId || laneId >= m_segments.size())
      return nullptr;
    return &m_segments[laneId];
  }

  const LaneNode* TrafficLaneGraph::getNode(uint32_t nodeId) const
  {
    if (nodeId >= m_nodes.size())
      return nullptr;
    return &m_nodes[nodeId];
  }

  float TrafficLaneGraph::laneSpeedLimit(uint32_t laneId) const
  {
    if (laneId == kInvalidLaneId || laneId >= m_segments.size())
      return m_speedLimit;
    const LaneSegment& seg = m_segments[laneId];
    if (seg.startNode >= m_nodes.size())
      return m_speedLimit;
    return m_nodes[seg.startNode].speedLimit;
  }
}
