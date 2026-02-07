#pragma once
#include "sc_ecs.h"
#include <cstdint>

#include "sc_physics.h"

namespace sc
{
  static constexpr uint32_t kInvalidLaneId = 0xFFFFFFFFu;

  enum class TrafficSimMode : uint8_t
  {
    Physics = 0,
    Kinematic = 1,
    OnRails = 2
  };

  enum class TrafficHitType : uint8_t
  {
    None = 0,
    Self = 1,
    Vehicle = 2,
    World = 3
  };

  struct TrafficAgent
  {
    uint32_t laneId = kInvalidLaneId;
    float laneS = 0.0f;
    float targetSpeed = 0.0f;
    uint8_t state = 0;
    float lookAheadDist = 12.0f;
    float desiredLaneChangeCooldown = 0.0f;
    float stuckTimer = 0.0f;
    uint8_t stuckLogged = 0;
  };

  struct TrafficVehicle
  {
    VehicleHandle vehicle{};
    PhysicsBodyHandle body{};
    TrafficSimMode mode = TrafficSimMode::OnRails;
    uint8_t renderOffsetMode = 0; // 0=unset, 1=raw COM, 2=COM-corrected
  };

  struct TrafficSensors
  {
    float frontRayLength = 20.0f;
    float sideRayLength = 6.0f;
    float safeDistance = 10.0f;
    float lastHitDistance = 0.0f;
    TrafficHitType lastHitType = TrafficHitType::None;
  };

  struct TrafficDebugState
  {
    bool showLanes = false;
    bool showAgentTargets = false;
    bool showSensorRays = false;
    bool showTierColors = false;

    float densityPerKm2 = 250.0f;
    float lookAheadDist = 12.0f;
    float safeDistance = 10.0f;
    float speedMultiplier = 1.0f;
    float frontRayLength = 20.0f;
    float playerExclusionRadius = 25.0f;

    float tierAEnter = 50.0f;
    float tierAExit = 70.0f;
    float tierBEnter = 110.0f;
    float tierBExit = 150.0f;

    uint32_t maxTrafficVehiclesTotal = 200;
    uint32_t maxTrafficVehiclesPhysics = 24;
    uint32_t maxTrafficVehiclesKinematic = 64;
    uint32_t trafficPinRadius = 1;

    uint32_t totalVehicles = 0;
    uint32_t tierPhysics = 0;
    uint32_t tierKinematic = 0;
    uint32_t tierOnRails = 0;
    uint32_t spawnsThisFrame = 0;
    uint32_t despawnsThisFrame = 0;
    uint32_t nearestLaneId = kInvalidLaneId;
    uint32_t spawnAttemptsThisFrame = 0;
    uint32_t spawnRejectLaneGap = 0;
    uint32_t spawnRejectOccupied = 0;
    uint32_t spawnRejectLanePerFrame = 0;
    uint32_t spawnRejectSectorLimit = 0;
    Entity nearestTrafficDesyncEntity = kInvalidEntity;
    float nearestTrafficEcsPos[3] = { 0.0f, 0.0f, 0.0f };
    float nearestTrafficPhysPos[3] = { 0.0f, 0.0f, 0.0f };
    float nearestTrafficDesync = 0.0f;
    float nearestTrafficDesyncTimer = 0.0f;
    uint8_t nearestTrafficDesyncLogged = 0;

    Entity nearestTrafficEntity = kInvalidEntity;
    TrafficSimMode nearestTrafficTier = TrafficSimMode::OnRails;
    float nearestTrafficDistance = 0.0f;
    float nearestTrafficSpeed = 0.0f;
    float nearestTrafficTargetSpeed = 0.0f;
    uint32_t nearestTrafficLaneId = kInvalidLaneId;
    float nearestTrafficLaneS = 0.0f;
    VehicleInput nearestTrafficInput{};
    bool nearestTrafficBodyActive = false;
    bool nearestTrafficBodyInWorld = false;
    float nearestTrafficBodyMass = 0.0f;
    float nearestTrafficBodyLinVel[3] = { 0.0f, 0.0f, 0.0f };
    bool nearestTrafficVehicleInWorld = false;
    uint32_t nearestTrafficVehicleWheelCount = 0u;
    float nearestTrafficVehicleSpeedKmh = 0.0f;
    float nearestTrafficSensorHitDistance = 0.0f;
    TrafficHitType nearestTrafficSensorHitType = TrafficHitType::None;

    Entity stuckTrafficEntity = kInvalidEntity;
    TrafficSimMode stuckTrafficTier = TrafficSimMode::OnRails;
    float stuckTrafficSpeed = 0.0f;
    float stuckTrafficTargetSpeed = 0.0f;
    uint32_t stuckTrafficLaneId = kInvalidLaneId;
    float stuckTrafficLaneS = 0.0f;
    bool stuckTrafficBodyActive = false;
    bool stuckTrafficBodyInWorld = false;
    float stuckTrafficBodyMass = 0.0f;
    float stuckTrafficBodyLinVel[3] = { 0.0f, 0.0f, 0.0f };
    bool stuckTrafficVehicleInWorld = false;
    uint32_t stuckTrafficVehicleWheelCount = 0u;
    float stuckTrafficVehicleSpeedKmh = 0.0f;
    float stuckTrafficSensorHitDistance = 0.0f;
    TrafficHitType stuckTrafficSensorHitType = TrafficHitType::None;
    uint32_t stuckCount = 0u;
  };
}
