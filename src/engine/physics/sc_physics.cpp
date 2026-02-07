#include "sc_physics.h"
#include "sc_debug_draw.h"
#include "sc_log.h"
#include "sc_time.h"
#include "sc_world_partition.h"

#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/Vehicle/btRaycastVehicle.h>

#include <algorithm>
#include <cmath>

namespace sc
{
  namespace
  {
    static btVector3 toBt(const float v[3])
    {
      return btVector3(v[0], v[1], v[2]);
    }

    static btQuaternion quatFromEuler(const float rot[3])
    {
      btQuaternion q;
      q.setEulerZYX(rot[2], rot[1], rot[0]);
      return q;
    }

    static void eulerFromQuat(const btQuaternion& q, float outRot[3])
    {
      btScalar yaw = 0, pitch = 0, roll = 0;
      q.getEulerZYX(yaw, pitch, roll);
      outRot[0] = static_cast<float>(roll);
      outRot[1] = static_cast<float>(pitch);
      outRot[2] = static_cast<float>(yaw);
    }

    static void extractScale(const Transform& t, float out[3])
    {
      out[0] = std::fabs(t.localScale[0]);
      out[1] = std::fabs(t.localScale[1]);
      out[2] = std::fabs(t.localScale[2]);
      for (int i = 0; i < 3; ++i)
      {
        if (out[i] < 1e-6f)
          out[i] = 1.0f;
      }
    }

    static Entity entityFromUserPointer(const void* ptr)
    {
      const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
      if (value == 0)
        return kInvalidEntity;
      Entity e{};
      e.value = static_cast<uint32_t>(value - 1u);
      return e;
    }

    class BulletDebugDrawer final : public btIDebugDraw
    {
    public:
      void setDraw(DebugDraw* draw) { m_draw = draw; }

      void drawLine(const btVector3& from, const btVector3& to, const btVector3& color) override
      {
        if (!m_draw)
          return;
        const float p0[3] = { from.x(), from.y(), from.z() };
        const float p1[3] = { to.x(), to.y(), to.z() };
        const float c[3] = { color.x(), color.y(), color.z() };
        m_draw->addLine(p0, p1, c);
      }

      void drawContactPoint(const btVector3&, const btVector3&, btScalar, int, const btVector3&) override {}
      void reportErrorWarning(const char* warningString) override
      {
        if (warningString)
          sc::log(sc::LogLevel::Warn, "Bullet: %s", warningString);
      }
      void draw3dText(const btVector3&, const char*) override {}
      void setDebugMode(int debugMode) override { m_debugMode = debugMode; }
      int getDebugMode() const override { return m_debugMode; }

    private:
      DebugDraw* m_draw = nullptr;
      int m_debugMode = DBG_DrawWireframe;
    };

    struct BodyRecord
    {
      bool active = false;
      Entity entity = kInvalidEntity;
      RigidBodyType type = RigidBodyType::Static;
      btRigidBody* body = nullptr;
      btCollisionShape* shape = nullptr;
      btCollisionShape* childShape = nullptr;
      btMotionState* motion = nullptr;
      uint32_t layer = 0;
      uint32_t mask = 0;
    };
  }

  struct PhysicsWorld::Impl
  {
    btBroadphaseInterface* broadphase = nullptr;
    btDefaultCollisionConfiguration* collisionConfig = nullptr;
    btCollisionDispatcher* dispatcher = nullptr;
    btSequentialImpulseConstraintSolver* solver = nullptr;
    btDiscreteDynamicsWorld* world = nullptr;
    BulletDebugDrawer debugDrawer{};

    std::vector<BodyRecord> bodies;
    std::vector<uint32_t> freeList;

    struct VehicleRecord
    {
      bool active = false;
      PhysicsBodyHandle chassis{};
      btRaycastVehicle* vehicle = nullptr;
      btVehicleRaycaster* raycaster = nullptr;
      uint32_t wheelCount = 0;
      bool front[kMaxVehicleWheels]{};
      float baseFrictionSlip = 1.2f;
    };

    std::vector<VehicleRecord> vehicles;
    std::vector<uint32_t> vehicleFreeList;

    PhysicsStats stats{};
    uint32_t dynamicCount = 0;
    uint32_t kinematicCount = 0;
    uint32_t staticCount = 0;
  };

  static btCollisionShape* createShape(const Collider& collider, const Transform& transform)
  {
    float scale[3]{ 1.0f, 1.0f, 1.0f };
    extractScale(transform, scale);

    switch (collider.type)
    {
      case ColliderType::Box:
      {
        const float hx = collider.halfExtents[0] * scale[0];
        const float hy = collider.halfExtents[1] * scale[1];
        const float hz = collider.halfExtents[2] * scale[2];
        return new btBoxShape(btVector3(hx, hy, hz));
      }
      case ColliderType::Sphere:
      {
        const float r = collider.radius * std::max(scale[0], std::max(scale[1], scale[2]));
        return new btSphereShape(r);
      }
      case ColliderType::Capsule:
      {
        const float r = collider.radius * std::max(scale[0], scale[2]);
        const float h = std::max(0.0f, collider.halfHeight * 2.0f * scale[1]);
        return new btCapsuleShape(r, h);
      }
      default:
        break;
    }

    return new btBoxShape(btVector3(0.5f, 0.5f, 0.5f));
  }

