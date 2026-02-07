#pragma once
#include <cstdint>

#include "sc_ecs.h"
#include "sc_world_partition.h"
#include "sc_physics.h"
#include "sc_traffic_common.h"
#include "sc_traffic_lanes.h"

namespace sc
{
  class DebugDraw;

  struct TrafficAIState
  {
    TrafficLaneGraph* lanes = nullptr;
    TrafficDebugState* debug = nullptr;
    PhysicsWorld* physics = nullptr;
    WorldStreamingState* streaming = nullptr;
  };

  struct TrafficPhysicsSyncState
  {
    PhysicsWorld* physics = nullptr;
    TrafficDebugState* debug = nullptr;
  };

  struct TrafficDebugDrawState
  {
    TrafficLaneGraph* lanes = nullptr;
    TrafficDebugState* debug = nullptr;
    DebugDraw* draw = nullptr;
    PhysicsWorld* physics = nullptr;
  };

  void TrafficAISystem(World& world, float dt, void* user);
  void TrafficPhysicsSyncSystem(World& world, float dt, void* user);
  void TrafficDebugDrawSystem(World& world, float dt, void* user);
}
