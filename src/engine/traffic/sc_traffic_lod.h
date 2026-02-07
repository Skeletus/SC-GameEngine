#pragma once
#include <cstdint>

#include "sc_ecs.h"
#include "sc_world_partition.h"
#include "sc_vehicle.h"
#include "sc_traffic_common.h"
#include "sc_traffic_lanes.h"

namespace sc
{
  struct TrafficLODState
  {
    WorldStreamingState* streaming = nullptr;
    TrafficLaneGraph* lanes = nullptr;
    TrafficDebugState* debug = nullptr;
    VehicleDebugState* vehicleDebug = nullptr;
    uint32_t meshId = 1u;
    uint32_t materialId = 0u;
    uint32_t lastTotalVehicles = 0;
  };

  struct TrafficPinState
  {
    WorldStreamingState* streaming = nullptr;
    TrafficDebugState* debug = nullptr;
  };

  void TrafficLODSystem(World& world, float dt, void* user);
  void TrafficPinSystem(World& world, float dt, void* user);
}

