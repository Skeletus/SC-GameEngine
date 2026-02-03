#include "sc_ecs.h"
#include "sc_time.h"

#include <cstring>

namespace sc
{
  // --------------------
  // EntityManager
  // --------------------
  Entity EntityManager::create()
  {
    if (!m_free.empty())
    {
      const uint32_t idx = m_free.back();
      m_free.pop_back();
      const uint32_t gen = m_generations[idx];
      m_aliveCount++;
      return Entity::fromParts(idx, gen);
    }

    const uint32_t idx = (uint32_t)m_generations.size();
    m_generations.push_back(0);
    m_aliveCount++;
    return Entity::fromParts(idx, 0);
  }

  bool EntityManager::destroy(Entity e)
  {
    const uint32_t idx = e.index();
    if (idx >= m_generations.size())
      return false;
    const uint32_t gen = m_generations[idx];
    if (gen != e.generation())
      return false;

    m_generations[idx] = gen + 1u;
    m_free.push_back(idx);
    if (m_aliveCount > 0)
      m_aliveCount--;
    return true;
  }

  bool EntityManager::isAlive(Entity e) const
  {
    const uint32_t idx = e.index();
    if (idx >= m_generations.size())
      return false;
    return m_generations[idx] == e.generation();
  }

  void EntityManager::reserve(uint32_t count)
  {
    m_generations.reserve(count);
    m_free.reserve(count / 4u);
  }

  // --------------------
  // Utils
  // --------------------
  void setName(Name& n, const char* text)
  {
    if (!text) { n.value[0] = '\0'; return; }
    strncpy_s(n.value, Name::kMax, text, Name::kMax - 1);
  }

  uint32_t World::nextComponentTypeId()
  {
    static std::atomic<uint32_t> counter{ 0 };
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

  World::~World()
  {
    for (IComponentPool* p : m_pools)
      delete p;
    m_pools.clear();
  }

  bool World::destroy(Entity e)
  {
    if (!m_entities.destroy(e))
      return false;

    for (IComponentPool* p : m_pools)
    {
      if (p) p->remove(e);
    }
    return true;
  }

  void World::reserveEntities(uint32_t count)
  {
    m_entities.reserve(count);
    for (IComponentPool* p : m_pools)
    {
      if (p) p->reserve(count);
    }
    m_renderQueue.reserve(count);
  }

  void World::publishStats(const EcsStatsSnapshot& snap)
  {
    const uint32_t back = 1u - m_statsIndex.load(std::memory_order_relaxed);
    m_stats[back] = snap;
    m_statsIndex.store(back, std::memory_order_release);
  }

  EcsStatsSnapshot World::statsSnapshot() const
  {
    const uint32_t idx = m_statsIndex.load(std::memory_order_acquire);
    return m_stats[idx];
  }

  // --------------------
  // Systems
  // --------------------
  void TransformSystem(World& world, float dt, void* user)
  {
    (void)dt; (void)user;
    world.ForEach<Transform>([](Entity, Transform& t)
    {
      if (t.scale[0] == 0.0f && t.scale[1] == 0.0f && t.scale[2] == 0.0f)
      {
        t.scale[0] = 1.0f;
        t.scale[1] = 1.0f;
        t.scale[2] = 1.0f;
      }
    });
  }

  void RenderPrepSystem(World& world, float dt, void* user)
  {
    (void)dt;
    RenderPrepState* state = static_cast<RenderPrepState*>(user);
    if (!state || !state->queue)
      return;

    RenderQueue& q = *state->queue;
    q.clear();

    world.ForEach<Transform, RenderMesh>([&](Entity e, Transform&, RenderMesh& rm)
    {
      DrawCommand cmd{};
      cmd.entity = e;
      cmd.meshId = rm.meshId;
      cmd.materialId = rm.materialId;
      q.draws.push_back(cmd);
    });
  }

  void DebugSystem(World& world, float dt, void* user)
  {
    (void)dt; (void)user;
    EcsStatsSnapshot snap{};
    snap.entityAlive = world.entityAliveCount();
    snap.entityCapacity = world.entityCapacity();
    snap.transforms = world.componentCount<Transform>();
    snap.cameras = world.componentCount<Camera>();
    snap.renderMeshes = world.componentCount<RenderMesh>();
    snap.names = world.componentCount<Name>();
    world.publishStats(snap);
  }

  void SpawnerSystem(World& world, float dt, void* user)
  {
    (void)dt;
    SpawnerState* state = static_cast<SpawnerState*>(user);
    if (!state)
      return;

    if (!state->initialized)
    {
      state->active.reserve(state->spawnCount);

      // Triangle entity (RenderMesh)
      state->triangle = world.create();
      world.add<Transform>(state->triangle);
      world.add<RenderMesh>(state->triangle).meshId = 0;
      setName(world.add<Name>(state->triangle), "TriangleEntity");

      for (uint32_t i = 0; i < state->spawnCount; ++i)
      {
        Entity e = world.create();
        world.add<Transform>(e);
        setName(world.add<Name>(e), "Actor");
        state->active.push_back(e);
      }

      state->initialized = true;
      return;
    }

    state->frame++;
    if (state->churnEvery == 0 || (state->frame % state->churnEvery) != 0)
      return;

    const uint32_t churn = state->churnCount;
    for (uint32_t i = 0; i < churn && !state->active.empty(); ++i)
    {
      Entity e = state->active.back();
      state->active.pop_back();
      world.destroy(e);
    }

    for (uint32_t i = 0; i < churn; ++i)
    {
      Entity e = world.create();
      world.add<Transform>(e);
      setName(world.add<Name>(e), "Actor");
      state->active.push_back(e);
    }
  }
}
