#pragma once

#include "sc_ecs.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sc
{
  class DebugDraw;
  class AssetManager;

  struct Vec3
  {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
  };

  struct AABB
  {
    Vec3 min{};
    Vec3 max{};
  };

  struct Plane
  {
    float n[3] = { 0.0f, 0.0f, 0.0f };
    float d = 0.0f;
  };

  struct Frustum
  {
    Plane planes[6]{};
    bool valid = false;
  };

  struct SectorCoord
  {
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const SectorCoord& o) const { return x == o.x && z == o.z; }
    bool operator!=(const SectorCoord& o) const { return !(*this == o); }
  };

  struct SectorCoordHash
  {
    size_t operator()(const SectorCoord& c) const noexcept;
  };

  enum class SectorLoadState : uint8_t
  {
    Unloaded = 0,
    Queued = 1,
    Loading = 2,
    ReadyToActivate = 3,
    Active = 4,
    Unloading = 5
  };

  struct SpawnRecord
  {
    char name[Name::kMax]{};
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
    uint32_t meshId = 1;
    uint32_t materialId = 0;
    AABB localBounds{};
  };

  struct Sector
  {
    SectorCoord coord{};
    SectorLoadState state = SectorLoadState::Unloaded;
    uint64_t lastTouchedFrame = 0;
    uint64_t queuedAt = 0;
    uint64_t ioStart = 0;
    uint64_t ioEnd = 0;
    uint64_t activateAt = 0;
    uint32_t requestId = 0;
    uint32_t pendingDespawns = 0;
    std::vector<SpawnRecord> spawns;
    std::vector<Entity> entities;
  };

  template<typename T>
  class ConcurrentQueue
  {
  public:
    void push(T&& item)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.emplace_back(std::move(item));
    }

    bool pop(T& out)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_queue.empty())
        return false;
      out = std::move(m_queue.front());
      m_queue.pop_front();
      return true;
    }

    size_t size() const
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      return m_queue.size();
    }

    void clear()
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.clear();
    }

  private:
    mutable std::mutex m_mutex;
    std::deque<T> m_queue;
  };

  struct SectorLoadResult
  {
    SectorCoord coord{};
    uint32_t requestId = 0;
    uint64_t ioStart = 0;
    uint64_t ioEnd = 0;
    std::vector<SpawnRecord> spawns;
  };

  struct PendingDespawn
  {
    Entity entity = kInvalidEntity;
    SectorCoord coord{};
  };

  struct WorldPartitionConfig
  {
    float sectorSizeMeters = 64.0f;
    uint32_t seed = 1337u;
    uint32_t propsPerSectorMin = 12u;
    uint32_t propsPerSectorMax = 24u;
    bool includeGroundPlane = true;
  };

  struct WorldPartitionBudget
  {
    uint32_t maxActiveSectors = 25u;
    uint32_t maxEntitiesBudget = 4096u;
  };

  struct WorldPartitionFrameStats
  {
    SectorCoord cameraSector{};
    uint32_t loadRadiusSectors = 0;
    uint32_t unloadRadiusSectors = 0;
    uint32_t desiredSectors = 0;
    uint32_t loadedSectors = 0;
    uint32_t loadedThisFrame = 0;
    uint32_t unloadedThisFrame = 0;
    uint32_t estimatedSectorEntities = 0;
    uint32_t entitiesSpawned = 0;
    uint32_t entitiesDespawned = 0;
    uint32_t rejectedBySectorBudget = 0;
    uint32_t rejectedByEntityBudget = 0;
    uint32_t queuedSectors = 0;
    uint32_t loadingSectors = 0;
    uint32_t readySectors = 0;
    uint32_t activeSectors = 0;
    uint32_t unloadingSectors = 0;
    uint32_t completedLoads = 0;
    uint32_t activations = 0;
    uint32_t despawns = 0;
    float avgLoadMs = 0.0f;
    float maxLoadMs = 0.0f;
    float pumpLoadsMs = 0.0f;
    float pumpUnloadsMs = 0.0f;
  };

  class WorldPartition
  {
  public:
    WorldPartition() = default;
    explicit WorldPartition(const WorldPartitionConfig& config) { configure(config); }

    void configure(const WorldPartitionConfig& config);
    const WorldPartitionConfig& config() const { return m_config; }

    SectorCoord worldToSector(const Vec3& pos) const;
    AABB sectorBounds(const SectorCoord& coord) const;

    void setPinnedCenters(const std::vector<SectorCoord>& centers, uint32_t radius);

    Sector& ensureSectorLoaded(const SectorCoord& coord);
    bool unloadSector(const SectorCoord& coord);

    void updateActiveSet(const Vec3& cameraPos,
                         const Vec3& cameraForward,
                         uint32_t loadRadiusSectors,
                         uint32_t unloadRadiusSectors,
                         const WorldPartitionBudget& budget,
                         bool useFrustumBias,
                         float frustumBiasWeight,
                         bool allowScheduling);
    void clearFrameDeltas();
    void dispatchPendingLoads(uint32_t maxConcurrentLoads);
    void pumpCompletedLoads(World& world, const WorldPartitionBudget& budget, uint32_t maxActivationsPerFrame);
    void pumpUnloadQueue(World& world, uint32_t maxDespawnsPerFrame);
    void shutdownStreaming();

    Sector* findSector(const SectorCoord& coord);
    const Sector* findSector(const SectorCoord& coord) const;

    const std::unordered_map<SectorCoord, Sector, SectorCoordHash>& sectors() const { return m_sectors; }
    const std::vector<SectorCoord>& loadedThisFrameCoords() const { return m_loadedThisFrame; }
    const std::vector<SectorCoord>& unloadedThisFrameCoords() const { return m_unloadedThisFrame; }
    const WorldPartitionFrameStats& frameStats() const { return m_frameStats; }

    uint32_t loadedSectorCount() const { return m_activeSectorCount; }
    uint32_t loadedEntityEstimate() const { return m_activeEntityEstimate; }

  private:
    Sector& getOrCreateSector(const SectorCoord& coord);
    void generateSectorSpawns(Sector& sector);
    void markQueued(Sector& sector);
    void markActive(Sector& sector, uint32_t spawnCount);
    void markReady(Sector& sector);
    void markUnloading(Sector& sector);
    void markUnloaded(Sector& sector);
    bool readSectorFile(const SectorCoord& coord, std::vector<SpawnRecord>& outSpawns) const;
    void queueUnloadEntities(Sector& sector);
    bool isSectorDesired(const SectorCoord& coord) const;
    bool isSectorPinned(const SectorCoord& coord) const;
    float sectorPriority(const SectorCoord& coord, const SectorCoord& cameraSector, const Vec3& cameraForward, float frustumBiasWeight, bool useFrustumBias) const;

  private:
    WorldPartitionConfig m_config{};
    std::unordered_map<SectorCoord, Sector, SectorCoordHash> m_sectors;
    std::vector<SectorCoord> m_loadedThisFrame;
    std::vector<SectorCoord> m_unloadedThisFrame;
    std::vector<SectorCoord> m_scratchDesired;
    std::vector<SectorCoord> m_scratchDesiredLookup;
    std::vector<SectorCoord> m_scratchUnload;
    std::vector<SectorCoord> m_scratchLoaded;
    std::vector<SectorCoord> m_scratchReady;
    std::vector<SectorCoord> m_pinnedCenters;
    std::vector<SectorCoord> m_pinnedExpanded;
    uint32_t m_pinnedRadius = 0;
    WorldPartitionFrameStats m_frameStats{};
    uint64_t m_frameCounter = 0;
    uint32_t m_activeSectorCount = 0;
    uint32_t m_activeEntityEstimate = 0;

    ConcurrentQueue<SectorLoadResult> m_completedLoads;
    std::deque<SectorCoord> m_pendingLoads;
    std::deque<PendingDespawn> m_pendingDespawns;
    uint32_t m_inFlightLoads = 0;
    uint32_t m_requestIdCounter = 1;
    bool m_shutdown = false;
    SectorCoord m_lastCameraSector{};
    Vec3 m_lastCameraForward{};
    uint32_t m_lastLoadRadius = 0;
    uint32_t m_lastUnloadRadius = 0;
    bool m_lastUseFrustumBias = false;
    float m_lastFrustumBiasWeight = 0.0f;
  };

  struct WorldSector
  {
    SectorCoord coord{};
    bool active = true;
  };

  struct Bounds
  {
    AABB localAabb{};
  };

  struct WorldStreamingBudgets
  {
    uint32_t maxActiveSectors = 25u;
    uint32_t loadRadiusSectors = 2u;
    uint32_t unloadRadiusSectors = 3u;
    uint32_t maxEntitiesBudget = 4096u;
    uint32_t maxDrawsBudget = 4096u;
    uint32_t maxConcurrentLoads = 4u;
    uint32_t maxActivationsPerFrame = 2u;
    uint32_t maxDespawnsPerFrame = 128u;
    bool useFrustumBias = false;
    float frustumBiasWeight = 0.0f;
  };

  struct WorldStreamingState
  {
    WorldPartition partition{};
    WorldStreamingBudgets budgets{};
    WorldPartitionFrameStats stats{};
    Entity cameraEntity = kInvalidEntity;
    uint64_t frameIndex = 0;
    bool freezeStreaming = false;
    bool freezeEviction = false;
    bool showSectorBounds = true;
    bool showSectorStateColors = false;
    bool showEntityBounds = false;
    uint32_t entityBoundsLimit = 96u;
    std::vector<SectorCoord> pinnedCenters;
    uint32_t pinnedRadius = 1u;
  };

  struct CullingStats
  {
    uint32_t renderablesTotal = 0;
    uint32_t visible = 0;
    uint32_t culled = 0;
  };

  struct CullingState
  {
    RenderFrameData* frame = nullptr;
    bool freezeCulling = false;
    Frustum frustum{};
    CullingStats stats{};
    std::vector<Entity> candidates;
    std::vector<Entity> visible;
    std::vector<Entity> culled;
    std::vector<uint8_t> visibilityMask;
  };

  struct RenderPrepStats
  {
    uint32_t drawsEmitted = 0;
    uint32_t drawsDroppedByBudget = 0;
  };

  struct RenderPrepStreamingState
  {
    RenderFrameData* frame = nullptr;
    CullingState* culling = nullptr;
    WorldStreamingState* streaming = nullptr;
    AssetManager* assets = nullptr;
    RenderPrepStats stats{};
  };

  struct DebugDrawSystemState
  {
    DebugDraw* draw = nullptr;
    WorldStreamingState* streaming = nullptr;
    CullingState* culling = nullptr;
  };

  Frustum frustumFromViewProj(const Mat4& viewProj);
  bool sphereInFrustum(const Frustum& frustum, const float center[3], float radius);
  void computeWorldBoundsSphere(const Transform& transform, const Bounds& bounds, float outCenter[3], float& outRadius);

  void WorldStreamingSystem(World& world, float dt, void* user);
  void CullingSystem(World& world, float dt, void* user);
  void RenderPrepStreamingSystem(World& world, float dt, void* user);
}