  static btCollisionShape* createShapeWithComOffset(const Collider& collider,
                                                    const Transform& transform,
                                                    const float comOffset[3],
                                                    btCollisionShape*& outChildShape)
  {
    outChildShape = createShape(collider, transform);
    if (!outChildShape)
      return nullptr;

    const float ox = comOffset ? comOffset[0] : 0.0f;
    const float oy = comOffset ? comOffset[1] : 0.0f;
    const float oz = comOffset ? comOffset[2] : 0.0f;
    if (std::fabs(ox) < 1e-6f && std::fabs(oy) < 1e-6f && std::fabs(oz) < 1e-6f)
      return outChildShape;

    btCompoundShape* compound = new btCompoundShape();
    btTransform local;
    local.setIdentity();
    local.setOrigin(btVector3(-ox, -oy, -oz));
    compound->addChildShape(local, outChildShape);
    return compound;
  }

  static btTransform makeTransform(const Transform& transform)
  {
    btTransform t;
    t.setIdentity();
    t.setOrigin(btVector3(transform.localPos[0], transform.localPos[1], transform.localPos[2]));
    t.setRotation(quatFromEuler(transform.localRot));
    return t;
  }

  PhysicsWorld::PhysicsWorld()
    : m_impl(std::make_unique<Impl>())
  {
  }

  PhysicsWorld::~PhysicsWorld()
  {
    shutdown();
  }

  bool PhysicsWorld::init()
  {
    if (!m_impl)
      m_impl = std::make_unique<Impl>();

    if (m_impl->world)
      return true;

    m_impl->broadphase = new btDbvtBroadphase();
    m_impl->collisionConfig = new btDefaultCollisionConfiguration();
    m_impl->dispatcher = new btCollisionDispatcher(m_impl->collisionConfig);
    m_impl->solver = new btSequentialImpulseConstraintSolver();
    m_impl->world = new btDiscreteDynamicsWorld(m_impl->dispatcher,
                                                m_impl->broadphase,
                                                m_impl->solver,
                                                m_impl->collisionConfig);
    m_impl->world->setGravity(btVector3(0.0f, -9.81f, 0.0f));
    m_impl->world->setDebugDrawer(&m_impl->debugDrawer);
    return true;
  }

  void PhysicsWorld::shutdown()
  {
    if (!m_impl || !m_impl->world)
      return;

    for (uint32_t i = 0; i < m_impl->vehicles.size(); ++i)
    {
      auto& vr = m_impl->vehicles[i];
      if (!vr.active)
        continue;
      if (vr.vehicle && m_impl->world)
        m_impl->world->removeVehicle(vr.vehicle);
      delete vr.vehicle;
      delete vr.raycaster;
      vr = {};
    }
    m_impl->vehicles.clear();
    m_impl->vehicleFreeList.clear();

    for (uint32_t i = 0; i < m_impl->bodies.size(); ++i)
    {
      BodyRecord& rec = m_impl->bodies[i];
      if (!rec.active || !rec.body)
        continue;
      m_impl->world->removeRigidBody(rec.body);
      delete rec.motion;
      delete rec.body;
      if (rec.shape != rec.childShape)
        delete rec.shape;
      delete rec.childShape;
      rec = BodyRecord{};
    }

    m_impl->bodies.clear();
    m_impl->freeList.clear();
    m_impl->dynamicCount = 0;
    m_impl->kinematicCount = 0;
    m_impl->staticCount = 0;

    delete m_impl->world;
    delete m_impl->solver;
    delete m_impl->dispatcher;
    delete m_impl->collisionConfig;
    delete m_impl->broadphase;

    m_impl->world = nullptr;
    m_impl->solver = nullptr;
    m_impl->dispatcher = nullptr;
    m_impl->collisionConfig = nullptr;
    m_impl->broadphase = nullptr;
  }

  void PhysicsWorld::step(float fixedDt)
  {
    if (!m_impl || !m_impl->world)
      return;

    const Tick start = nowTicks();
    m_impl->world->stepSimulation(fixedDt, 0, fixedDt);
    const Tick end = nowTicks();

    m_impl->stats.dynamicBodies = m_impl->dynamicCount;
    m_impl->stats.kinematicBodies = m_impl->kinematicCount;
    m_impl->stats.staticColliders = m_impl->staticCount;

    if (m_impl->world->getBroadphase() && m_impl->world->getBroadphase()->getOverlappingPairCache())
      m_impl->stats.broadphaseProxies = (uint32_t)m_impl->world->getBroadphase()->getOverlappingPairCache()->getNumOverlappingPairs();
    else
      m_impl->stats.broadphaseProxies = m_impl->dynamicCount + m_impl->staticCount + m_impl->kinematicCount;

    m_impl->stats.stepMs = (float)(ticksToSeconds(end - start) * 1000.0);
  }

  void PhysicsWorld::debugDraw(DebugDraw& draw)
  {
    if (!m_impl || !m_impl->world)
      return;
    m_impl->debugDrawer.setDraw(&draw);
    m_impl->debugDrawer.setDebugMode(btIDebugDraw::DBG_DrawWireframe);
    m_impl->world->debugDrawWorld();
    m_impl->debugDrawer.setDraw(nullptr);
  }

