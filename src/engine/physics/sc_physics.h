#pragma once
#include <cstdint>
#include <vector>
#include <memory>

#include "sc_ecs.h"

namespace sc
{
  class DebugDraw;

  enum class ColliderType : uint8_t
  {
    Box = 0,
    Sphere,
    Capsule
  };

  struct Collider
  {
    ColliderType type = ColliderType::Box;
    float halfExtents[3] = { 0.5f, 0.5f, 0.5f }; // Box
    float radius = 0.5f;                        // Sphere/Capsule
    float halfHeight = 0.5f;                    // Capsule (half of cylinder height)
    uint32_t layer = 1u;
    uint32_t mask = 0xFFFFFFFFu;
    bool isTrigger = false;
  };

  enum class RigidBodyType : uint8_t
  {
    Static = 0,
    Dynamic,
    Kinematic
  };

  struct RigidBody
  {
    RigidBodyType type = RigidBodyType::Dynamic;
    float mass = 1.0f;
    float friction = 0.8f;
    float restitution = 0.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.05f;
  };

  struct PhysicsBodyHandle
  {
    uint32_t id = 0;
    static constexpr uint32_t kInvalid = 0u;
    bool valid() const { return id != kInvalid; }
  };

  struct PhysicsStats
  {
    uint32_t dynamicBodies = 0;
    uint32_t kinematicBodies = 0;
    uint32_t staticColliders = 0;
    uint32_t broadphaseProxies = 0;
    float stepMs = 0.0f;
  };

  struct RaycastHit
  {
    bool hit = false;
    Entity entity = kInvalidEntity;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 1.0f, 0.0f };
    float distance = 0.0f;
    uint32_t layer = 0;
  };

  struct SweepHit
  {
    bool hit = false;
    Entity entity = kInvalidEntity;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 1.0f, 0.0f };
    float distance = 0.0f;
  };

  struct PhysicsDebugState
  {
    bool showPhysicsDebug = false;
    bool showColliders = false;
    bool pausePhysics = false;
    bool requestRaycast = false;
    bool requestResetDemo = false;
    float rayMaxDistance = 200.0f;
    uint32_t rayMask = 0xFFFFFFFFu;
    PhysicsStats stats{};
    RaycastHit lastRayHit{};
  };

  class PhysicsWorld
  {
  public:
    PhysicsWorld();
    ~PhysicsWorld();

    bool init();
    void shutdown();

    void step(float fixedDt);
    void debugDraw(DebugDraw& draw);

    PhysicsBodyHandle addRigidBody(Entity entity,
                                   const Transform& transform,
                                   const RigidBody& rb,
                                   const Collider& collider);
    PhysicsBodyHandle addStaticCollider(Entity entity,
                                        const Transform& transform,
                                        const Collider& collider);
    void removeRigidBody(PhysicsBodyHandle handle);
    void removeStaticCollider(PhysicsBodyHandle handle);

    bool setKinematicTarget(PhysicsBodyHandle handle, const Transform& transform);
    bool getBodyTransform(PhysicsBodyHandle handle, float outPos[3], float outRot[3]) const;

    RaycastHit raycast(const float origin[3], const float dir[3], float maxDist, uint32_t mask) const;
    SweepHit sweepCapsule(const float start[3], const float end[3], float radius, float halfHeight, uint32_t mask) const;

    const PhysicsStats& stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

  struct PhysicsTrackedBody
  {
    Entity entity = kInvalidEntity;
    PhysicsBodyHandle handle{};
  };

  struct PhysicsSyncState
  {
    PhysicsWorld* world = nullptr;
    PhysicsDebugState* debug = nullptr;
    std::vector<PhysicsTrackedBody> tracked;
  };

  struct PhysicsDebugDrawState
  {
    PhysicsWorld* world = nullptr;
    PhysicsDebugState* debug = nullptr;
    DebugDraw* draw = nullptr;
  };

  struct PhysicsDemoState
  {
    bool initialized = false;
    PhysicsDebugState* debug = nullptr;
    std::vector<Entity> demoEntities;
    uint32_t stackCount = 10u;
    float basePos[3] = { 0.0f, 6.0f, 0.0f };
    float spacing = 1.05f;
    uint32_t materialId = 0;
  };

  void PhysicsSyncSystem(World& world, float dt, void* user);
  void PhysicsDebugDrawSystem(World& world, float dt, void* user);
  void PhysicsDemoSystem(World& world, float dt, void* user);
}
