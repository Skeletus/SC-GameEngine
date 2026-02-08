#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc
{
  using AssetId = uint64_t;
  using TextureHandle = uint32_t;
  using MeshHandle = uint32_t;
  using MaterialHandle = uint32_t;

  static constexpr TextureHandle kInvalidTextureHandle = UINT32_MAX;
  static constexpr MeshHandle kInvalidMeshHandle = UINT32_MAX;
  static constexpr MaterialHandle kInvalidMaterialHandle = UINT32_MAX;

  enum class PipelineId : uint32_t
  {
    UnlitColor = 0,
    Textured = 1
  };

  enum class TextureFormat : uint32_t
  {
    Unknown = 0,
    RGBA8_SRGB = 1,
    RGBA8_UNORM = 2
  };

  struct MaterialDesc
  {
    TextureHandle albedo = kInvalidTextureHandle;
    bool unlit = false;
    bool transparent = false;
  };

  struct Material
  {
    MaterialDesc desc{};
    PipelineId pipelineId = PipelineId::Textured;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  };

  struct TextureAsset
  {
    AssetId id = 0;
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    bool fromDisk = false;
    bool srgb = true;
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    bool resident = false;
    bool pinned = false;
    uint64_t lastUsedFrame = 0;
    bool loading = false;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
  };

  struct AssetStatsSnapshot
  {
    uint32_t textureCount = 0;
    uint32_t meshCount = 0;
    uint32_t materialCount = 0;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    uint64_t textureCacheHits = 0;
    uint64_t textureCacheMisses = 0;
    uint64_t meshCacheHits = 0;
    uint64_t meshCacheMisses = 0;
    uint64_t materialCacheHits = 0;
    uint64_t materialCacheMisses = 0;
    uint32_t residentTextures = 0;
    uint32_t queuedTextureLoads = 0;
    uint32_t evictions = 0;
    uint64_t gpuBudgetBytes = 0;
    uint64_t gpuResidentBytes = 0;
    float evictionMs = 0.0f;
    bool samplerAnisotropyEnabled = false;
    float samplerMaxAnisotropy = 1.0f;
  };

  struct TextureDebugEntry
  {
    TextureHandle handle = kInvalidTextureHandle;
    std::string label;
    uint32_t width = 0;
    uint32_t height = 0;
    bool fromDisk = false;
  };

  struct AssetManagerInit
  {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorPool materialDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    bool samplerAnisotropyEnabled = false;
    float samplerMaxAnisotropy = 1.0f;
  };

  struct AssetResidencyConfig
  {
    uint64_t gpuBudgetBytes = 256ull * 1024ull * 1024ull;
    uint32_t maxResidentTextures = 512u;
    uint32_t maxTextureLoadsPerFrame = 2u;
    bool freezeEviction = false;
  };

  class AssetManager
  {
  public:
    bool init(const AssetManagerInit& init);
    void shutdown();

    void setCommandContext(VkCommandPool commandPool, VkQueue graphicsQueue);

    TextureHandle loadTexture2D(const std::string& path, bool srgb = true);
    MeshHandle loadMesh(const std::string& path);
    void registerMeshAlias(const std::string& path, MeshHandle handle);
    MaterialHandle createMaterial(const MaterialDesc& desc);

    const Material* getMaterial(MaterialHandle handle) const;
    const TextureAsset* getTexture(TextureHandle handle) const;
    bool reloadTexture(TextureHandle handle);
    TextureHandle placeholderTexture() const { return m_placeholderTexture; }

    AssetStatsSnapshot stats() const;
    std::vector<TextureDebugEntry> textureDebugEntries() const;

    const AssetResidencyConfig& residencyConfig() const { return m_residency; }
    void setResidencyConfig(const AssetResidencyConfig& cfg) { m_residency = cfg; }
    void setFreezeEviction(bool freeze) { m_residency.freezeEviction = freeze; }

    void beginFrame(uint64_t frameIndex);
    void touchMaterial(MaterialHandle handle);
    void touchMesh(MeshHandle /*handle*/) {}
    void requestTextureResident(TextureHandle handle);
    void pumpTextureLoads(uint32_t maxLoadsPerFrame);
    void evictIfNeeded();

  private:
    bool createTextureFromPixels(const std::string& debugPath,
                                 AssetId assetId,
                                 const unsigned char* rgbaPixels,
                                 uint32_t width,
                                 uint32_t height,
                                 bool fromDisk,
                                 bool srgb,
                                 TextureHandle& outHandle);
    void destroyTextureGpu(TextureAsset& texture);
    void refreshMaterialsForTexture(TextureHandle handle);
    bool uploadTexturePixels(const unsigned char* rgbaPixels,
                             uint32_t width,
                             uint32_t height,
                             TextureFormat format,
                             TextureAsset& outTexture);
    bool writeMaterialDescriptor(Material& material);
    TextureHandle createFallbackTexture(const std::string& debugPath, AssetId id);

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkDescriptorPool m_materialDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
    bool m_samplerAnisotropyEnabled = false;
    float m_samplerMaxAnisotropy = 1.0f;

    TextureHandle m_defaultWhiteTexture = kInvalidTextureHandle;
    TextureHandle m_placeholderTexture = kInvalidTextureHandle;

    std::vector<TextureAsset> m_textures;
    std::vector<Material> m_materials;

    std::unordered_map<AssetId, TextureHandle> m_textureCache;
    std::unordered_map<AssetId, MeshHandle> m_meshCache;
    std::unordered_map<uint64_t, MaterialHandle> m_materialCache;

    uint64_t m_textureCacheHits = 0;
    uint64_t m_textureCacheMisses = 0;
    uint64_t m_meshCacheHits = 0;
    uint64_t m_meshCacheMisses = 0;
    uint64_t m_materialCacheHits = 0;
    uint64_t m_materialCacheMisses = 0;

    AssetResidencyConfig m_residency{};
    uint64_t m_frameIndex = 0;
    uint64_t m_evictionsThisFrame = 0;
    uint64_t m_queuedLoadsThisFrame = 0;
    uint64_t m_evictionTicks = 0;
    std::deque<TextureHandle> m_textureLoadQueue;
  };
}
