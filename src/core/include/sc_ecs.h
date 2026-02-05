#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <type_traits>

#include "sc_math.h"

namespace sc
{
  // --------------------
  // Entity
  // --------------------
  struct Entity
  {
    uint32_t value = 0;

    static constexpr uint32_t INDEX_BITS = 24;
    static constexpr uint32_t GENERATION_BITS = 8;
    static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1u;

    static Entity fromParts(uint32_t index, uint32_t generation)
    {
      Entity e{};
      e.value = (generation << INDEX_BITS) | (index & INDEX_MASK);
      return e;
    }

    uint32_t index() const { return value & INDEX_MASK; }
    uint32_t generation() const { return value >> INDEX_BITS; }

    bool operator==(const Entity& o) const { return value == o.value; }
    bool operator!=(const Entity& o) const { return value != o.value; }
  };

  static constexpr Entity kInvalidEntity{ 0xFFFFFFFFu };
  inline bool isValidEntity(Entity e) { return e.value != kInvalidEntity.value; }

  // --------------------
  // Entity Manager
  // --------------------
  class EntityManager
  {
  public:
    Entity create();
    bool destroy(Entity e);

    bool isAlive(Entity e) const;
    uint32_t aliveCount() const { return m_aliveCount; }
    uint32_t capacity() const { return (uint32_t)m_generations.size(); }

    void reserve(uint32_t count);

  private:
    std::vector<uint32_t> m_generations;
    std::vector<uint32_t> m_free;
    uint32_t m_aliveCount = 0;
  };

  // --------------------
  // Components
  // --------------------
  struct Transform
  {
    Entity parent = kInvalidEntity;
    float localPos[3] = { 0.0f, 0.0f, 0.0f };
    float localRot[3] = { 0.0f, 0.0f, 0.0f }; // radians
    float localScale[3] = { 1.0f, 1.0f, 1.0f };
    Mat4 worldMatrix = Mat4::identity();
    bool dirty = true;
  };

  inline void markDirty(Transform& t)
  {
    t.dirty = true;
  }

  inline void setLocal(Transform& t, const float pos[3], const float rot[3], const float scale[3])
  {
    t.localPos[0] = pos[0]; t.localPos[1] = pos[1]; t.localPos[2] = pos[2];
    t.localRot[0] = rot[0]; t.localRot[1] = rot[1]; t.localRot[2] = rot[2];
    t.localScale[0] = scale[0]; t.localScale[1] = scale[1]; t.localScale[2] = scale[2];
    t.dirty = true;
  }

  inline void setParent(Transform& t, Entity parent)
  {
    t.parent = (parent == t.parent) ? t.parent : parent;
    t.dirty = true;
  }

  inline void setLocalPosition(Transform& t, float x, float y, float z)
  {
    t.localPos[0] = x; t.localPos[1] = y; t.localPos[2] = z;
    t.dirty = true;
  }

  struct Camera
  {
    float fovY = 60.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    float aspect = 16.0f / 9.0f;
    bool active = false;
  };

  struct RenderMesh
  {
    uint32_t meshId = 0;
    uint32_t materialId = 0;
  };

  struct Name
  {
    static constexpr uint32_t kMax = 32;
    char value[kMax]{};
  };

  void setName(Name& n, const char* text);

  // --------------------
  // Render queue (CPU-side)
  // --------------------
  struct DrawItem
  {
    Entity entity{};
    uint32_t meshId = 0;
    uint32_t materialId = 0;
    Mat4 model = Mat4::identity();
  };

  struct RenderFrameData
  {
    Mat4 viewProj = Mat4::identity();
    std::vector<DrawItem> draws;
    void clear() { draws.clear(); }
    void reserve(uint32_t count) { draws.reserve(count); }
  };

  // --------------------
  // ECS Stats
  // --------------------
  struct EcsStatsSnapshot
  {
    uint32_t entityAlive = 0;
    uint32_t entityCapacity = 0;
    uint32_t transforms = 0;
    uint32_t cameras = 0;
    uint32_t renderMeshes = 0;
    uint32_t names = 0;
  };

  // --------------------
  // Component pools (SparseSet)
  // --------------------
  struct IComponentPool
  {
    virtual ~IComponentPool() = default;
    virtual void remove(Entity e) = 0;
    virtual uint32_t size() const = 0;
    virtual void reserve(uint32_t count) = 0;
  };

  template<typename T>
  class ComponentPool final : public IComponentPool
  {
  public:
    T& add(Entity e)
    {
      const uint32_t idx = e.index();
      if (idx >= (uint32_t)m_sparse.size())
        m_sparse.resize(idx + 1u, 0u);

      uint32_t slot = m_sparse[idx];
      if (slot != 0)
        return m_data[slot - 1u];

      const uint32_t denseIndex = (uint32_t)m_denseEntities.size();
      m_denseEntities.push_back(e);
      m_data.emplace_back(T{});
      m_sparse[idx] = denseIndex + 1u;
      return m_data.back();
    }

    bool has(Entity e) const
    {
      const uint32_t idx = e.index();
      if (idx >= (uint32_t)m_sparse.size())
        return false;
      const uint32_t slot = m_sparse[idx];
      return slot != 0;
    }

    T* get(Entity e)
    {
      const uint32_t idx = e.index();
      if (idx >= (uint32_t)m_sparse.size())
        return nullptr;
      const uint32_t slot = m_sparse[idx];
      if (slot == 0)
        return nullptr;
      return &m_data[slot - 1u];
    }

