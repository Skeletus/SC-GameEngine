#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

#include "sc_jobs.h"
#include "sc_memtrack.h"
#include "sc_ecs.h"
#include "sc_scheduler.h"

struct SDL_Window;
union SDL_Event;

namespace sc
{
  class DebugUI
  {
  public:
    bool init(SDL_Window* window,
              VkInstance instance,
              VkDevice device,
              VkPhysicalDevice phys,
              uint32_t queueFamily,
              VkQueue queue,
              VkRenderPass renderPass,
              uint32_t imageCount);

    void shutdown();

    void processEvent(const SDL_Event& e);
    void newFrame();
    void draw(VkCommandBuffer cmd);

    bool onSwapchainRecreated(VkRenderPass newRenderPass, uint32_t newImageCount);

    void setFrameStats(uint32_t frameIndex, uint32_t imageIndex, VkExtent2D extent);
    void setTelemetry(const JobsTelemetrySnapshot& jobs, const MemStats& mem);
    void setEcsStats(const EcsStatsSnapshot& ecs, const SchedulerStatsSnapshot& sched);
    bool isTrianglePaused() const { return m_pauseTriangle; }
    void setWorldContext(World* world, Entity camera, Entity triangle, Entity root);

  private:
    bool createDescriptorPool();
    bool uploadFonts();

  private:
    SDL_Window* m_window = nullptr;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    uint32_t m_imageCount = 0;

    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    VkExtent2D m_extent{};
    uint32_t m_frameIndex = 0;
    uint32_t m_imageIndex = 0;

    uint64_t m_lastCounter = 0;
    double m_freq = 0.0;
    float m_frameTimes[120]{};
    uint32_t m_frameOffset = 0;
    uint32_t m_frameCount = 0;

    bool m_pauseTriangle = false;
    bool m_initialized = false;

    JobsTelemetrySnapshot m_jobsSnap{};
    MemStats m_memSnap{};
    EcsStatsSnapshot m_ecsSnap{};
    SchedulerStatsSnapshot m_schedSnap{};

    World* m_world = nullptr;
    Entity m_cameraEntity = kInvalidEntity;
    Entity m_triangleEntity = kInvalidEntity;
    Entity m_rootEntity = kInvalidEntity;
  };
}
