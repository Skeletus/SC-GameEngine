#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

struct SDL_Window;

namespace sc
{
  struct VkConfig
  {
    bool enableValidation = true;
  };

  class VkRenderer
  {
  public:
    bool init(SDL_Window* window, const VkConfig& cfg);
    void shutdown();

    bool beginFrame();
    void endFrame();

  private:
    bool createInstance(SDL_Window* window);
    bool setupDebug();
    bool createSurface(SDL_Window* window);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    bool createRenderPass();
    bool createFramebuffers();
    bool createCommands();
    bool createSync();

    void destroySwapchainObjects();

  private:
    VkConfig m_cfg{};

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;

    uint32_t m_gfxFamily = UINT32_MAX;
    VkQueue  m_gfxQueue = VK_NULL_HANDLE;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent{};

    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainViews;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;

    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_cmdBuffers;

    VkSemaphore m_imageAvailable = VK_NULL_HANDLE;
    VkSemaphore m_renderFinished = VK_NULL_HANDLE;
    VkFence m_inFlight = VK_NULL_HANDLE;

    uint32_t m_imageIndex = 0;
  };
}
