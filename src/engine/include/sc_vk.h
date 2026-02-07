#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <string>

#include "sc_imgui.h"
#include "sc_assets.h"

struct SDL_Window;
union SDL_Event;

namespace sc
{
  struct RenderFrameData;
  class DebugDraw;
  struct WorldStreamingState;
  struct CullingState;
  struct RenderPrepStreamingState;
  struct PhysicsDebugState;
  struct VehicleDebugState;
  struct TrafficDebugState;

  struct MeshVertex
  {
    float pos[3]{};
    float color[3]{};
    float uv[2]{};
  };

  struct GpuMesh
  {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
  };

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
    void setEcsStats(const EcsStatsSnapshot& ecs, const SchedulerStatsSnapshot& sched);
    void setRenderFrame(const RenderFrameData* frame) { m_renderFrame = frame; }
    void setDebugWorld(World* world, Entity camera, Entity triangle, Entity cube, Entity root);
    void setWorldStreamingContext(WorldStreamingState* streaming,
                                  CullingState* culling,
                                  RenderPrepStreamingState* renderPrep);
    void setPhysicsContext(PhysicsDebugState* physics);
    void setVehicleContext(VehicleDebugState* vehicle);
    void setTrafficContext(TrafficDebugState* traffic);
    void setDebugDraw(DebugDraw* draw) { m_debugDraw = draw; m_debugUI.setDebugDraw(draw); }
    AssetManager& assets() { return m_assets; }
    const AssetManager& assets() const { return m_assets; }

    float swapchainAspect() const;

  private:
    bool createInstance();
    bool setupDebug();
    bool createSurface(SDL_Window* window);
    bool pickPhysicalDevice();
    bool createDevice();

    bool createSwapchain();
    bool createRenderPass();
    bool createPipeline();
    bool createDepthResources();
    bool createFramebuffers();
    bool createCommands();
    bool createSync();
    bool createMeshes();
    void destroyMeshes();
    bool createDebugDrawBuffers(size_t vertexCapacity);
    void destroyDebugDrawBuffers();

    bool recreateSwapchain();
    void destroySwapchainObjects();
    void destroyPipeline();

    std::vector<uint8_t> readFile(const char* path);
    VkShaderModule createShaderModule(const std::vector<uint8_t>& code);

    bool createDescriptorSetLayout();
    bool createUniformBuffers();
    bool createDescriptorPool();
    bool allocateDescriptorSets();
    bool createDefaultAssets();
    void buildAssetUiSnapshot();
    void destroyUniformBuffers();
    VkFormat findDepthFormat() const;

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
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> m_depthImages;
    std::vector<VkDeviceMemory> m_depthMemory;
    std::vector<VkImageView> m_depthViews;

    // Pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_unlitPipeline = VK_NULL_HANDLE;
    VkPipeline m_texturedPipeline = VK_NULL_HANDLE;
    VkPipeline m_debugPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_globalDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_globalSets[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkBuffer m_cameraBuffers[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory m_cameraMemory[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    void* m_cameraMapped[MAX_FRAMES] = { nullptr, nullptr };

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
    EcsStatsSnapshot m_ecsSnap{};
    SchedulerStatsSnapshot m_schedSnap{};
    const RenderFrameData* m_renderFrame = nullptr;
    DebugDraw* m_debugDraw = nullptr;

    std::vector<GpuMesh> m_meshes;
    AssetManager m_assets{};
    std::vector<std::string> m_textureOptionLabels;
    std::vector<MaterialHandle> m_textureOptionMaterials;
    uint32_t m_sceneTextureSelection = 0;
    bool m_samplerAnisotropyEnabled = false;
    float m_samplerMaxAnisotropy = 1.0f;

    VkBuffer m_debugVertexBuffers[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory m_debugVertexMemory[MAX_FRAMES] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    void* m_debugVertexMapped[MAX_FRAMES] = { nullptr, nullptr };
    size_t m_debugVertexCapacity = 0;
  };
}
