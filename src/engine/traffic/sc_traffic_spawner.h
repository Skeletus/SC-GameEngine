#pragma once
#include <cstdint>

#include "sc_ecs.h"
#include "sc_world_partition.h"
#include "sc_traffic_common.h"
#include "sc_traffic_lanes.h"
#include "sc_vehicle.h"

namespace sc
{
  struct TrafficSpawnerState
  {
    WorldStreamingState* streaming = nullptr;
    TrafficLaneGraph* lanes = nullptr;
    TrafficDebugState* debug = nullptr;
    VehicleDebugState* vehicleDebug = nullptr;
    uint32_t meshId = 1u;
    uint32_t materialId = 0u;
    float vehicleScale[3] = { 1.8f, 0.7f, 3.5f };
    uint64_t lastLogTicks = 0;
  };

  void TrafficSpawnerSystem(World& world, float dt, void* user);
}

