#include "sc_assets.h"

#include "sc_log.h"
#include "sc_paths.h"
#include "sc_time.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace sc
{
  static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeFilter, VkMemoryPropertyFlags properties)
  {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
      if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        return i;
    }
    return UINT32_MAX;
  }

  static bool createBuffer(VkDevice device,
                           VkPhysicalDevice phys,
                           VkDeviceSize size,
                           VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkBuffer& buffer,
                           VkDeviceMemory& memory)
  {
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS)
      return false;

    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(device, buffer, &memReq);
    const uint32_t typeIndex = findMemoryType(phys, memReq.memoryTypeBits, properties);
    if (typeIndex == UINT32_MAX)
      return false;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS)
      return false;

    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
  }

  static bool createImage(VkDevice device,
                          VkPhysicalDevice phys,
                          uint32_t width,
                          uint32_t height,
                          uint32_t mipLevels,
                          VkFormat format,
                          VkImageUsageFlags usage,
                          VkImage& image,
                          VkDeviceMemory& memory)
  {
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent.width = width;
    ici.extent.height = height;
    ici.extent.depth = 1;
    ici.mipLevels = mipLevels;
    ici.arrayLayers = 1;
    ici.format = format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = usage;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &ici, nullptr, &image) != VK_SUCCESS)
      return false;

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);
    const uint32_t typeIndex = findMemoryType(phys, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (typeIndex == UINT32_MAX)
      return false;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS)
      return false;

    vkBindImageMemory(device, image, memory, 0);
    return true;
  }

  static VkCommandBuffer beginOneShot(VkDevice device, VkCommandPool cmdPool)
  {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS)
      return VK_NULL_HANDLE;

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    {
      vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
      return VK_NULL_HANDLE;
    }

    return cmd;
  }

  static bool endOneShot(VkDevice device, VkQueue queue, VkCommandPool cmdPool, VkCommandBuffer cmd)
  {
    if (!cmd)
      return false;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
      return false;

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
      return false;
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    return true;
  }

  static void transitionImageLayout(VkCommandBuffer cmd,
                                    VkImage image,
                                    VkImageLayout oldLayout,
                                    VkImageLayout newLayout,
                                    uint32_t mipLevels)
  {
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  static uint64_t materialCacheKey(const MaterialDesc& desc)
  {
    uint64_t key = static_cast<uint64_t>(desc.albedo);
    key ^= static_cast<uint64_t>(desc.unlit ? 0x9E3779B97F4A7C15ull : 0ull);
    key ^= static_cast<uint64_t>(desc.transparent ? 0xC2B2AE3D27D4EB4Full : 0ull);
    return key;
  }

  static VkFormat toVkFormat(TextureFormat format)
  {
    switch (format)
    {
      case TextureFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
      case TextureFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
      default: return VK_FORMAT_R8G8B8A8_SRGB;
    }
  }

  bool AssetManager::init(const AssetManagerInit& init)
  {
    m_device = init.device;
    m_phys = init.physicalDevice;
    m_graphicsQueue = init.graphicsQueue;
    m_commandPool = init.commandPool;
    m_materialDescriptorPool = init.materialDescriptorPool;
    m_materialSetLayout = init.materialSetLayout;
    m_samplerAnisotropyEnabled = init.samplerAnisotropyEnabled;
    m_samplerMaxAnisotropy = std::max(1.0f, init.samplerMaxAnisotropy);

    const std::array<unsigned char, 4> white = { 255, 255, 255, 255 };
    TextureHandle whiteHandle = kInvalidTextureHandle;
    if (!createTextureFromPixels("__default_white__", fnv1a64("__default_white__"),
                                 white.data(), 1, 1, false, true, whiteHandle))
    {
      sc::log(LogLevel::Error, "AssetManager: failed to create default white texture.");
      return false;
    }
    m_defaultWhiteTexture = whiteHandle;

    TextureHandle placeholder = createFallbackTexture("__placeholder__", fnv1a64("__placeholder__"));
    if (placeholder == kInvalidTextureHandle)
      placeholder = m_defaultWhiteTexture;
    m_placeholderTexture = placeholder;

    return true;
  }

  void AssetManager::shutdown()
  {
    for (TextureAsset& texture : m_textures)
    {
      destroyTextureGpu(texture);
      texture = TextureAsset{};
    }

    m_textures.clear();
    m_materials.clear();
    m_textureCache.clear();
    m_meshCache.clear();
    m_materialCache.clear();
    m_defaultWhiteTexture = kInvalidTextureHandle;
    m_placeholderTexture = kInvalidTextureHandle;
    m_textureLoadQueue.clear();
  }

  void AssetManager::setCommandContext(VkCommandPool commandPool, VkQueue graphicsQueue)
  {
    m_commandPool = commandPool;
    m_graphicsQueue = graphicsQueue;
  }

  TextureHandle AssetManager::loadTexture2D(const std::string& path, bool srgb)
  {
    const std::filesystem::path resolvedPath = resolveAssetPath(path);
    const std::string normalized = normalizePathForId(resolvedPath);
    const AssetId id = fnv1a64(normalized);

    const auto it = m_textureCache.find(id);
    if (it != m_textureCache.end())
    {
      ++m_textureCacheHits;
      const TextureHandle handle = it->second;
      if (handle < m_textures.size())
      {
        TextureAsset& tex = m_textures[handle];
        tex.lastUsedFrame = m_frameIndex;
        if (!tex.resident)
          requestTextureResident(handle);
      }
      return handle;
    }

    ++m_textureCacheMisses;

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(resolvedPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0)
    {
      sc::log(LogLevel::Warn, "AssetManager: failed to load texture '%s', creating fallback.", resolvedPath.string().c_str());
      const TextureHandle fallback = createFallbackTexture(path, id);
      m_textureCache[id] = fallback;
      return fallback;
    }

    TextureHandle handle = kInvalidTextureHandle;
    const bool ok = createTextureFromPixels(path, id, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), true, srgb, handle);
    stbi_image_free(pixels);
    if (!ok)
      return kInvalidTextureHandle;

    m_textureCache[id] = handle;
    return handle;
  }

  MeshHandle AssetManager::loadMesh(const std::string& path)
  {
    const std::string normalized = normalizePathForId(path);
    const AssetId id = fnv1a64(normalized);
    const auto it = m_meshCache.find(id);
    if (it != m_meshCache.end())
    {
      ++m_meshCacheHits;
      return it->second;
    }

    ++m_meshCacheMisses;
    return kInvalidMeshHandle;
  }

  void AssetManager::registerMeshAlias(const std::string& path, MeshHandle handle)
  {
    const AssetId id = fnv1a64(normalizePathForId(path));
    m_meshCache[id] = handle;
  }

  MaterialHandle AssetManager::createMaterial(const MaterialDesc& desc)
  {
    const uint64_t key = materialCacheKey(desc);
    const auto it = m_materialCache.find(key);
    if (it != m_materialCache.end())
    {
      ++m_materialCacheHits;
      return it->second;
    }

    ++m_materialCacheMisses;

    Material material{};
    material.desc = desc;
    if (material.desc.albedo == kInvalidTextureHandle)
      material.desc.albedo = m_defaultWhiteTexture;

    material.pipelineId = material.desc.unlit ? PipelineId::UnlitColor : PipelineId::Textured;

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_materialDescriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_materialSetLayout;
    if (vkAllocateDescriptorSets(m_device, &ai, &material.descriptorSet) != VK_SUCCESS)
    {
      sc::log(LogLevel::Error, "AssetManager: vkAllocateDescriptorSets (material) failed.");
      return kInvalidMaterialHandle;
    }

    if (!writeMaterialDescriptor(material))
      return kInvalidMaterialHandle;

    const MaterialHandle handle = static_cast<MaterialHandle>(m_materials.size());
    m_materials.push_back(material);
    m_materialCache[key] = handle;

    if (material.desc.albedo < m_textures.size())
    {
      TextureAsset& tex = m_textures[material.desc.albedo];
      if (!tex.resident)
        requestTextureResident(material.desc.albedo);
    }
    return handle;
  }

  const Material* AssetManager::getMaterial(MaterialHandle handle) const
  {
    if (handle >= m_materials.size())
      return nullptr;
    return &m_materials[handle];
  }

  const TextureAsset* AssetManager::getTexture(TextureHandle handle) const
  {
    if (handle >= m_textures.size())
      return nullptr;
    return &m_textures[handle];
  }

  AssetStatsSnapshot AssetManager::stats() const
  {
    AssetStatsSnapshot snap{};
    snap.textureCount = static_cast<uint32_t>(m_textures.size());
    snap.meshCount = static_cast<uint32_t>(m_meshCache.size());
    snap.materialCount = static_cast<uint32_t>(m_materials.size());
    snap.textureCacheHits = m_textureCacheHits;
    snap.textureCacheMisses = m_textureCacheMisses;
    snap.meshCacheHits = m_meshCacheHits;
    snap.meshCacheMisses = m_meshCacheMisses;
    snap.materialCacheHits = m_materialCacheHits;
    snap.materialCacheMisses = m_materialCacheMisses;
    snap.samplerAnisotropyEnabled = m_samplerAnisotropyEnabled;
    snap.samplerMaxAnisotropy = m_samplerMaxAnisotropy;

    for (const TextureAsset& texture : m_textures)
    {
      snap.cpuBytes += texture.cpuBytes;
      snap.gpuBytes += texture.gpuBytes;
      if (texture.resident)
      {
        snap.residentTextures++;
        snap.gpuResidentBytes += texture.gpuBytes;
      }
    }

    snap.queuedTextureLoads = static_cast<uint32_t>(m_textureLoadQueue.size());
    snap.evictions = static_cast<uint32_t>(m_evictionsThisFrame);
    snap.gpuBudgetBytes = m_residency.gpuBudgetBytes;
    snap.evictionMs = static_cast<float>(ticksToSeconds(m_evictionTicks) * 1000.0);

    return snap;
  }

  std::vector<TextureDebugEntry> AssetManager::textureDebugEntries() const
  {
    std::vector<TextureDebugEntry> out;
    out.reserve(m_textures.size());

    for (TextureHandle i = 0; i < m_textures.size(); ++i)
    {
      const TextureAsset& t = m_textures[i];
      TextureDebugEntry entry{};
      entry.handle = i;
      entry.label = t.path;
      entry.width = t.width;
      entry.height = t.height;
      entry.fromDisk = t.fromDisk;
      out.push_back(std::move(entry));
    }
    return out;
  }

  void AssetManager::beginFrame(uint64_t frameIndex)
  {
    m_frameIndex = frameIndex;
    m_evictionsThisFrame = 0;
    m_queuedLoadsThisFrame = 0;
    m_evictionTicks = 0;
  }

  void AssetManager::touchMaterial(MaterialHandle handle)
  {
    if (handle >= m_materials.size())
      return;

    const Material& mat = m_materials[handle];
    const TextureHandle texHandle = mat.desc.albedo;
    if (texHandle >= m_textures.size())
      return;

    TextureAsset& tex = m_textures[texHandle];
    tex.lastUsedFrame = m_frameIndex;
    if (!tex.resident)
      requestTextureResident(texHandle);
  }

  void AssetManager::requestTextureResident(TextureHandle handle)
  {
    if (handle >= m_textures.size())
      return;

    TextureAsset& tex = m_textures[handle];
    if (tex.resident || tex.loading)
      return;
    if (!tex.fromDisk)
    {
      tex.resident = true;
      tex.loading = false;
      tex.pinned = true;
      return;
    }

    tex.loading = true;
    m_textureLoadQueue.push_back(handle);
    m_queuedLoadsThisFrame++;
  }

  void AssetManager::pumpTextureLoads(uint32_t maxLoadsPerFrame)
  {
    const uint32_t limit = maxLoadsPerFrame == 0 ? 0 : maxLoadsPerFrame;
    uint32_t loaded = 0;

    while (loaded < limit && !m_textureLoadQueue.empty())
    {
      const TextureHandle handle = m_textureLoadQueue.front();
      m_textureLoadQueue.pop_front();

      if (handle >= m_textures.size())
        continue;

      TextureAsset& tex = m_textures[handle];
      if (tex.resident)
      {
        tex.loading = false;
        continue;
      }

      if (!reloadTexture(handle))
      {
        tex.loading = false;
        continue;
      }

      tex.loading = false;
      tex.lastUsedFrame = m_frameIndex;
      refreshMaterialsForTexture(handle);
      loaded++;
    }
  }

  void AssetManager::evictIfNeeded()
  {
    if (m_residency.freezeEviction)
      return;

    const Tick start = nowTicks();

    uint64_t gpuUsed = 0;
    uint32_t residentCount = 0;
    for (const TextureAsset& tex : m_textures)
    {
      if (tex.resident)
      {
        gpuUsed += tex.gpuBytes;
        residentCount++;
      }
    }

    const bool overBytes = (m_residency.gpuBudgetBytes > 0 && gpuUsed > m_residency.gpuBudgetBytes);
    const bool overCount = (m_residency.maxResidentTextures > 0 && residentCount > m_residency.maxResidentTextures);
    if (!overBytes && !overCount)
    {
      m_evictionTicks += nowTicks() - start;
      return;
    }

    struct Candidate
    {
      TextureHandle handle = kInvalidTextureHandle;
      uint64_t lastUsed = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(m_textures.size());
    for (TextureHandle i = 0; i < m_textures.size(); ++i)
    {
      const TextureAsset& tex = m_textures[i];
      if (!tex.resident)
        continue;
      if (tex.pinned)
        continue;
      if (!tex.fromDisk)
        continue;
      candidates.push_back({ i, tex.lastUsedFrame });
    }

    std::sort(candidates.begin(), candidates.end(),
      [&](const Candidate& a, const Candidate& b)
      {
        if (a.lastUsed != b.lastUsed) return a.lastUsed < b.lastUsed;
        return a.handle < b.handle;
      });

    for (const Candidate& c : candidates)
    {
      const bool stillOverBytes = (m_residency.gpuBudgetBytes > 0 && gpuUsed > m_residency.gpuBudgetBytes);
      const bool stillOverCount = (m_residency.maxResidentTextures > 0 && residentCount > m_residency.maxResidentTextures);
      if (!stillOverBytes && !stillOverCount)
        break;

      TextureAsset& tex = m_textures[c.handle];
      const uint64_t bytes = tex.gpuBytes;
      destroyTextureGpu(tex);
      tex.resident = false;
      tex.loading = false;
      tex.lastUsedFrame = 0;
      if (residentCount > 0)
        residentCount--;
      if (gpuUsed > bytes)
        gpuUsed -= bytes;
      else
        gpuUsed = 0;

      refreshMaterialsForTexture(c.handle);
      m_evictionsThisFrame++;
    }

    m_evictionTicks += nowTicks() - start;
  }

  bool AssetManager::createTextureFromPixels(const std::string& debugPath,
                                             AssetId assetId,
                                             const unsigned char* rgbaPixels,
                                             uint32_t width,
                                             uint32_t height,
                                             bool fromDisk,
                                             bool srgb,
                                             TextureHandle& outHandle)
  {
    const TextureFormat format = srgb ? TextureFormat::RGBA8_SRGB : TextureFormat::RGBA8_UNORM;
    TextureAsset texture{};
    texture.id = assetId;
    texture.path = debugPath;
    texture.width = width;
    texture.height = height;
    texture.mipLevels = 1;
    texture.cpuBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
    texture.gpuBytes = texture.cpuBytes;
    texture.fromDisk = fromDisk;
    texture.srgb = srgb;
    texture.format = format;
    texture.resident = true;
    texture.pinned = !fromDisk;
    texture.lastUsedFrame = m_frameIndex;
    texture.loading = false;

    if (!uploadTexturePixels(rgbaPixels, width, height, format, texture))
      return false;

    outHandle = static_cast<TextureHandle>(m_textures.size());
    m_textures.push_back(std::move(texture));
    return true;
  }

  bool AssetManager::reloadTexture(TextureHandle handle)
  {
    if (handle >= m_textures.size())
      return false;

    TextureAsset& texture = m_textures[handle];
    if (texture.path.empty())
      return false;

    const std::filesystem::path resolvedPath = resolveAssetPath(texture.path);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(resolvedPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0)
    {
      sc::log(LogLevel::Warn, "AssetManager: failed to reload texture '%s'.", resolvedPath.string().c_str());
      if (pixels)
        stbi_image_free(pixels);
      return false;
    }

    destroyTextureGpu(texture);

    texture.width = static_cast<uint32_t>(width);
    texture.height = static_cast<uint32_t>(height);
    texture.mipLevels = 1;
    texture.cpuBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
    texture.gpuBytes = texture.cpuBytes;
    texture.fromDisk = true;

    const bool ok = uploadTexturePixels(pixels,
                                        static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height),
                                        texture.format,
                                        texture);
    stbi_image_free(pixels);
    if (!ok)
      return false;

    texture.resident = true;
    return true;
  }

  void AssetManager::destroyTextureGpu(TextureAsset& texture)
  {
    if (texture.sampler) vkDestroySampler(m_device, texture.sampler, nullptr);
    if (texture.view) vkDestroyImageView(m_device, texture.view, nullptr);
    if (texture.image) vkDestroyImage(m_device, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(m_device, texture.memory, nullptr);
    texture.sampler = VK_NULL_HANDLE;
    texture.view = VK_NULL_HANDLE;
    texture.image = VK_NULL_HANDLE;
    texture.memory = VK_NULL_HANDLE;
    texture.gpuBytes = 0;
    texture.resident = false;
  }

  void AssetManager::refreshMaterialsForTexture(TextureHandle handle)
  {
    for (Material& material : m_materials)
    {
      if (material.desc.albedo == handle)
        writeMaterialDescriptor(material);
    }
  }

  bool AssetManager::uploadTexturePixels(const unsigned char* rgbaPixels,
                                         uint32_t width,
                                         uint32_t height,
                                         TextureFormat format,
                                         TextureAsset& outTexture)
  {
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4ull;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!createBuffer(m_device, m_phys, imageSize,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      stagingBuffer, stagingMemory))
    {
      return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped) != VK_SUCCESS)
      return false;
    std::memcpy(mapped, rgbaPixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);

    const VkFormat vkFormat = toVkFormat(format);
    if (!createImage(m_device, m_phys, width, height, 1,
                     vkFormat,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     outTexture.image, outTexture.memory))
    {
      return false;
    }

    VkCommandBuffer cmd = beginOneShot(m_device, m_commandPool);
    if (!cmd)
      return false;

    transitionImageLayout(cmd, outTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = { 0, 0, 0 };
    copy.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, stagingBuffer, outTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    transitionImageLayout(cmd, outTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    if (!endOneShot(m_device, m_graphicsQueue, m_commandPool, cmd))
      return false;

    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = outTexture.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = vkFormat;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &ivci, nullptr, &outTexture.view) != VK_SUCCESS)
      return false;

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = m_samplerAnisotropyEnabled ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = m_samplerAnisotropyEnabled ? m_samplerMaxAnisotropy : 1.0f;
    sci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    sci.compareEnable = VK_FALSE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.minLod = 0.0f;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(m_device, &sci, nullptr, &outTexture.sampler) != VK_SUCCESS)
      return false;

    return true;
  }

  bool AssetManager::writeMaterialDescriptor(Material& material)
  {
    const TextureAsset* texture = getTexture(material.desc.albedo);
    if (!texture || !texture->resident)
      texture = getTexture(m_placeholderTexture);
    if (!texture)
      return false;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = texture->sampler;
    imageInfo.imageView = texture->view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = material.descriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return true;
  }

  TextureHandle AssetManager::createFallbackTexture(const std::string& debugPath, AssetId id)
  {
    const std::array<unsigned char, 16> pixels =
    {
      255,   0, 255, 255,
        0,   0,   0, 255,
        0,   0,   0, 255,
      255,   0, 255, 255
    };

    TextureHandle handle = kInvalidTextureHandle;
    if (!createTextureFromPixels(debugPath, id, pixels.data(), 2, 2, false, true, handle))
    {
      sc::log(LogLevel::Error, "AssetManager: failed to create fallback texture.");
      return m_defaultWhiteTexture;
    }
    return handle;
  }
}