  PhysicsBodyHandle PhysicsWorld::addRigidBody(Entity entity,
                                               const Transform& transform,
                                               const RigidBody& rb,
                                               const Collider& collider)
  {
    if (!m_impl || !m_impl->world)
      return {};

    btCollisionShape* shape = createShape(collider, transform);
    if (!shape)
      return {};

    btTransform startTransform = makeTransform(transform);

    btVector3 localInertia(0, 0, 0);
    btScalar mass = (rb.type == RigidBodyType::Dynamic) ? btScalar(rb.mass) : btScalar(0.0f);
    if (mass > 0.0f)
      shape->calculateLocalInertia(mass, localInertia);

    btDefaultMotionState* motion = new btDefaultMotionState(startTransform);
    btRigidBody::btRigidBodyConstructionInfo info(mass, motion, shape, localInertia);
    info.m_friction = rb.friction;
    info.m_restitution = rb.restitution;
    btRigidBody* body = new btRigidBody(info);
    body->setDamping(rb.linearDamping, rb.angularDamping);

    if (rb.type == RigidBodyType::Kinematic)
    {
      body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
      body->setActivationState(DISABLE_DEACTIVATION);
    }

    if (collider.isTrigger)
      body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);

    uint32_t index = 0;
    if (!m_impl->freeList.empty())
    {
      index = m_impl->freeList.back();
      m_impl->freeList.pop_back();
    }
    else
    {
      index = static_cast<uint32_t>(m_impl->bodies.size());
      m_impl->bodies.emplace_back();
    }

    BodyRecord& rec = m_impl->bodies[index];
    rec.active = true;
    rec.entity = entity;
    rec.type = rb.type;
    rec.body = body;
    rec.shape = shape;
    rec.childShape = shape;
    rec.motion = motion;
    rec.layer = collider.layer;
    rec.mask = collider.mask;

    // Default collision setup:
    // - Dynamic bodies stay on layer 1 with full mask.
    // - Static bodies move to layer 2 and only collide with layer 1.
    if (rb.type == RigidBodyType::Static && rec.layer == 1u && rec.mask == 0xFFFFFFFFu)
    {
      rec.layer = 2u;
      rec.mask = 1u;
    }

    const PhysicsBodyHandle handle{ index + 1u };
    body->setUserPointer(reinterpret_cast<void*>(static_cast<uintptr_t>(entity.value + 1u)));
    body->setUserIndex(static_cast<int>(handle.id));

    m_impl->world->addRigidBody(body, (int)rec.layer, (int)rec.mask);

    if (rb.type == RigidBodyType::Dynamic) m_impl->dynamicCount++;
    else if (rb.type == RigidBodyType::Kinematic) m_impl->kinematicCount++;
    else m_impl->staticCount++;

