#include "sc_imgui.h"
#include "sc_log.h"

#include <SDL.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_vulkan.h>

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

  void DebugUI::setWorldContext(World* world, Entity camera, Entity triangle, Entity root)
  {
    m_world = world;
    m_cameraEntity = camera;
    m_triangleEntity = triangle;
    m_rootEntity = root;
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
    ImGui::Checkbox("Pause Triangle", &m_pauseTriangle);
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
