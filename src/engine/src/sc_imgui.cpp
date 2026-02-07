#include "sc_imgui.h"
#include "sc_log.h"
#include "sc_world_partition.h"
#include "sc_physics.h"
#include "sc_vehicle.h"
#include "sc_traffic_common.h"

#include <SDL.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace sc
{
  static void check_vk_result(VkResult r)
  {
    if (r == VK_SUCCESS)
      return;
    sc::log(sc::LogLevel::Error, "ImGui Vulkan error (%d)", (int)r);
  }

  bool DebugUI::init(SDL_Window* window,
                     VkInstance instance,
                     VkDevice device,
                     VkPhysicalDevice phys,
                     uint32_t queueFamily,
                     VkQueue queue,
                     VkRenderPass renderPass,
                     uint32_t imageCount)
  {
    m_window = window;
    m_instance = instance;
    m_device = device;
    m_phys = phys;
    m_queueFamily = queueFamily;
    m_queue = queue;
    m_renderPass = renderPass;
    m_imageCount = imageCount;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini in runtime

    if (!ImGui_ImplSDL2_InitForVulkan(m_window))
    {
      sc::log(sc::LogLevel::Error, "ImGui_ImplSDL2_InitForVulkan failed.");
      return false;
    }

    if (!createDescriptorPool()) return false;

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = m_instance;
    info.PhysicalDevice = m_phys;
    info.Device = m_device;
    info.QueueFamily = m_queueFamily;
    info.Queue = m_queue;
    info.DescriptorPool = m_descriptorPool;
    info.RenderPass = m_renderPass;
    info.MinImageCount = m_imageCount;
    info.ImageCount = m_imageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check_vk_result;

    if (!ImGui_ImplVulkan_Init(&info))
    {
      sc::log(sc::LogLevel::Error, "ImGui_ImplVulkan_Init failed.");
      return false;
    }

    if (!uploadFonts()) return false;

    m_freq = static_cast<double>(SDL_GetPerformanceFrequency());
    m_lastCounter = SDL_GetPerformanceCounter();

    m_initialized = true;
    sc::log(sc::LogLevel::Info, "DebugUI initialized.");
    return true;
  }

  void DebugUI::shutdown()
  {
    if (!m_initialized)
      return;

    vkDeviceWaitIdle(m_device);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }

    m_initialized = false;
  }

  void DebugUI::processEvent(const SDL_Event& e)
  {
    if (!m_initialized)
      return;
    ImGui_ImplSDL2_ProcessEvent(&e);
  }

  void DebugUI::setFrameStats(uint32_t frameIndex, uint32_t imageIndex, VkExtent2D extent)
  {
    m_frameIndex = frameIndex;
    m_imageIndex = imageIndex;
    m_extent = extent;
  }

  void DebugUI::setTelemetry(const JobsTelemetrySnapshot& jobs, const MemStats& mem)
  {
    m_jobsSnap = jobs;
    m_memSnap = mem;
  }

  void DebugUI::setEcsStats(const EcsStatsSnapshot& ecs, const SchedulerStatsSnapshot& sched)
  {
    m_ecsSnap = ecs;
    m_schedSnap = sched;
  }

  void DebugUI::setWorldContext(World* world, Entity camera, Entity triangle, Entity cube, Entity root)
  {
    m_world = world;
    m_cameraEntity = camera;
    m_triangleEntity = triangle;
    m_cubeEntity = cube;
    m_rootEntity = root;
  }

  void DebugUI::setWorldStreamingContext(WorldStreamingState* streaming,
                                         CullingState* culling,
                                         RenderPrepStreamingState* renderPrep)
  {
    m_worldStreaming = streaming;
    m_culling = culling;
    m_renderPrepStreaming = renderPrep;
  }

  void DebugUI::setAssetPanelData(const AssetStatsSnapshot& stats,
                                  const std::vector<std::string>& labels,
                                  const std::vector<MaterialHandle>& materialIds,
                                  uint32_t selectedIndex)
  {
    m_assetStats = stats;
    m_assetLabels = labels;
    m_assetMaterialIds = materialIds;
    m_assetSelectedIndex = selectedIndex;
    if (m_assetSelectedIndex >= m_assetMaterialIds.size())
      m_assetSelectedIndex = 0;
  }

  void DebugUI::newFrame()
  {
    if (!m_initialized)
      return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const uint64_t now = SDL_GetPerformanceCounter();
    const double dt = static_cast<double>(now - m_lastCounter) / m_freq;
    m_lastCounter = now;

    const float dtMs = static_cast<float>(dt * 1000.0);
    m_frameTimes[m_frameOffset] = dtMs;
    m_frameOffset = (m_frameOffset + 1) % (uint32_t)(sizeof(m_frameTimes) / sizeof(m_frameTimes[0]));
    if (m_frameCount < (uint32_t)(sizeof(m_frameTimes) / sizeof(m_frameTimes[0])))
      m_frameCount++;

    float sumMs = 0.0f;
    for (uint32_t i = 0; i < m_frameCount; ++i) sumMs += m_frameTimes[i];
    const float avgMs = (m_frameCount > 0) ? (sumMs / (float)m_frameCount) : 0.0f;
    const float fps = (avgMs > 0.0f) ? (1000.0f / avgMs) : 0.0f;

    ImGui::Begin("Debug Overlay");
    ImGui::Text("FPS: %.1f  (%.2f ms/frame)", fps, avgMs);
    ImGui::Separator();
    ImGui::Text("Resolution: %ux%u", m_extent.width, m_extent.height);
    ImGui::Text("FrameIndex: %u  ImageIndex: %u", m_frameIndex, m_imageIndex);
    ImGui::Checkbox("Pause Meshes", &m_pauseTriangle);
    ImGui::PlotLines("Frame Time (ms)", m_frameTimes, (int)m_frameCount, (int)m_frameOffset, nullptr, 0.0f, 50.0f, ImVec2(0, 60));
    ImGui::Separator();

    ImGui::Text("Jobs/Memory");
    ImGui::Text("Workers: %u  Pending: %llu", m_jobsSnap.workerThreads, (unsigned long long)m_jobsSnap.jobsPending);
    ImGui::Text("Jobs: enq=%llu  done=%llu", (unsigned long long)m_jobsSnap.jobsEnqueued, (unsigned long long)m_jobsSnap.jobsCompleted);
    ImGui::Text("Job time: %.3f ms", m_jobsSnap.totalJobMs);

    if (m_jobsSnap.topScopes.count > 0)
    {
      ImGui::Text("Top Scopes:");
      for (uint32_t i = 0; i < m_jobsSnap.topScopes.count; ++i)
      {
        const auto& e = m_jobsSnap.topScopes.entries[i];
        ImGui::BulletText("%s: %.3f ms", e.name ? e.name : "(null)", e.ms);
      }
    }

    ImGui::Text("Memory Live (bytes):");
    for (uint8_t i = 0; i < (uint8_t)MemTag::Count; ++i)
    {
      ImGui::BulletText("%s: %llu", memTagName((MemTag)i), (unsigned long long)m_memSnap.bytesLive[i]);
    }

    ImGui::Separator();
    ImGui::Text("ECS");
    ImGui::Text("Entities: %u / %u", m_ecsSnap.entityAlive, m_ecsSnap.entityCapacity);
    ImGui::BulletText("Transform: %u", m_ecsSnap.transforms);
    ImGui::BulletText("Camera: %u", m_ecsSnap.cameras);
    ImGui::BulletText("RenderMesh: %u", m_ecsSnap.renderMeshes);
    ImGui::BulletText("Name: %u", m_ecsSnap.names);

    ImGui::Separator();
    ImGui::Text("Systems");
    for (uint32_t i = 0; i < m_schedSnap.count; ++i)
    {
      const auto& e = m_schedSnap.entries[i];
      ImGui::BulletText("%s (P%u): %.3f ms", e.name ? e.name : "(null)", (uint32_t)e.phase, e.ms);
    }

    if (m_debugDraw)
    {
      ImGui::Separator();
      ImGui::Text("Debug Draw");
      DebugDrawSettings& s = m_debugDraw->settings();
      ImGui::Checkbox("Show Grid", &s.showGrid);
      ImGui::SliderFloat("Grid Size", &s.gridSize, 1.0f, 200.0f, "%.1f");
      ImGui::SliderFloat("Grid Step", &s.gridStep, 0.1f, 10.0f, "%.1f");
      if (s.gridStep > s.gridSize)
        s.gridStep = s.gridSize;
    }

    if (m_world)
    {
      ImGui::Separator();
      ImGui::Text("Scene");

      if (Transform* camT = m_world->get<Transform>(m_cameraEntity))
      {
        float pos[3] = { camT->localPos[0], camT->localPos[1], camT->localPos[2] };
        if (ImGui::DragFloat3("Camera Pos", pos, 0.05f))
        {
          setLocalPosition(*camT, pos[0], pos[1], pos[2]);
        }

        float rot[3] = { camT->localRot[0], camT->localRot[1], camT->localRot[2] };
        if (ImGui::DragFloat3("Camera Rot", rot, 0.01f))
        {
          camT->localRot[0] = rot[0];
          camT->localRot[1] = rot[1];
          camT->localRot[2] = rot[2];
          markDirty(*camT);
        }
      }
      if (Camera* cam = m_world->get<Camera>(m_cameraEntity))
      {
        ImGui::Text("Active Camera: %u  FOV: %.1f", m_cameraEntity.index(), cam->fovY);
      }

      if (Transform* triT = m_world->get<Transform>(m_triangleEntity))
      {
        float pos[3] = { triT->localPos[0], triT->localPos[1], triT->localPos[2] };
        if (ImGui::DragFloat3("Triangle Pos", pos, 0.05f))
        {
          setLocalPosition(*triT, pos[0], pos[1], pos[2]);
        }
      }
      if (Transform* rootT = m_world->get<Transform>(m_rootEntity))
      {
        float pos[3] = { rootT->localPos[0], rootT->localPos[1], rootT->localPos[2] };
        if (ImGui::DragFloat3("Root Pos", pos, 0.05f))
        {
          setLocalPosition(*rootT, pos[0], pos[1], pos[2]);
        }
      }

      if (ImGui::TreeNode("Hierarchy"))
      {
        std::vector<Entity> entities;
        m_world->ForEach<Transform>([&](Entity e, Transform&) { entities.push_back(e); });

        uint32_t maxIndex = 0;
        for (const Entity e : entities)
          maxIndex = (e.index() > maxIndex) ? e.index() : maxIndex;

        std::vector<std::vector<Entity>> children(maxIndex + 1u);
        std::vector<Entity> roots;
        roots.reserve(entities.size());

        for (const Entity e : entities)
        {
          Transform* t = m_world->get<Transform>(e);
          if (!t)
            continue;

          bool validParent = isValidEntity(t->parent) && t->parent != e &&
                             m_world->isAlive(t->parent) && m_world->has<Transform>(t->parent);

          if (!validParent)
          {
            roots.push_back(e);
          }
          else
          {
            children[t->parent.index()].push_back(e);
          }
        }

        auto drawNode = [&](auto&& self, Entity e) -> void
        {
          const char* label = "(Entity)";
          if (Name* n = m_world->get<Name>(e))
            label = n->value;

          const bool open = ImGui::TreeNode((void*)(uintptr_t)e.value, "%s [%u]", label, e.index());
          if (open)
          {
            const uint32_t idx = e.index();
            if (idx < children.size())
            {
              for (const Entity c : children[idx])
                self(self, c);
            }
            ImGui::TreePop();
          }
        };

        for (const Entity r : roots)
          drawNode(drawNode, r);

        ImGui::TreePop();
      }

      const Mat4& vp = m_world->renderFrame().viewProj;
      ImGui::Text("ViewProj m00/m11/m22: %.2f  %.2f  %.2f", vp.m[0], vp.m[5], vp.m[10]);
    }

    if (m_worldStreaming)
    {
      ImGui::Separator();
      ImGui::Text("World Streaming (Async)");

      ImGui::Checkbox("Freeze Streaming", &m_worldStreaming->freezeStreaming);
      if (m_culling)
        ImGui::Checkbox("Freeze Culling", &m_culling->freezeCulling);
      ImGui::Checkbox("Freeze Eviction", &m_worldStreaming->freezeEviction);
      ImGui::Checkbox("Show Sector Bounds", &m_worldStreaming->showSectorBounds);
      ImGui::Checkbox("Show Sector State Colors", &m_worldStreaming->showSectorStateColors);
      ImGui::Checkbox("Show Entity Bounds", &m_worldStreaming->showEntityBounds);

      int loadRadius = static_cast<int>(m_worldStreaming->budgets.loadRadiusSectors);
      if (ImGui::SliderInt("Load Radius (sectors)", &loadRadius, 0, 8))
      {
        m_worldStreaming->budgets.loadRadiusSectors = static_cast<uint32_t>(std::max(loadRadius, 0));
        if (m_worldStreaming->budgets.unloadRadiusSectors <= m_worldStreaming->budgets.loadRadiusSectors)
          m_worldStreaming->budgets.unloadRadiusSectors = m_worldStreaming->budgets.loadRadiusSectors + 1u;
      }

      int unloadRadius = static_cast<int>(m_worldStreaming->budgets.unloadRadiusSectors);
      if (ImGui::SliderInt("Unload Radius (sectors)", &unloadRadius, 1, 12))
      {
        const uint32_t minUnload = m_worldStreaming->budgets.loadRadiusSectors + 1u;
        m_worldStreaming->budgets.unloadRadiusSectors = static_cast<uint32_t>(std::max(unloadRadius, (int)minUnload));
      }

      int maxSectors = static_cast<int>(m_worldStreaming->budgets.maxActiveSectors);
      if (ImGui::SliderInt("Max Active Sectors", &maxSectors, 1, 256))
        m_worldStreaming->budgets.maxActiveSectors = static_cast<uint32_t>(std::max(maxSectors, 1));

      int maxEntities = static_cast<int>(m_worldStreaming->budgets.maxEntitiesBudget);
      if (ImGui::SliderInt("Max Entities Budget", &maxEntities, 128, 50000))
        m_worldStreaming->budgets.maxEntitiesBudget = static_cast<uint32_t>(std::max(maxEntities, 128));

      int maxDraws = static_cast<int>(m_worldStreaming->budgets.maxDrawsBudget);
      if (ImGui::SliderInt("Max Draws Budget", &maxDraws, 128, 50000))
        m_worldStreaming->budgets.maxDrawsBudget = static_cast<uint32_t>(std::max(maxDraws, 128));

      int maxConcurrentLoads = static_cast<int>(m_worldStreaming->budgets.maxConcurrentLoads);
      if (ImGui::SliderInt("Max Concurrent Loads", &maxConcurrentLoads, 1, 16))
        m_worldStreaming->budgets.maxConcurrentLoads = static_cast<uint32_t>(std::max(maxConcurrentLoads, 1));

      int maxActivations = static_cast<int>(m_worldStreaming->budgets.maxActivationsPerFrame);
      if (ImGui::SliderInt("Max Activations/Frame", &maxActivations, 1, 32))
        m_worldStreaming->budgets.maxActivationsPerFrame = static_cast<uint32_t>(std::max(maxActivations, 1));

      int maxDespawns = static_cast<int>(m_worldStreaming->budgets.maxDespawnsPerFrame);
      if (ImGui::SliderInt("Max Despawns/Frame", &maxDespawns, 1, 1024))
        m_worldStreaming->budgets.maxDespawnsPerFrame = static_cast<uint32_t>(std::max(maxDespawns, 1));

      ImGui::Checkbox("Frustum Bias", &m_worldStreaming->budgets.useFrustumBias);
      if (m_worldStreaming->budgets.useFrustumBias)
        ImGui::SliderFloat("Frustum Bias Weight", &m_worldStreaming->budgets.frustumBiasWeight, 0.0f, 8.0f, "%.2f");

      int boundsLimit = static_cast<int>(m_worldStreaming->entityBoundsLimit);
      if (ImGui::SliderInt("Entity Bounds Limit", &boundsLimit, 0, 512))
        m_worldStreaming->entityBoundsLimit = static_cast<uint32_t>(std::max(boundsLimit, 0));

      const WorldPartitionFrameStats& ws = m_worldStreaming->stats;
      ImGui::Text("Camera Sector: (%d, %d)", ws.cameraSector.x, ws.cameraSector.z);
      ImGui::Text("Loaded / Desired: %u / %u", ws.loadedSectors, ws.desiredSectors);
      ImGui::Text("Sectors this frame: +%u / -%u", ws.loadedThisFrame, ws.unloadedThisFrame);
      ImGui::Text("Sector states: queued %u  loading %u  ready %u  active %u  unloading %u",
                  ws.queuedSectors, ws.loadingSectors, ws.readySectors, ws.activeSectors, ws.unloadingSectors);
      ImGui::Text("Loads: completed %u  avg %.2f ms  max %.2f ms",
                  ws.completedLoads, ws.avgLoadMs, ws.maxLoadMs);
      ImGui::Text("Activations / Despawns: %u / %u", ws.activations, ws.despawns);
      ImGui::Text("Pump loads: %.2f ms  unloads: %.2f ms", ws.pumpLoadsMs, ws.pumpUnloadsMs);
      ImGui::Text("Entities this frame: +%u / -%u", ws.entitiesSpawned, ws.entitiesDespawned);
      ImGui::Text("Estimated sector entities: %u", ws.estimatedSectorEntities);

      if (m_culling)
      {
        ImGui::Text("Renderables: total %u  visible %u  culled %u",
                    m_culling->stats.renderablesTotal,
                    m_culling->stats.visible,
                    m_culling->stats.culled);
      }

      if (m_renderPrepStreaming)
      {
        ImGui::Text("Draws: emitted %u  dropped by budget %u",
                    m_renderPrepStreaming->stats.drawsEmitted,
                    m_renderPrepStreaming->stats.drawsDroppedByBudget);
      }

      const bool sectorBudgetExceeded = ws.rejectedBySectorBudget > 0;
      const bool entityBudgetExceeded = ws.rejectedByEntityBudget > 0;
      const bool drawBudgetExceeded = m_renderPrepStreaming && m_renderPrepStreaming->stats.drawsDroppedByBudget > 0;
      ImGui::Text("Budget exceeded: sectors=%s entities=%s draws=%s",
                  sectorBudgetExceeded ? "YES" : "NO",
                  entityBudgetExceeded ? "YES" : "NO",
                  drawBudgetExceeded ? "YES" : "NO");

      if (sectorBudgetExceeded)
        ImGui::Text("Sector budget rejections: %u", ws.rejectedBySectorBudget);
      if (entityBudgetExceeded)
        ImGui::Text("Entity budget rejections: %u", ws.rejectedByEntityBudget);
    }

    if (m_physics)
    {
      ImGui::Separator();
      ImGui::Text("Physics");
      ImGui::Checkbox("Show Physics Debug", &m_physics->showPhysicsDebug);
      ImGui::Checkbox("Show Colliders", &m_physics->showColliders);
      ImGui::Checkbox("Pause Physics", &m_physics->pausePhysics);

      if (ImGui::Button("Raycast"))
        m_physics->requestRaycast = true;
      ImGui::SameLine();
      if (ImGui::Button("Reset Physics Demo"))
        m_physics->requestResetDemo = true;

      ImGui::SliderFloat("Raycast Max Dist", &m_physics->rayMaxDistance, 5.0f, 500.0f, "%.1f");

      const PhysicsStats& ps = m_physics->stats;
      ImGui::Text("Bodies: dynamic %u  kinematic %u  static %u",
                  ps.dynamicBodies, ps.kinematicBodies, ps.staticColliders);
      ImGui::Text("Broadphase proxies: %u", ps.broadphaseProxies);
      ImGui::Text("Step: %.3f ms", ps.stepMs);

      if (m_physics->lastRayHit.hit)
      {
        ImGui::Text("Last Ray Hit: entity %u  dist %.2f",
                    m_physics->lastRayHit.entity.index(),
                    m_physics->lastRayHit.distance);
      }
      else
      {
        ImGui::Text("Last Ray Hit: none");
      }
    }

    if (m_vehicleDebug)
    {
      ImGui::Separator();
      ImGui::Text("Vehicle");

      ImGui::Checkbox("Vehicle Camera Enabled", &m_vehicleDebug->cameraEnabled);

      if (ImGui::Button("Respawn Vehicle"))
        m_vehicleDebug->requestRespawn = true;

      ImGui::Checkbox("Show Wheel Raycasts", &m_vehicleDebug->showWheelRaycasts);
      ImGui::Checkbox("Show Contact Points", &m_vehicleDebug->showContactPoints);

      const Entity veh = m_vehicleDebug->activeVehicle;
      VehicleComponent* vc = (m_world && isValidEntity(veh)) ? m_world->get<VehicleComponent>(veh) : nullptr;
      VehicleRuntime* vr = (m_world && isValidEntity(veh)) ? m_world->get<VehicleRuntime>(veh) : nullptr;

      if (!vc || !vr)
      {
        ImGui::Text("Active vehicle: none");
      }
      else
      {
        const float kmh = vr->speedMs * 3.6f;
        ImGui::Text("Speed: %.1f m/s (%.1f km/h)", vr->speedMs, kmh);
        ImGui::Text("Engine Force: %.1f  Brake: %.1f", vr->engineForce, vr->brakeForce);
        ImGui::Text("Steer Angle: %.1f deg", vr->steerAngle * 57.29578f);

        for (uint32_t i = 0; i < vr->wheelCount; ++i)
        {
          ImGui::Text("Wheel %u: contact=%s  compression=%.2f",
                      i,
                      vr->wheelContact[i] ? "YES" : "NO",
                      vr->suspensionCompression[i]);
        }

        ImGui::Separator();
        ImGui::Text("Handling");
        ImGui::DragFloat("Mass", &vc->mass, 10.0f, 200.0f, 5000.0f, "%.0f");
        ImGui::DragFloat("Engine Force", &vc->engineForce, 100.0f, 1000.0f, 20000.0f, "%.0f");
        ImGui::DragFloat("Max Speed (m/s)", &vc->maxSpeed, 0.5f, 5.0f, 100.0f, "%.1f");
        ImGui::DragFloat("Brake Force", &vc->brakeForce, 100.0f, 1000.0f, 30000.0f, "%.0f");
        ImGui::DragFloat("Handbrake Force", &vc->handbrakeForce, 100.0f, 1000.0f, 30000.0f, "%.0f");

        float steerDeg = vc->maxSteerAngle * 57.29578f;
        if (ImGui::SliderFloat("Max Steer (deg)", &steerDeg, 5.0f, 45.0f, "%.1f"))
          vc->maxSteerAngle = steerDeg / 57.29578f;
        ImGui::DragFloat("Steer Response", &vc->steerResponse, 0.1f, 1.0f, 20.0f, "%.2f");

        ImGui::Separator();
        ImGui::Text("Suspension");
        ImGui::DragFloat("Rest Length", &vc->suspensionRestLength, 0.01f, 0.05f, 1.0f, "%.2f");
        ImGui::DragFloat("Stiffness", &vc->suspensionStiffness, 0.5f, 1.0f, 60.0f, "%.1f");
        ImGui::DragFloat("Damping Compression", &vc->dampingCompression, 0.1f, 0.1f, 10.0f, "%.2f");
        ImGui::DragFloat("Damping Relaxation", &vc->dampingRelaxation, 0.1f, 0.1f, 10.0f, "%.2f");
        ImGui::DragFloat("Wheel Radius", &vc->wheelRadius, 0.01f, 0.1f, 1.0f, "%.2f");
        ImGui::DragFloat("Wheel Width", &vc->wheelWidth, 0.01f, 0.05f, 1.0f, "%.2f");

        float com[3] = { vc->centerOfMassOffset[0], vc->centerOfMassOffset[1], vc->centerOfMassOffset[2] };
        if (ImGui::DragFloat3("COM Offset", com, 0.01f))
        {
          vc->centerOfMassOffset[0] = com[0];
          vc->centerOfMassOffset[1] = com[1];
          vc->centerOfMassOffset[2] = com[2];
          m_vehicleDebug->requestRespawn = true;
        }
      }
    }

    if (m_trafficDebug)
    {
      ImGui::Separator();
      ImGui::Text("Traffic");

      ImGui::Text("Total: %u  Tier A: %u  Tier B: %u  Tier C: %u",
                  m_trafficDebug->totalVehicles,
                  m_trafficDebug->tierPhysics,
                  m_trafficDebug->tierKinematic,
                  m_trafficDebug->tierOnRails);
      auto hitTypeLabel = [](TrafficHitType t) -> const char*
      {
        switch (t)
        {
          case TrafficHitType::Self: return "Self";
          case TrafficHitType::Vehicle: return "Vehicle";
          case TrafficHitType::World: return "World";
          default: break;
        }
        return "None";
      };

      if (isValidEntity(m_trafficDebug->nearestTrafficEntity))
      {
        ImGui::Text("Nearest: e=%u tier=%u dist=%.1f",
                    m_trafficDebug->nearestTrafficEntity.index(),
                    (uint32_t)m_trafficDebug->nearestTrafficTier,
                    m_trafficDebug->nearestTrafficDistance);
        ImGui::Text("Speed: %.2f  Target: %.2f  Lane: %u  s=%.2f",
                    m_trafficDebug->nearestTrafficSpeed,
                    m_trafficDebug->nearestTrafficTargetSpeed,
                    m_trafficDebug->nearestTrafficLaneId,
                    m_trafficDebug->nearestTrafficLaneS);
        ImGui::Text("Input: thr=%.2f brk=%.2f str=%.2f",
                    m_trafficDebug->nearestTrafficInput.throttle,
                    m_trafficDebug->nearestTrafficInput.brake,
                    m_trafficDebug->nearestTrafficInput.steer);
        ImGui::Text("Body Active: %s  Sensor: %.2f (%s)",
                    m_trafficDebug->nearestTrafficBodyActive ? "YES" : "NO",
                    m_trafficDebug->nearestTrafficSensorHitDistance,
                    hitTypeLabel(m_trafficDebug->nearestTrafficSensorHitType));
        ImGui::Text("ECS Pos: %.2f %.2f %.2f",
                    m_trafficDebug->nearestTrafficEcsPos[0],
                    m_trafficDebug->nearestTrafficEcsPos[1],
                    m_trafficDebug->nearestTrafficEcsPos[2]);
        ImGui::Text("Phys Pos: %.2f %.2f %.2f  Delta: %.2f",
                    m_trafficDebug->nearestTrafficPhysPos[0],
                    m_trafficDebug->nearestTrafficPhysPos[1],
                    m_trafficDebug->nearestTrafficPhysPos[2],
                    m_trafficDebug->nearestTrafficDesync);
      }
      else
      {
        ImGui::Text("Nearest: none");
      }

      if (isValidEntity(m_trafficDebug->stuckTrafficEntity))
      {
        ImGui::Text("Stuck: e=%u tier=%u",
                    m_trafficDebug->stuckTrafficEntity.index(),
                    (uint32_t)m_trafficDebug->stuckTrafficTier);
        ImGui::Text("Speed: %.2f  Target: %.2f  Lane: %u  s=%.2f",
                    m_trafficDebug->stuckTrafficSpeed,
                    m_trafficDebug->stuckTrafficTargetSpeed,
                    m_trafficDebug->stuckTrafficLaneId,
                    m_trafficDebug->stuckTrafficLaneS);
        ImGui::Text("Body Active: %s  Sensor: %.2f (%s)",
                    m_trafficDebug->stuckTrafficBodyActive ? "YES" : "NO",
                    m_trafficDebug->stuckTrafficSensorHitDistance,
                    hitTypeLabel(m_trafficDebug->stuckTrafficSensorHitType));
      }
      else
      {
        ImGui::Text("Stuck: none");
      }
      ImGui::Text("Spawns / Despawns: +%u / -%u",
                  m_trafficDebug->spawnsThisFrame,
                  m_trafficDebug->despawnsThisFrame);

      if (m_trafficDebug->nearestLaneId != kInvalidLaneId)
        ImGui::Text("Nearest Lane: %u", m_trafficDebug->nearestLaneId);
      else
        ImGui::Text("Nearest Lane: none");

      ImGui::Separator();
      ImGui::Text("Spawning");
      ImGui::SliderFloat("Density (veh/km^2)", &m_trafficDebug->densityPerKm2, 0.0f, 1000.0f, "%.1f");
      ImGui::SliderFloat("Player Exclusion (m)", &m_trafficDebug->playerExclusionRadius, 0.0f, 80.0f, "%.1f");

      ImGui::Separator();
      ImGui::Text("AI");
      ImGui::SliderFloat("LookAhead Dist", &m_trafficDebug->lookAheadDist, 2.0f, 40.0f, "%.1f");
      ImGui::SliderFloat("Safe Distance", &m_trafficDebug->safeDistance, 2.0f, 40.0f, "%.1f");
      ImGui::SliderFloat("Front Ray Length", &m_trafficDebug->frontRayLength, 5.0f, 60.0f, "%.1f");
      ImGui::SliderFloat("Speed Multiplier", &m_trafficDebug->speedMultiplier, 0.2f, 2.5f, "%.2f");

      ImGui::Separator();
      ImGui::Text("LOD");
      ImGui::SliderFloat("Tier A Enter", &m_trafficDebug->tierAEnter, 10.0f, 200.0f, "%.1f");
      ImGui::SliderFloat("Tier A Exit", &m_trafficDebug->tierAExit, 20.0f, 260.0f, "%.1f");
      ImGui::SliderFloat("Tier B Enter", &m_trafficDebug->tierBEnter, 30.0f, 300.0f, "%.1f");
      ImGui::SliderFloat("Tier B Exit", &m_trafficDebug->tierBExit, 40.0f, 400.0f, "%.1f");

      if (m_trafficDebug->tierAExit < m_trafficDebug->tierAEnter + 1.0f)
        m_trafficDebug->tierAExit = m_trafficDebug->tierAEnter + 1.0f;
      if (m_trafficDebug->tierBEnter < m_trafficDebug->tierAExit + 1.0f)
        m_trafficDebug->tierBEnter = m_trafficDebug->tierAExit + 1.0f;
      if (m_trafficDebug->tierBExit < m_trafficDebug->tierBEnter + 1.0f)
        m_trafficDebug->tierBExit = m_trafficDebug->tierBEnter + 1.0f;

      int maxTotal = static_cast<int>(m_trafficDebug->maxTrafficVehiclesTotal);
      int maxPhysics = static_cast<int>(m_trafficDebug->maxTrafficVehiclesPhysics);
      int maxKinematic = static_cast<int>(m_trafficDebug->maxTrafficVehiclesKinematic);
      if (ImGui::SliderInt("Max Total", &maxTotal, 0, 1000))
        m_trafficDebug->maxTrafficVehiclesTotal = static_cast<uint32_t>(std::max(maxTotal, 0));
      if (ImGui::SliderInt("Max Tier A", &maxPhysics, 0, 256))
        m_trafficDebug->maxTrafficVehiclesPhysics = static_cast<uint32_t>(std::max(maxPhysics, 0));
      if (ImGui::SliderInt("Max Tier B", &maxKinematic, 0, 512))
        m_trafficDebug->maxTrafficVehiclesKinematic = static_cast<uint32_t>(std::max(maxKinematic, 0));

      ImGui::Separator();
      ImGui::Text("Debug");
      ImGui::Checkbox("Show Lanes", &m_trafficDebug->showLanes);
      ImGui::Checkbox("Show Targets", &m_trafficDebug->showAgentTargets);
      ImGui::Checkbox("Show Sensors", &m_trafficDebug->showSensorRays);
      ImGui::Checkbox("Show Tier Colors", &m_trafficDebug->showTierColors);
    }

    ImGui::Separator();
    ImGui::Text("Assets / Materials");
    ImGui::Text("Textures: %u  Materials: %u  Mesh cache: %u",
                m_assetStats.textureCount, m_assetStats.materialCount, m_assetStats.meshCount);
    ImGui::Text("CPU bytes: %llu  GPU bytes(est): %llu",
                static_cast<unsigned long long>(m_assetStats.cpuBytes),
                static_cast<unsigned long long>(m_assetStats.gpuBytes));
    ImGui::Text("Resident textures: %u  Queued loads: %u  Evictions: %u",
                m_assetStats.residentTextures,
                m_assetStats.queuedTextureLoads,
                m_assetStats.evictions);
    ImGui::Text("GPU budget: %llu  Used: %llu  Eviction: %.2f ms",
                static_cast<unsigned long long>(m_assetStats.gpuBudgetBytes),
                static_cast<unsigned long long>(m_assetStats.gpuResidentBytes),
                m_assetStats.evictionMs);
    ImGui::Text("Texture cache H/M: %llu / %llu",
                static_cast<unsigned long long>(m_assetStats.textureCacheHits),
                static_cast<unsigned long long>(m_assetStats.textureCacheMisses));
    ImGui::Text("Material cache H/M: %llu / %llu",
                static_cast<unsigned long long>(m_assetStats.materialCacheHits),
                static_cast<unsigned long long>(m_assetStats.materialCacheMisses));
    ImGui::Text("Sampler anisotropy: %s (max %.1f)",
                m_assetStats.samplerAnisotropyEnabled ? "ON" : "OFF",
                m_assetStats.samplerMaxAnisotropy);

    if (!m_assetLabels.empty() && m_assetLabels.size() == m_assetMaterialIds.size())
    {
      std::vector<const char*> itemPtrs;
      itemPtrs.reserve(m_assetLabels.size());
      for (const std::string& label : m_assetLabels)
        itemPtrs.push_back(label.c_str());

      int comboIndex = static_cast<int>(m_assetSelectedIndex);
      if (ImGui::Combo("Scene Texture", &comboIndex, itemPtrs.data(), static_cast<int>(itemPtrs.size())))
      {
        m_assetSelectedIndex = static_cast<uint32_t>(comboIndex);
        const MaterialHandle selectedMaterial = m_assetMaterialIds[m_assetSelectedIndex];
        if (RenderMesh* triMesh = m_world ? m_world->get<RenderMesh>(m_triangleEntity) : nullptr)
          triMesh->materialId = selectedMaterial;
        if (RenderMesh* cubeMesh = m_world ? m_world->get<RenderMesh>(m_cubeEntity) : nullptr)
          cubeMesh->materialId = selectedMaterial;
      }
    }
    ImGui::End();
  }

  void DebugUI::draw(VkCommandBuffer cmd)
  {
    if (!m_initialized)
      return;
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  }

  bool DebugUI::onSwapchainRecreated(VkRenderPass newRenderPass, uint32_t newImageCount)
  {
    if (!m_initialized)
      return true;

    m_renderPass = newRenderPass;
    m_imageCount = newImageCount;

    vkDeviceWaitIdle(m_device);
    ImGui_ImplVulkan_Shutdown();

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = m_instance;
    info.PhysicalDevice = m_phys;
    info.Device = m_device;
    info.QueueFamily = m_queueFamily;
    info.Queue = m_queue;
    info.DescriptorPool = m_descriptorPool;
    info.RenderPass = m_renderPass;
    info.MinImageCount = m_imageCount;
    info.ImageCount = m_imageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check_vk_result;

    if (!ImGui_ImplVulkan_Init(&info))
    {
      sc::log(sc::LogLevel::Error, "ImGui_ImplVulkan_Init (recreate) failed.");
      return false;
    }

    if (!uploadFonts()) return false;
    return true;
  }

  bool DebugUI::createDescriptorPool()
  {
    VkDescriptorPoolSize pool_sizes[] =
    {
      { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
      { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
      { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * (uint32_t)(sizeof(pool_sizes) / sizeof(pool_sizes[0]));
    pool_info.poolSizeCount = (uint32_t)(sizeof(pool_sizes) / sizeof(pool_sizes[0]));
    pool_info.pPoolSizes = pool_sizes;

    VkResult r = vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_descriptorPool);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateDescriptorPool failed (%d)", (int)r);
      return false;
    }
    return true;
  }

  bool DebugUI::uploadFonts()
  {
    if (!ImGui_ImplVulkan_CreateFontsTexture())
    {
      sc::log(sc::LogLevel::Error, "ImGui_ImplVulkan_CreateFontsTexture failed.");
      return false;
    }
    return true;
  }
}
