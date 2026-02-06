#pragma once
#include <vector>

#include "sc_ecs.h"
#include "sc_physics.h"

namespace sc
{
  struct WorldStreamingState;
  class DebugDraw;

  struct VehicleTracked
  {
    Entity entity = kInvalidEntity;
    VehicleHandle handle{};
    PhysicsBodyHandle body{};
  };

  struct VehicleDebugState
  {
    Entity activeVehicle = kInvalidEntity;
    bool showWheelRaycasts = false;
    bool showContactPoints = false;
    bool requestRespawn = false;
    bool cameraEnabled = true;
    VehicleRuntime telemetry{};
  };

  struct VehicleSystemState
  {
    PhysicsWorld* world = nullptr;
    PhysicsSyncState* sync = nullptr;
    VehicleDebugState* debug = nullptr;
    std::vector<VehicleTracked> tracked;
  };

  struct VehicleInputState
  {
    VehicleDebugState* debug = nullptr;
    float throttleResponse = 6.0f;
    float brakeResponse = 6.0f;
  };

  struct VehicleCameraState
  {
    PhysicsWorld* world = nullptr;
    VehicleDebugState* debug = nullptr;
    bool useFixedFollow = true;
    float fixedOffset[3] = { 0.0f, 3.0f, -8.0f };
    float fixedRot[3] = { -0.42f, 3.0f, 0.0f };
    float distance = 7.0f;
    float height = 1.0f;
    float rearOffset = 1.5f;
    float lookDownDegrees = 45.0f;
    float positionStiffness = 16.0f;
    float positionDamping = 6.0f;
    bool dynamicFov = true;
    float minFov = 60.0f;
    float maxFov = 75.0f;
    bool enableOcclusion = true;
    float occlusionPadding = 0.3f;
    float velocity[3] = { 0.0f, 0.0f, 0.0f };
  };

  struct VehicleDemoState
  {
    bool initialized = false;
    Entity vehicle = kInvalidEntity;
    VehicleDebugState* debug = nullptr;
    float spawnPos[3] = { 0.0f, 2.0f, 0.0f };
    float spawnRot[3] = { 0.0f, 0.0f, 0.0f };
    float spawnScale[3] = { 1.8f, 0.7f, 3.5f };
    uint32_t meshId = 1u;
    uint32_t materialId = 0u;
  };

  struct VehicleStreamingPinState
  {
    WorldStreamingState* streaming = nullptr;
    VehicleDebugState* debug = nullptr;
    uint32_t pinRadius = 1u;
  };

  struct VehicleDebugDrawState
  {
    DebugDraw* draw = nullptr;
    VehicleDebugState* debug = nullptr;
  };

  void VehicleInputSystem(World& world, float dt, void* user);
  void VehicleSystemPreStep(World& world, float dt, void* user);
  void VehicleSystemPostStep(World& world, float dt, void* user);
  void VehicleDemoSystem(World& world, float dt, void* user);
  void VehicleStreamingPinSystem(World& world, float dt, void* user);
  void VehicleCameraSystem(World& world, float dt, void* user);
  void VehicleDebugDrawSystem(World& world, float dt, void* user);
}