    return handle;
  }

  PhysicsBodyHandle PhysicsWorld::addStaticCollider(Entity entity,
                                                    const Transform& transform,
                                                    const Collider& collider)
  {
    RigidBody rb{};
    rb.type = RigidBodyType::Static;
    rb.mass = 0.0f;
    return addRigidBody(entity, transform, rb, collider);
  }

  PhysicsBodyHandle PhysicsWorld::addRigidBodyWithComOffset(Entity entity,
                                                            const Transform& transform,
                                                            const RigidBody& rb,
                                                            const Collider& collider,
                                                            const float comOffset[3])
  {
    if (!m_impl || !m_impl->world)
      return {};

    btCollisionShape* childShape = nullptr;
    btCollisionShape* shape = createShapeWithComOffset(collider, transform, comOffset, childShape);
    if (!shape)
      return {};

    btTransform startTransform = makeTransform(transform);

    btVector3 localInertia(0, 0, 0);
    btScalar mass = (rb.type == RigidBodyType::Dynamic) ? btScalar(rb.mass) : btScalar(0.0f);
    if (mass > 0.0f)
      shape->calculateLocalInertia(mass, localInertia);

    btDefaultMotionState* motion = new btDefaultMotionState(startTransform);
    btRigidBody::btRigidBodyConstructionInfo info(mass, motion, shape, localInertia);
    info.m_friction = rb.friction;
    info.m_restitution = rb.restitution;
    btRigidBody* body = new btRigidBody(info);
    body->setDamping(rb.linearDamping, rb.angularDamping);

    if (rb.type == RigidBodyType::Kinematic)
    {
      body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
      body->setActivationState(DISABLE_DEACTIVATION);
    }

    if (collider.isTrigger)
      body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);

    uint32_t index = 0;
    if (!m_impl->freeList.empty())
    {
      index = m_impl->freeList.back();
      m_impl->freeList.pop_back();
    }
    else
    {
      index = static_cast<uint32_t>(m_impl->bodies.size());
      m_impl->bodies.emplace_back();
    }

    BodyRecord& rec = m_impl->bodies[index];
    rec.active = true;
    rec.entity = entity;
    rec.type = rb.type;
    rec.body = body;
    rec.shape = shape;
    rec.childShape = childShape;
    rec.motion = motion;
    rec.layer = collider.layer;
    rec.mask = collider.mask;

    if (rb.type == RigidBodyType::Static && rec.layer == 1u && rec.mask == 0xFFFFFFFFu)
    {
      rec.layer = 2u;
      rec.mask = 1u;
    }

    const PhysicsBodyHandle handle{ index + 1u };
    body->setUserPointer(reinterpret_cast<void*>(static_cast<uintptr_t>(entity.value + 1u)));
    body->setUserIndex(static_cast<int>(handle.id));

    m_impl->world->addRigidBody(body, (int)rec.layer, (int)rec.mask);

    if (rb.type == RigidBodyType::Dynamic) m_impl->dynamicCount++;
    else if (rb.type == RigidBodyType::Kinematic) m_impl->kinematicCount++;
    else m_impl->staticCount++;

    return handle;
  }

  void PhysicsWorld::removeRigidBody(PhysicsBodyHandle handle)
  {
    if (!m_impl || !m_impl->world)
      return;

    if (!handle.valid())
      return;

    for (uint32_t i = 0; i < m_impl->vehicles.size(); ++i)
    {
      auto& vr = m_impl->vehicles[i];
      if (vr.active && vr.chassis.id == handle.id)
      {
        VehicleHandle vh{ i + 1u };
        removeRaycastVehicle(vh);
      }
    }

    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return;

    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return;

    m_impl->world->removeRigidBody(rec.body);
    delete rec.motion;
    delete rec.body;
    if (rec.shape != rec.childShape)
      delete rec.shape;
    delete rec.childShape;

    if (rec.type == RigidBodyType::Dynamic && m_impl->dynamicCount > 0) m_impl->dynamicCount--;
    else if (rec.type == RigidBodyType::Kinematic && m_impl->kinematicCount > 0) m_impl->kinematicCount--;
    else if (m_impl->staticCount > 0) m_impl->staticCount--;

    rec = BodyRecord{};
    m_impl->freeList.push_back(idx);
  }

  void PhysicsWorld::removeStaticCollider(PhysicsBodyHandle handle)
  {
    removeRigidBody(handle);
  }

  bool PhysicsWorld::setKinematicTarget(PhysicsBodyHandle handle, const Transform& transform)
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    if (rec.type != RigidBodyType::Kinematic)
      return false;

    btTransform t = makeTransform(transform);
    rec.body->setWorldTransform(t);
    if (rec.motion)
      rec.motion->setWorldTransform(t);
    rec.body->activate(true);
    return true;
  }

  bool PhysicsWorld::getBodyTransform(PhysicsBodyHandle handle, float outPos[3], float outRot[3]) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;

    btTransform t;
    if (rec.motion)
      rec.motion->getWorldTransform(t);
    else
      t = rec.body->getWorldTransform();

    const btVector3 p = t.getOrigin();
    outPos[0] = p.x();
    outPos[1] = p.y();
    outPos[2] = p.z();

    const btQuaternion q = t.getRotation();
    eulerFromQuat(q, outRot);
    return true;
  }

  bool PhysicsWorld::isBodyActive(PhysicsBodyHandle handle) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    return rec.body->isActive();
  }

  void PhysicsWorld::activateBody(PhysicsBodyHandle handle)
  {
    if (!m_impl)
      return;
    if (!handle.valid())
      return;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return;
    rec.body->activate(true);
  }

  bool PhysicsWorld::isBodyInWorld(PhysicsBodyHandle handle) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    return rec.body->isInWorld();
  }

  bool PhysicsWorld::getBodyType(PhysicsBodyHandle handle, RigidBodyType& outType) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    outType = rec.type;
    return true;
  }

  bool PhysicsWorld::getBodyMass(PhysicsBodyHandle handle, float& outMass) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    const btScalar inv = rec.body->getInvMass();
    outMass = (inv > 0.0f) ? (1.0f / inv) : 0.0f;
    return true;
  }

  bool PhysicsWorld::getBodyLinearVelocity(PhysicsBodyHandle handle, float outVel[3]) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    const btVector3 v = rec.body->getLinearVelocity();
    outVel[0] = v.x();
    outVel[1] = v.y();
    outVel[2] = v.z();
    return true;
  }

  bool PhysicsWorld::getBodyCollisionFlags(PhysicsBodyHandle handle, uint32_t& outFlags) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->bodies.size())
      return false;
    BodyRecord& rec = m_impl->bodies[idx];
    if (!rec.active || !rec.body)
      return false;
    outFlags = static_cast<uint32_t>(rec.body->getCollisionFlags());
    return true;
  }

  bool PhysicsWorld::isVehicleInWorld(VehicleHandle handle) const
  {
    if (!m_impl || !m_impl->world)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return false;
    const Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active || !rec.vehicle)
      return false;
    return rec.vehicle->getRigidBody()->isInWorld();
  }

  bool PhysicsWorld::getVehicleSpeedKmh(VehicleHandle handle, float& outSpeed) const
  {
    if (!m_impl)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return false;
    const Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active || !rec.vehicle)
      return false;
    outSpeed = (float)rec.vehicle->getCurrentSpeedKmHour();
    return true;
  }

  uint32_t PhysicsWorld::getVehicleWheelCount(VehicleHandle handle) const
  {
    if (!m_impl)
      return 0u;
    if (!handle.valid())
      return 0u;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return 0u;
    const Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active)
      return 0u;
    return rec.wheelCount;
  }

  RaycastHit PhysicsWorld::raycast(const float origin[3], const float dir[3], float maxDist, uint32_t mask) const
  {
    RaycastHit out{};
    if (!m_impl || !m_impl->world)
      return out;

    const float lenSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (lenSq <= 1e-6f)
      return out;

    const float invLen = 1.0f / std::sqrt(lenSq);
    const float ndir[3] = { dir[0] * invLen, dir[1] * invLen, dir[2] * invLen };

    const btVector3 from(origin[0], origin[1], origin[2]);
    const btVector3 to(origin[0] + ndir[0] * maxDist,
                       origin[1] + ndir[1] * maxDist,
                       origin[2] + ndir[2] * maxDist);

    btCollisionWorld::ClosestRayResultCallback cb(from, to);
    cb.m_collisionFilterGroup = 0xFFFF;
    cb.m_collisionFilterMask = (int)mask;
    m_impl->world->rayTest(from, to, cb);

    if (cb.hasHit())
    {
      out.hit = true;
      out.distance = maxDist * cb.m_closestHitFraction;
      const btVector3 hp = cb.m_hitPointWorld;
      const btVector3 hn = cb.m_hitNormalWorld;
      out.position[0] = hp.x(); out.position[1] = hp.y(); out.position[2] = hp.z();
      out.normal[0] = hn.x(); out.normal[1] = hn.y(); out.normal[2] = hn.z();
      out.entity = entityFromUserPointer(cb.m_collisionObject->getUserPointer());
      if (cb.m_collisionObject->getBroadphaseHandle())
        out.layer = (uint32_t)cb.m_collisionObject->getBroadphaseHandle()->m_collisionFilterGroup;
    }

    return out;
  }

  SweepHit PhysicsWorld::sweepCapsule(const float start[3], const float end[3], float radius, float halfHeight, uint32_t mask) const
  {
    SweepHit out{};
    if (!m_impl || !m_impl->world)
      return out;

    btCapsuleShape capsule(radius, std::max(0.0f, halfHeight * 2.0f));
    btTransform from;
    btTransform to;
    from.setIdentity();
    to.setIdentity();
    from.setOrigin(toBt(start));
    to.setOrigin(toBt(end));

    btCollisionWorld::ClosestConvexResultCallback cb(from.getOrigin(), to.getOrigin());
    cb.m_collisionFilterGroup = 0xFFFF;
    cb.m_collisionFilterMask = (int)mask;
    m_impl->world->convexSweepTest(&capsule, from, to, cb);

    if (cb.hasHit())
    {
      out.hit = true;
      out.distance = (float)cb.m_closestHitFraction;
      const btVector3 hp = cb.m_hitPointWorld;
      const btVector3 hn = cb.m_hitNormalWorld;
      out.position[0] = hp.x(); out.position[1] = hp.y(); out.position[2] = hp.z();
      out.normal[0] = hn.x(); out.normal[1] = hn.y(); out.normal[2] = hn.z();
      out.entity = entityFromUserPointer(cb.m_hitCollisionObject->getUserPointer());
    }

    return out;
  }

  VehicleHandle PhysicsWorld::createRaycastVehicle(PhysicsBodyHandle chassis,
                                                   const VehicleComponent& vehicle,
                                                   const VehicleWheelConfig* wheels,
                                                   uint32_t wheelCount)
  {
    if (!m_impl || !m_impl->world)
      return {};
    if (!chassis.valid())
      return {};
    if (!wheels || wheelCount == 0)
      return {};

    const uint32_t bodyIndex = chassis.id - 1u;
    if (bodyIndex >= m_impl->bodies.size())
      return {};
    BodyRecord& bodyRec = m_impl->bodies[bodyIndex];
    if (!bodyRec.active || !bodyRec.body)
      return {};
    if (bodyRec.type != RigidBodyType::Dynamic)
      return {};

    uint32_t index = 0;
    if (!m_impl->vehicleFreeList.empty())
    {
      index = m_impl->vehicleFreeList.back();
      m_impl->vehicleFreeList.pop_back();
    }
    else
    {
      index = static_cast<uint32_t>(m_impl->vehicles.size());
      m_impl->vehicles.emplace_back();
    }

    const uint32_t clampedCount = std::min<uint32_t>(wheelCount, kMaxVehicleWheels);

    btRaycastVehicle::btVehicleTuning tuning;
    tuning.m_suspensionStiffness = vehicle.suspensionStiffness;
    tuning.m_suspensionCompression = vehicle.dampingCompression;
    tuning.m_suspensionDamping = vehicle.dampingRelaxation;
    tuning.m_frictionSlip = 1.2f;
    tuning.m_maxSuspensionTravelCm = vehicle.suspensionRestLength * 100.0f;

    btVehicleRaycaster* raycaster = new btDefaultVehicleRaycaster(m_impl->world);
    btRaycastVehicle* raycastVehicle = new btRaycastVehicle(tuning, bodyRec.body, raycaster);
    raycastVehicle->setCoordinateSystem(0, 1, 2);

    m_impl->world->addVehicle(raycastVehicle);
    bodyRec.body->setActivationState(DISABLE_DEACTIVATION);

    for (uint32_t i = 0; i < clampedCount; ++i)
    {
      const VehicleWheelConfig& wc = wheels[i];
      btVector3 conn(wc.connectionPoint[0], wc.connectionPoint[1], wc.connectionPoint[2]);
      btVector3 dir(wc.direction[0], wc.direction[1], wc.direction[2]);
      btVector3 axle(wc.axle[0], wc.axle[1], wc.axle[2]);

      raycastVehicle->addWheel(conn,
                               dir,
                               axle,
                               wc.suspensionRestLength,
                               wc.wheelRadius,
                               tuning,
                               wc.front);

      btWheelInfo& wi = raycastVehicle->getWheelInfo((int)i);
      wi.m_suspensionStiffness = vehicle.suspensionStiffness;
      wi.m_wheelsDampingCompression = vehicle.dampingCompression;
      wi.m_wheelsDampingRelaxation = vehicle.dampingRelaxation;
      wi.m_frictionSlip = tuning.m_frictionSlip;
      wi.m_rollInfluence = 0.1f;
      wi.m_maxSuspensionTravelCm = vehicle.suspensionRestLength * 100.0f;
    }

    raycastVehicle->resetSuspension();

    for (uint32_t i = 0; i < clampedCount; ++i)
      raycastVehicle->updateWheelTransform((int)i, true);

    Impl::VehicleRecord& rec = m_impl->vehicles[index];
    rec.active = true;
    rec.chassis = chassis;
    rec.vehicle = raycastVehicle;
    rec.raycaster = raycaster;
    rec.wheelCount = clampedCount;
    rec.baseFrictionSlip = tuning.m_frictionSlip;
    for (uint32_t i = 0; i < clampedCount; ++i)
      rec.front[i] = wheels[i].front;

    return VehicleHandle{ index + 1u };
  }

  void PhysicsWorld::removeRaycastVehicle(VehicleHandle handle)
  {
    if (!m_impl || !m_impl->world)
      return;
    if (!handle.valid())
      return;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return;

    Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active)
      return;

    if (rec.vehicle)
      m_impl->world->removeVehicle(rec.vehicle);
    delete rec.vehicle;
    delete rec.raycaster;
    rec = {};
    m_impl->vehicleFreeList.push_back(idx);
  }

  bool PhysicsWorld::setVehicleControls(VehicleHandle handle,
                                        float engineForce,
                                        float brakeForce,
                                        float steerAngle,
                                        float handbrakeForce)
  {
    if (!m_impl || !m_impl->world)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return false;

    Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active || !rec.vehicle)
      return false;

    const float hbNorm = (handbrakeForce > 0.0f)
                       ? std::min(1.0f, handbrakeForce / (handbrakeForce + brakeForce + 1.0f))
                       : 0.0f;
    const float rearSlip = rec.baseFrictionSlip * (1.0f - hbNorm * 0.7f);

    for (uint32_t i = 0; i < rec.wheelCount; ++i)
    {
      const bool front = rec.front[i];
      if (front)
      {
        rec.vehicle->setSteeringValue(steerAngle, (int)i);
        rec.vehicle->applyEngineForce(0.0f, (int)i);
        rec.vehicle->setBrake(brakeForce, (int)i);
      }
      else
      {
        rec.vehicle->setSteeringValue(0.0f, (int)i);
        rec.vehicle->applyEngineForce(engineForce, (int)i);
        rec.vehicle->setBrake(brakeForce + handbrakeForce, (int)i);

        btWheelInfo& wi = rec.vehicle->getWheelInfo((int)i);
        wi.m_frictionSlip = rearSlip;
      }
    }

    return true;
  }

  bool PhysicsWorld::updateVehicleTuning(VehicleHandle handle, const VehicleComponent& vehicle)
  {
    if (!m_impl || !m_impl->world)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return false;

    Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active || !rec.vehicle)
      return false;

    const float maxTravel = vehicle.suspensionRestLength * 100.0f;
    for (uint32_t i = 0; i < rec.wheelCount; ++i)
    {
      btWheelInfo& wi = rec.vehicle->getWheelInfo((int)i);
      wi.m_suspensionStiffness = vehicle.suspensionStiffness;
      wi.m_wheelsDampingCompression = vehicle.dampingCompression;
      wi.m_wheelsDampingRelaxation = vehicle.dampingRelaxation;
      wi.m_wheelsRadius = vehicle.wheelRadius;
      wi.m_suspensionRestLength1 = vehicle.suspensionRestLength;
      wi.m_maxSuspensionTravelCm = maxTravel;
    }

    const uint32_t bodyIdx = rec.chassis.id - 1u;
    if (bodyIdx < m_impl->bodies.size())
    {
      BodyRecord& bodyRec = m_impl->bodies[bodyIdx];
      if (bodyRec.active && bodyRec.body && bodyRec.shape)
      {
        btVector3 inertia(0, 0, 0);
        const btScalar mass = btScalar(std::max(0.0f, vehicle.mass));
        if (mass > 0.0f)
          bodyRec.shape->calculateLocalInertia(mass, inertia);
        bodyRec.body->setMassProps(mass, inertia);
        bodyRec.body->updateInertiaTensor();
      }
    }

    return true;
  }

  bool PhysicsWorld::getVehicleTelemetry(VehicleHandle handle, VehicleRuntime& ioRuntime, float restLength)
  {
    if (!m_impl || !m_impl->world)
      return false;
    if (!handle.valid())
      return false;
    const uint32_t idx = handle.id - 1u;
    if (idx >= m_impl->vehicles.size())
      return false;

    Impl::VehicleRecord& rec = m_impl->vehicles[idx];
    if (!rec.active || !rec.vehicle)
      return false;

    ioRuntime.wheelCount = rec.wheelCount;
    const float speedKmh = rec.vehicle->getCurrentSpeedKmHour();
    ioRuntime.speedMs = speedKmh / 3.6f;

    const float invRest = (restLength > 1e-4f) ? (1.0f / restLength) : 0.0f;
    for (uint32_t i = 0; i < rec.wheelCount; ++i)
    {
      const btWheelInfo& wi = rec.vehicle->getWheelInfo((int)i);
      ioRuntime.wheelContact[i] = wi.m_raycastInfo.m_isInContact;
      const float suspLen = wi.m_raycastInfo.m_suspensionLength;
      const float comp = (restLength - suspLen) * invRest;
      ioRuntime.suspensionCompression[i] = std::max(0.0f, std::min(1.0f, comp));

      const btTransform& wt = wi.m_worldTransform;
      const btVector3 p = wt.getOrigin();
      ioRuntime.wheelWorldPos[i][0] = p.x();
      ioRuntime.wheelWorldPos[i][1] = p.y();
      ioRuntime.wheelWorldPos[i][2] = p.z();

      const btQuaternion q = wt.getRotation();
      eulerFromQuat(q, ioRuntime.wheelWorldRot[i]);

      const btVector3 cp = wi.m_raycastInfo.m_contactPointWS;
      ioRuntime.wheelContactPoint[i][0] = cp.x();
      ioRuntime.wheelContactPoint[i][1] = cp.y();
      ioRuntime.wheelContactPoint[i][2] = cp.z();
    }

    return true;
  }

  const PhysicsStats& PhysicsWorld::stats() const
  {
    static PhysicsStats dummy{};
    return m_impl ? m_impl->stats : dummy;
  }

  static Entity pickActiveCamera(World& world, Transform*& outTransform)
  {
    Entity active = kInvalidEntity;
    Entity fallback = kInvalidEntity;
    Transform* activeTransform = nullptr;
    Transform* fallbackTransform = nullptr;

    world.ForEach<Camera, Transform>([&](Entity e, Camera& cam, Transform& tr)
    {
      if (!activeTransform && cam.active)
      {
        active = e;
        activeTransform = &tr;
      }
      if (!fallbackTransform)
      {
        fallback = e;
        fallbackTransform = &tr;
      }
    });

    if (activeTransform)
    {
      outTransform = activeTransform;
      return active;
    }
    outTransform = fallbackTransform;
    return fallback;
  }

  void PhysicsSyncSystem(World& world, float dt, void* user)
  {
    (void)dt;
    PhysicsSyncState* state = static_cast<PhysicsSyncState*>(user);
    if (!state || !state->world)
      return;

    PhysicsWorld& physics = *state->world;
    PhysicsDebugState* debug = state->debug;

    for (size_t i = 0; i < state->tracked.size();)
    {
      const PhysicsTrackedBody tb = state->tracked[i];
      const bool alive = world.isAlive(tb.entity);
      const bool has = alive &&
                       world.has<RigidBody>(tb.entity) &&
                       world.has<Collider>(tb.entity) &&
                       world.has<Transform>(tb.entity);

      bool remove = false;
      if (!alive || !has || !tb.handle.valid())
      {
        remove = true;
      }
      else
      {
        RigidBody* rb = world.get<RigidBody>(tb.entity);
        PhysicsBodyHandle* hb = world.get<PhysicsBodyHandle>(tb.entity);
        if (!rb || !hb || hb->id != tb.handle.id || rb->type != tb.type)
          remove = true;
      }

      if (remove)
      {
        physics.removeRigidBody(tb.handle);
        if (alive)
          world.remove<PhysicsBodyHandle>(tb.entity);
        state->tracked[i] = state->tracked.back();
        state->tracked.pop_back();
        continue;
      }
      ++i;
    }

    world.ForEach<RigidBody, Collider, Transform>([&](Entity e, RigidBody& rb, Collider& col, Transform& tr)
    {
      if (world.has<PhysicsBodyHandle>(e))
        return;

      PhysicsBodyHandle handle = (rb.type == RigidBodyType::Static)
                               ? physics.addStaticCollider(e, tr, col)
                               : physics.addRigidBody(e, tr, rb, col);
      if (!handle.valid())
        return;

      PhysicsBodyHandle& hb = world.add<PhysicsBodyHandle>(e);
      hb = handle;
      state->tracked.push_back({ e, handle, rb.type });
    });

    world.ForEach<RigidBody, Transform, PhysicsBodyHandle>([&](Entity, RigidBody& rb, Transform& tr, PhysicsBodyHandle& h)
    {
      if (!h.valid())
        return;
      if (rb.type == RigidBodyType::Kinematic)
        physics.setKinematicTarget(h, tr);
    });

    const bool paused = debug ? debug->pausePhysics : false;
    physics.step(paused ? 0.0f : dt);

    world.ForEach<RigidBody, Transform, PhysicsBodyHandle>([&](Entity, RigidBody& rb, Transform& tr, PhysicsBodyHandle& h)
    {
      if (!h.valid())
        return;
      if (rb.type != RigidBodyType::Dynamic)
        return;

      float pos[3]{};
      float rot[3]{};
      if (physics.getBodyTransform(h, pos, rot))
      {
        tr.localPos[0] = pos[0];
        tr.localPos[1] = pos[1];
        tr.localPos[2] = pos[2];
        tr.localRot[0] = rot[0];
        tr.localRot[1] = rot[1];
        tr.localRot[2] = rot[2];
        tr.dirty = true;
      }
    });

    if (debug)
      debug->stats = physics.stats();
  }

  void PhysicsDebugDrawSystem(World& world, float dt, void* user)
  {
    (void)dt;
    PhysicsDebugDrawState* state = static_cast<PhysicsDebugDrawState*>(user);
    if (!state || !state->world || !state->draw || !state->debug)
      return;

    PhysicsDebugState& dbg = *state->debug;

    if (dbg.showPhysicsDebug || dbg.showColliders)
      state->world->debugDraw(*state->draw);

    if (!dbg.requestRaycast)
      return;

    dbg.requestRaycast = false;

    Transform* camT = nullptr;
    Entity cam = pickActiveCamera(world, camT);
    if (!camT || !isValidEntity(cam))
      return;

    const float yaw = camT->localRot[1];
    const float pitch = camT->localRot[0];
    float dir[3]{};
    dir[0] = std::sin(yaw) * std::cos(pitch);
    dir[1] = -std::sin(pitch);
    dir[2] = std::cos(yaw) * std::cos(pitch);

    const float origin[3] = { camT->localPos[0], camT->localPos[1], camT->localPos[2] };
    const RaycastHit hit = state->world->raycast(origin, dir, dbg.rayMaxDistance, dbg.rayMask);
    dbg.lastRayHit = hit;

    const float dist = hit.hit ? hit.distance : dbg.rayMaxDistance;
    const float end[3] = {
      origin[0] + dir[0] * dist,
      origin[1] + dir[1] * dist,
      origin[2] + dir[2] * dist
    };

    const float hitColor[3] = { 0.2f, 1.0f, 0.2f };
    const float missColor[3] = { 1.0f, 0.2f, 0.2f };
    state->draw->addLine(origin, end, hit.hit ? hitColor : missColor);

    if (hit.hit)
    {
      const float cross = 0.25f;
      const float px[3] = { hit.position[0] - cross, hit.position[1], hit.position[2] };
      const float nx[3] = { hit.position[0] + cross, hit.position[1], hit.position[2] };
      const float py[3] = { hit.position[0], hit.position[1] - cross, hit.position[2] };
      const float ny[3] = { hit.position[0], hit.position[1] + cross, hit.position[2] };
      const float pz[3] = { hit.position[0], hit.position[1], hit.position[2] - cross };
      const float nz[3] = { hit.position[0], hit.position[1], hit.position[2] + cross };
      state->draw->addLine(px, nx, hitColor);
      state->draw->addLine(py, ny, hitColor);
      state->draw->addLine(pz, nz, hitColor);

      if (isValidEntity(hit.entity))
      {
        const char* name = "(unnamed)";
        if (Name* n = world.get<Name>(hit.entity))
          name = n->value;

        if (WorldSector* ws = world.get<WorldSector>(hit.entity))
        {
          sc::log(sc::LogLevel::Info, "Raycast hit entity %u (%s) sector (%d, %d)",
                  hit.entity.index(), name, ws->coord.x, ws->coord.z);
        }
        else
        {
          sc::log(sc::LogLevel::Info, "Raycast hit entity %u (%s)", hit.entity.index(), name);
        }
      }
    }
  }

  void PhysicsDemoSystem(World& world, float dt, void* user)
  {
    (void)dt;
    PhysicsDemoState* state = static_cast<PhysicsDemoState*>(user);
    if (!state)
      return;

    const bool requestReset = state->debug ? state->debug->requestResetDemo : false;
    if (state->initialized && !requestReset)
      return;

    if (state->debug)
      state->debug->requestResetDemo = false;

    for (const Entity e : state->demoEntities)
      world.destroy(e);
    state->demoEntities.clear();

    state->demoEntities.reserve(state->stackCount);

    for (uint32_t i = 0; i < state->stackCount; ++i)
    {
      Entity e = world.create();

      Transform& t = world.add<Transform>(e);
      setLocalPosition(t,
                       state->basePos[0],
                       state->basePos[1] + state->spacing * static_cast<float>(i),
                       state->basePos[2]);

      RenderMesh& rm = world.add<RenderMesh>(e);
      rm.meshId = 1u;
      rm.materialId = state->materialId;

      Collider& col = world.add<Collider>(e);
      col.type = ColliderType::Box;
      col.halfExtents[0] = 0.5f;
      col.halfExtents[1] = 0.5f;
      col.halfExtents[2] = 0.5f;

      RigidBody& rb = world.add<RigidBody>(e);
      rb.type = RigidBodyType::Dynamic;
      rb.mass = 1.0f;
      rb.friction = 0.8f;
      rb.restitution = 0.1f;

      setName(world.add<Name>(e), "PhysCube");
      state->demoEntities.push_back(e);
    }

    state->initialized = true;
  }
}