    void remove(Entity e) override
    {
      const uint32_t idx = e.index();
      if (idx >= (uint32_t)m_sparse.size())
        return;
      const uint32_t slot = m_sparse[idx];
      if (slot == 0)
        return;

      const uint32_t denseIndex = slot - 1u;
      const uint32_t last = (uint32_t)m_denseEntities.size() - 1u;

      if (denseIndex != last)
      {
        m_denseEntities[denseIndex] = m_denseEntities[last];
        m_data[denseIndex] = m_data[last];
        m_sparse[m_denseEntities[denseIndex].index()] = denseIndex + 1u;
      }

      m_denseEntities.pop_back();
      m_data.pop_back();
      m_sparse[idx] = 0;
    }

    uint32_t size() const override { return (uint32_t)m_denseEntities.size(); }
    void reserve(uint32_t count) override
    {
      m_denseEntities.reserve(count);
      m_data.reserve(count);
    }

    const std::vector<Entity>& denseEntities() const { return m_denseEntities; }

  private:
    std::vector<Entity> m_denseEntities;
    std::vector<T> m_data;
    std::vector<uint32_t> m_sparse;
  };

  // --------------------
  // World
  // --------------------
  class World
  {
  public:
    ~World();
    Entity create() { return m_entities.create(); }
    bool destroy(Entity e);
    bool isAlive(Entity e) const { return m_entities.isAlive(e); }

    void reserveEntities(uint32_t count);

    template<typename T, typename... Args>
    T& add(Entity e, Args&&... args)
    {
      auto* pool = getPool<T>();
      T& c = pool->add(e);
      c = T{ static_cast<Args&&>(args)... };
      return c;
    }

    template<typename T>
    bool has(Entity e) const
    {
      const auto* pool = getPoolConst<T>();
      return pool ? pool->has(e) : false;
    }

    template<typename T>
    T* get(Entity e)
    {
      auto* pool = getPool<T>();
      return pool ? pool->get(e) : nullptr;
    }

    template<typename T>
    void remove(Entity e)
    {
      auto* pool = getPool<T>();
      if (pool) pool->remove(e);
    }

    template<typename T>
    uint32_t componentCount() const
    {
      const auto* pool = getPoolConst<T>();
      return pool ? pool->size() : 0;
    }

    uint32_t entityAliveCount() const { return m_entities.aliveCount(); }
    uint32_t entityCapacity() const { return m_entities.capacity(); }

    RenderFrameData& renderFrame() { return m_renderFrame; }
    const RenderFrameData& renderFrame() const { return m_renderFrame; }

    void publishStats(const EcsStatsSnapshot& snap);
    EcsStatsSnapshot statsSnapshot() const;

    template<typename... Ts, typename F>
    void ForEach(F&& f)
    {
      forEachImpl<Ts...>(static_cast<F&&>(f));
    }

    template<typename... Ts>
    class View
    {
    public:
      explicit View(World& w) : m_world(&w) {}

      template<typename F>
      void Each(F&& f) const
      {
        m_world->template ForEach<Ts...>(static_cast<F&&>(f));
      }

    private:
      World* m_world = nullptr;
    };

    template<typename... Ts>
    View<Ts...> view() { return View<Ts...>(*this); }

  private:
    template<typename T>
    ComponentPool<T>* getPool()
    {
      const uint32_t id = componentTypeId<T>();
      if (id >= (uint32_t)m_pools.size())
        m_pools.resize(id + 1u, nullptr);
      if (!m_pools[id])
        m_pools[id] = new ComponentPool<T>();
      return static_cast<ComponentPool<T>*>(m_pools[id]);
    }

    template<typename T>
    const ComponentPool<T>* getPoolConst() const
    {
      const uint32_t id = componentTypeId<T>();
      if (id >= (uint32_t)m_pools.size())
        return nullptr;
      return static_cast<const ComponentPool<T>*>(m_pools[id]);
    }

    template<typename T>
    static uint32_t componentTypeId()
    {
      static uint32_t id = nextComponentTypeId();
      return id;
    }

    static uint32_t nextComponentTypeId();

    template<typename T0, typename... Ts, typename F>
    void forEachImpl(F&& f)
    {
      ComponentPool<T0>* driver = getPool<T0>();
      if (!driver || driver->size() == 0)
        return;

      const auto& dense = driver->denseEntities();
      for (const Entity e : dense)
      {
        if ((has<T0>(e) && ... && has<Ts>(e)))
        {
          f(e, *get<T0>(e), *get<Ts>(e)...);
        }
      }
    }

  private:
    EntityManager m_entities;
    std::vector<IComponentPool*> m_pools;

    RenderFrameData m_renderFrame;

    EcsStatsSnapshot m_stats[2]{};
    std::atomic<uint32_t> m_statsIndex{ 0 };
  };

  // --------------------
  // Systems (Phase 1.1)
  // --------------------
  struct SpawnerState
  {
    bool initialized = false;
    uint32_t frame = 0;
    uint32_t spawnCount = 256;
    uint32_t churnEvery = 120;
    uint32_t churnCount = 8;
    std::vector<Entity> active;
    Entity triangle{};
    Entity cube{};
    Entity camera{};
    Entity root{};
    bool overrideCamera = false;
    float cameraPos[3] = { 0.0f, 0.0f, 5.0f };
    float cameraRot[3] = { 0.0f, 0.0f, 0.0f };
  };

  struct RenderPrepState
  {
    RenderFrameData* frame = nullptr;
  };

  struct CameraSystemState
  {
    RenderFrameData* frame = nullptr;
    Entity activeCamera = kInvalidEntity;
    float aspect = 16.0f / 9.0f;
  };

  void TransformSystem(World& world, float dt, void* user);
  void CameraSystem(World& world, float dt, void* user);
  void RenderPrepSystem(World& world, float dt, void* user);
  void DebugSystem(World& world, float dt, void* user);
  void SpawnerSystem(World& world, float dt, void* user);
}
