#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

#include "sc_imgui.h"

struct SDL_Window;
union SDL_Event;

namespace sc
{
  struct VkConfig
  {
    bool enableValidation = true;
    bool enableDebugUI = true;
  };

  class VkRenderer
  {
  public:
    static constexpr uint32_t MAX_FRAMES = 2;

    bool init(SDL_Window* window, const VkConfig& cfg);
    void shutdown();

    SDL_Window* m_window = nullptr;

    void onSDLEvent(const SDL_Event& e);

    // Llama esto cuando SDL notifique resize (SIZE_CHANGED)
    void onResizeRequest() { m_swapchainDirty = true; }

    bool beginFrame();
    void endFrame();
    void setTelemetry(const JobsTelemetrySnapshot& jobs, const MemStats& mem);

  private:
    bool createInstance();
    bool setupDebug();
    bool createSurface(SDL_Window* window);
    bool pickPhysicalDevice();
    bool createDevice();

    bool createSwapchain();
    bool createRenderPass();
    bool createPipeline();
    bool createFramebuffers();
    bool createCommands();
    bool createSync();

    bool recreateSwapchain();
    void destroySwapchainObjects();
    void destroyPipeline();

    std::vector<uint8_t> readFile(const char* path);
    VkShaderModule createShaderModule(const std::vector<uint8_t>& code);

  private:
    VkConfig m_cfg{};

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;

    uint32_t m_gfxFamily = UINT32_MAX;
    VkQueue  m_gfxQueue = VK_NULL_HANDLE;

    // Swapchain + views
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent{};

    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainViews;

    // Render targets
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;

    // Pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    // Commands
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_cmdBuffers;

    // Sync per-frame (frames-in-flight)
    VkSemaphore m_imageAvailable[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkSemaphore m_renderFinished[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFence     m_inFlight[MAX_FRAMES]       = { VK_NULL_HANDLE, VK_NULL_HANDLE };

    uint32_t m_frameIndex = 0; // 0..MAX_FRAMES-1
    uint32_t m_imageIndex = 0; // swapchain image index actual

    bool m_swapchainDirty = false; // marcado por resize o out-of-date

    DebugUI m_debugUI{};
    JobsTelemetrySnapshot m_jobsSnap{};
    MemStats m_memSnap{};
  };
}
