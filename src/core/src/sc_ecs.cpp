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
    m_renderFrame.reserve(count);
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

    std::vector<Entity> entities;
    world.ForEach<Transform>([&](Entity e, Transform&)
    {
      entities.push_back(e);
    });

    if (entities.empty())
      return;

    uint32_t maxIndex = 0;
    for (const Entity e : entities)
      maxIndex = (e.index() > maxIndex) ? e.index() : maxIndex;

    std::vector<std::vector<Entity>> children(maxIndex + 1u);
    std::vector<Entity> roots;
    roots.reserve(entities.size());

    for (const Entity e : entities)
    {
      Transform& t = *world.get<Transform>(e);

      if (t.localScale[0] == 0.0f && t.localScale[1] == 0.0f && t.localScale[2] == 0.0f)
      {
        t.localScale[0] = 1.0f;
        t.localScale[1] = 1.0f;
        t.localScale[2] = 1.0f;
        t.dirty = true;
      }

      bool validParent = isValidEntity(t.parent) && t.parent != e &&
                         world.isAlive(t.parent) && world.has<Transform>(t.parent);

      if (!validParent)
      {
        if (isValidEntity(t.parent))
          t.dirty = true;
        t.parent = kInvalidEntity;
        roots.push_back(e);
      }
      else
      {
        children[t.parent.index()].push_back(e);
      }
    }

    struct StackItem
    {
      Entity e{};
      bool parentDirty = false;
    };

    std::vector<StackItem> stack;
    stack.reserve(entities.size());
    for (const Entity r : roots)
      stack.push_back({ r, false });

    while (!stack.empty())
    {
      const StackItem item = stack.back();
      stack.pop_back();

      Transform& t = *world.get<Transform>(item.e);
      const bool nodeDirty = t.dirty || item.parentDirty;

      if (nodeDirty)
      {
        const Mat4 local = mat4_trs(t.localPos, t.localRot, t.localScale);
        if (isValidEntity(t.parent))
        {
          Transform* pt = world.get<Transform>(t.parent);
          if (pt)
            t.worldMatrix = mat4_mul(pt->worldMatrix, local);
          else
            t.worldMatrix = local;
        }
        else
        {
          t.worldMatrix = local;
        }
        t.dirty = false;
      }

      const uint32_t idx = item.e.index();
      if (idx < children.size())
      {
        for (const Entity c : children[idx])
          stack.push_back({ c, nodeDirty });
      }
    }
  }

  void CameraSystem(World& world, float dt, void* user)
  {
    (void)dt;
    CameraSystemState* state = static_cast<CameraSystemState*>(user);
    if (!state || !state->frame)
      return;

    RenderFrameData& frame = *state->frame;

    Camera* activeCam = nullptr;
    Transform* camXform = nullptr;
    Entity activeEntity = kInvalidEntity;

    Camera* fallbackCam = nullptr;
    Transform* fallbackXform = nullptr;
    Entity fallbackEntity = kInvalidEntity;

    world.ForEach<Camera, Transform>([&](Entity e, Camera& cam, Transform& tr)
    {
      if (!fallbackCam)
      {
        fallbackCam = &cam;
        fallbackXform = &tr;
        fallbackEntity = e;
      }

      if (!activeCam && cam.active)
      {
        activeCam = &cam;
        camXform = &tr;
        activeEntity = e;
      }
    });

    if (!activeCam && fallbackCam)
    {
      activeCam = fallbackCam;
      camXform = fallbackXform;
      activeEntity = fallbackEntity;
    }

    if (!activeCam)
    {
      frame.viewProj = Mat4::identity();
      state->activeCamera = kInvalidEntity;
      return;
    }

    activeCam->aspect = (state->aspect > 0.0f) ? state->aspect : activeCam->aspect;

    const float fovRad = activeCam->fovY * 3.1415926535f / 180.0f;
    const Mat4 proj = mat4_perspective_rh_zo(fovRad, activeCam->aspect, activeCam->nearZ, activeCam->farZ, true);

    // Conventions: right-handed, camera looks along -Z in view space, depth 0..1 (Vulkan).
    // We flip Y in the projection to keep +Y up in world space.
    const Mat4 view = camXform ? mat4_inverse(camXform->worldMatrix) : Mat4::identity();

    frame.viewProj = mat4_mul(proj, view);
    state->activeCamera = activeEntity;
  }

  void RenderPrepSystem(World& world, float dt, void* user)
  {
    (void)dt;
    RenderPrepState* state = static_cast<RenderPrepState*>(user);
    if (!state || !state->frame)
      return;

    RenderFrameData& frame = *state->frame;
    frame.clear();

    world.ForEach<Transform, RenderMesh>([&](Entity e, Transform& t, RenderMesh& rm)
    {
      DrawItem cmd{};
      cmd.entity = e;
      cmd.meshId = rm.meshId;
      cmd.materialId = rm.materialId;
      cmd.model = t.worldMatrix;

      frame.draws.push_back(cmd);
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

      // Camera entity
      state->camera = world.create();
      {
        Transform& t = world.add<Transform>(state->camera);
        setLocalPosition(t, 0.0f, 0.0f, 5.0f);
        Camera& cam = world.add<Camera>(state->camera);
        cam.active = true;
        setName(world.add<Name>(state->camera), "MainCamera");
      }

      // Triangle entity (RenderMesh)
      state->root = world.create();
      {
        Transform& t = world.add<Transform>(state->root);
        setLocalPosition(t, 0.0f, 0.0f, 0.0f);
        setName(world.add<Name>(state->root), "Root");
      }

      state->triangle = world.create();
      {
        Transform& t = world.add<Transform>(state->triangle);
        setParent(t, state->root);
        setLocalPosition(t, 0.0f, 0.0f, 2.0f);
      }
      {
        RenderMesh& rm = world.add<RenderMesh>(state->triangle);
        rm.meshId = 0;
        rm.materialId = 0;
      }
      setName(world.add<Name>(state->triangle), "TriangleEntity");

      state->cube = world.create();
      {
        Transform& t = world.add<Transform>(state->cube);
        setParent(t, state->root);
        setLocalPosition(t, 0.0f, 0.0f, 0.0f);
      }
      {
        RenderMesh& rm = world.add<RenderMesh>(state->cube);
        rm.meshId = 1;
        rm.materialId = 0;
      }
      setName(world.add<Name>(state->cube), "CubeEntity");

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
