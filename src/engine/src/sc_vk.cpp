#include "sc_vk.h"
#include "sc_log.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vector>
#include <cstring>
#include <algorithm>
#include <fstream>

namespace sc
{
  struct CameraUbo
  {
    Mat4 viewProj{};
  };

  static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeFilter, VkMemoryPropertyFlags properties);
  static bool createBuffer(VkDevice device,
                           VkPhysicalDevice phys,
                           VkDeviceSize size,
                           VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkBuffer& buffer,
                           VkDeviceMemory& memory);
  static bool createImage(VkDevice device,
                          VkPhysicalDevice phys,
                          uint32_t width,
                          uint32_t height,
                          VkFormat format,
                          VkImageTiling tiling,
                          VkImageUsageFlags usage,
                          VkMemoryPropertyFlags properties,
                          VkImage& image,
                          VkDeviceMemory& memory);
  static VkImageView createImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectMask);
  static bool hasStencilComponent(VkFormat format);

  // ---- debug messenger helpers ----
  static VKAPI_ATTR VkBool32 VKAPI_CALL vkDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* cb,
    void* user)
  {
    (void)type; (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
      sc::log(sc::LogLevel::Warn, "Vulkan: %s", cb->pMessage);
    else
      sc::log(sc::LogLevel::Debug, "Vulkan: %s", cb->pMessage);
    return VK_FALSE;
  }

  static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pMessenger)
  {
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(instance, pCreateInfo, pAllocator, pMessenger);
  }

  static void DestroyDebugUtilsMessengerEXT(VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* pAllocator)
  {
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(instance, messenger, pAllocator);
  }

  bool VkRenderer::init(SDL_Window* window, const VkConfig& cfg)
  {
    m_cfg = cfg;
    m_window = window;

    if (!createInstance()) return false;
    if (m_cfg.enableValidation && !setupDebug()) return false;
    if (!createSurface(window)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createRenderPass()) return false;
    if (!createDepthResources()) return false;
    if (!createDescriptorSetLayout()) return false;
    if (!createUniformBuffers()) return false;
    if (!createDescriptorPool()) return false;
    if (!allocateDescriptorSets()) return false;
    if (m_cfg.enableDebugUI)
    {
      if (!m_debugUI.init(m_window, m_instance, m_device, m_phys, m_gfxFamily, m_gfxQueue, m_renderPass, (uint32_t)m_swapchainImages.size())) return false;
    }
    if (!createPipeline()) return false;
    if (!createMeshes()) return false;
    if (!createDebugDrawBuffers(65536)) return false;
    if (!createFramebuffers()) return false;
    if (!createCommands()) return false;
    if (!createSync()) return false;

    sc::log(sc::LogLevel::Info, "Vulkan init OK.");
    return true;
  }

  void VkRenderer::shutdown()
  {
    if (m_device) vkDeviceWaitIdle(m_device);

    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      if (m_inFlight[i]) vkDestroyFence(m_device, m_inFlight[i], nullptr);
      if (m_renderFinished[i]) vkDestroySemaphore(m_device, m_renderFinished[i], nullptr);
      if (m_imageAvailable[i]) vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
    }

    if (m_cmdPool) vkDestroyCommandPool(m_device, m_cmdPool, nullptr);

    destroySwapchainObjects();
    destroyPipeline();
    destroyDebugDrawBuffers();
    destroyMeshes();
    m_debugUI.shutdown();

    destroyUniformBuffers();
    if (m_globalDescriptorPool) { vkDestroyDescriptorPool(m_device, m_globalDescriptorPool, nullptr); m_globalDescriptorPool = VK_NULL_HANDLE; }
    if (m_globalSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_globalSetLayout, nullptr); m_globalSetLayout = VK_NULL_HANDLE; }

    if (m_renderPass) vkDestroyRenderPass(m_device, m_renderPass, nullptr);

    if (m_swapchain) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    if (m_device) vkDestroyDevice(m_device, nullptr);

    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);

    if (m_cfg.enableValidation && m_debugMessenger)
      DestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);

    if (m_instance) vkDestroyInstance(m_instance, nullptr);

    *this = VkRenderer{};
    sc::log(sc::LogLevel::Info, "Vulkan shutdown complete.");
  }

  bool VkRenderer::createInstance()
  {
    // SDL required extensions
    unsigned extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(nullptr, &extCount, nullptr) || extCount == 0)
    {
      sc::log(sc::LogLevel::Error, "SDL_Vulkan_GetInstanceExtensions failed.");
      return false;
    }

    std::vector<const char*> exts(extCount);
    SDL_Vulkan_GetInstanceExtensions(nullptr, &extCount, exts.data());

    if (m_cfg.enableValidation)
    {
      exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    std::vector<const char*> layers;
    if (m_cfg.enableValidation)
      layers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "SandboxCityEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "SandboxCityEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateInstance failed (%d)", (int)r);
      return false;
    }
    return true;
  }

  bool VkRenderer::setupDebug()
  {
    VkDebugUtilsMessengerCreateInfoEXT ci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    ci.messageSeverity =
      VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType =
      VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = vkDebugCallback;

    VkResult r = CreateDebugUtilsMessengerEXT(m_instance, &ci, nullptr, &m_debugMessenger);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Warn, "Debug messenger not available (%d). Continuing.", (int)r);
      m_debugMessenger = VK_NULL_HANDLE;
    }
    return true;
  }

  bool VkRenderer::createSurface(SDL_Window* window)
  {
    if (!SDL_Vulkan_CreateSurface(window, m_instance, &m_surface))
    {
      sc::log(sc::LogLevel::Error, "SDL_Vulkan_CreateSurface failed.");
      return false;
    }
    return true;
  }

  bool VkRenderer::pickPhysicalDevice()
  {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0)
    {
      sc::log(sc::LogLevel::Error, "No Vulkan physical devices found.");
      return false;
    }

    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devs.data());

    // Simple scoring: prefer discrete GPU
    auto score = [](VkPhysicalDevice d)
    {
      VkPhysicalDeviceProperties p{};
      vkGetPhysicalDeviceProperties(d, &p);
      int s = 0;
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) s += 1000;
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) s += 100;
      s += (int)p.limits.maxImageDimension2D;
      return s;
    };

    std::sort(devs.begin(), devs.end(), [&](auto a, auto b) { return score(a) > score(b); });

    // Find a device with graphics + present support
    for (auto d : devs)
    {
      uint32_t qCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qCount);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, qs.data());

      for (uint32_t i = 0; i < qCount; ++i)
      {
        if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;

        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(d, i, m_surface, &present);
        if (!present) continue;

        m_phys = d;
        m_gfxFamily = i;

        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        sc::log(sc::LogLevel::Info, "GPU: %s", p.deviceName);
        return true;
      }
    }

    sc::log(sc::LogLevel::Error, "No suitable GPU found (needs graphics+present).");
    return false;
  }

  bool VkRenderer::createDevice()
  {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = m_gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;

    VkResult r = vkCreateDevice(m_phys, &dci, nullptr, &m_device);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateDevice failed (%d)", (int)r);
      return false;
    }

    vkGetDeviceQueue(m_device, m_gfxFamily, 0, &m_gfxQueue);
    return true;
  }

  bool VkRenderer::createSwapchain()
  {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_phys, m_surface, &caps);

    uint32_t fCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, m_surface, &fCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, m_surface, &fCount, formats.data());

    uint32_t pCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_phys, m_surface, &pCount, nullptr);
    std::vector<VkPresentModeKHR> presents(pCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_phys, m_surface, &pCount, presents.data());

    // Choose format (prefer SRGB)
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats)
    {
      if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB)
      {
        chosen = f;
        break;
      }
    }

    // Choose present mode (FIFO is guaranteed; mailbox if available)
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (auto& m : presents)
    {
      if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF)
    {
      int w = 0, h = 0;
      SDL_Vulkan_GetDrawableSize(m_window, &w, &h); // necesitas pasar window a createSwapchain o guardar SDL_Window*
      extent.width  = (uint32_t)w;
      extent.height = (uint32_t)h;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
      imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pm;
    ci.clipped = VK_TRUE;

    VkResult r = vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateSwapchainKHR failed (%d)", (int)r);
      return false;
    }

    m_swapchainFormat = chosen.format;
    m_swapchainExtent = extent;

    uint32_t scCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &scCount, nullptr);
    m_swapchainImages.resize(scCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &scCount, m_swapchainImages.data());

    m_swapchainViews.resize(scCount);
    for (uint32_t i = 0; i < scCount; ++i)
    {
      VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
      vci.image = m_swapchainImages[i];
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = m_swapchainFormat;
      vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vci.subresourceRange.levelCount = 1;
      vci.subresourceRange.layerCount = 1;

      VkResult vr = vkCreateImageView(m_device, &vci, nullptr, &m_swapchainViews[i]);
      if (vr != VK_SUCCESS)
      {
        sc::log(sc::LogLevel::Error, "vkCreateImageView failed (%d)", (int)vr);
        return false;
      }
    }

    sc::log(sc::LogLevel::Info, "Swapchain: %ux%u format=%d images=%u",
      m_swapchainExtent.width, m_swapchainExtent.height, (int)m_swapchainFormat, (uint32_t)m_swapchainImages.size());
    return true;
  }

  bool VkRenderer::createRenderPass()
  {
    m_depthFormat = findDepthFormat();
    if (m_depthFormat == VK_FORMAT_UNDEFINED)
    {
      sc::log(sc::LogLevel::Error, "No supported depth format found.");
      return false;
    }

    VkAttachmentDescription color{};
    color.format = m_swapchainFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = m_depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[2] = { color, depth };

    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    VkResult r = vkCreateRenderPass(m_device, &rpci, nullptr, &m_renderPass);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateRenderPass failed (%d)", (int)r);
      return false;
    }
    return true;
  }

  std::vector<uint8_t> VkRenderer::readFile(const char* path)
  {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
      sc::log(sc::LogLevel::Error, "Shader file not found: %s", path);
      return {};
    }

    const size_t size = (size_t)file.tellg();
    std::vector<uint8_t> buffer(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    file.close();
    return buffer;
  }

  VkShaderModule VkRenderer::createShaderModule(const std::vector<uint8_t>& code)
  {
    if (code.empty())
      return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(m_device, &ci, nullptr, &module);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateShaderModule failed (%d)", (int)r);
      return VK_NULL_HANDLE;
    }
    return module;
  }

  bool VkRenderer::createDescriptorSetLayout()
  {
    VkDescriptorSetLayoutBinding camBinding{};
    camBinding.binding = 0;
    camBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    camBinding.descriptorCount = 1;
    camBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = 1;
    ci.pBindings = &camBinding;

    VkResult r = vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &m_globalSetLayout);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateDescriptorSetLayout failed (%d)", (int)r);
      return false;
    }
    return true;
  }

  bool VkRenderer::createUniformBuffers()
  {
    const VkDeviceSize size = sizeof(CameraUbo);

    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      if (!createBuffer(m_device, m_phys, size,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_cameraBuffers[i], m_cameraMemory[i]))
      {
        sc::log(sc::LogLevel::Error, "createBuffer (camera UBO) failed.");
        return false;
      }

      if (vkMapMemory(m_device, m_cameraMemory[i], 0, size, 0, &m_cameraMapped[i]) != VK_SUCCESS)
      {
        sc::log(sc::LogLevel::Error, "vkMapMemory (camera UBO) failed.");
        return false;
      }
    }
    return true;
  }

  bool VkRenderer::createDescriptorPool()
  {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES;

    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &poolSize;

    VkResult r = vkCreateDescriptorPool(m_device, &pci, nullptr, &m_globalDescriptorPool);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateDescriptorPool failed (%d)", (int)r);
      return false;
    }
    return true;
  }

  bool VkRenderer::allocateDescriptorSets()
  {
    VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_globalSetLayout, m_globalSetLayout };
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_globalDescriptorPool;
    ai.descriptorSetCount = MAX_FRAMES;
    ai.pSetLayouts = layouts;

    VkResult r = vkAllocateDescriptorSets(m_device, &ai, m_globalSets);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkAllocateDescriptorSets failed (%d)", (int)r);
      return false;
    }

    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      VkDescriptorBufferInfo bi{};
      bi.buffer = m_cameraBuffers[i];
      bi.offset = 0;
      bi.range = sizeof(CameraUbo);

      VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
      write.dstSet = m_globalSets[i];
      write.dstBinding = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write.descriptorCount = 1;
      write.pBufferInfo = &bi;

      vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
    return true;
  }

  void VkRenderer::destroyUniformBuffers()
  {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      if (m_cameraMapped[i])
      {
        vkUnmapMemory(m_device, m_cameraMemory[i]);
        m_cameraMapped[i] = nullptr;
      }
      if (m_cameraBuffers[i]) { vkDestroyBuffer(m_device, m_cameraBuffers[i], nullptr); m_cameraBuffers[i] = VK_NULL_HANDLE; }
      if (m_cameraMemory[i]) { vkFreeMemory(m_device, m_cameraMemory[i], nullptr); m_cameraMemory[i] = VK_NULL_HANDLE; }
    }
  }

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

  VkFormat VkRenderer::findDepthFormat() const
  {
    const VkFormat candidates[] =
    {
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (VkFormat format : candidates)
    {
      VkFormatProperties props{};
      vkGetPhysicalDeviceFormatProperties(m_phys, format, &props);
      if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
        return format;
    }
    return VK_FORMAT_UNDEFINED;
  }

  bool VkRenderer::createDepthResources()
  {
    if (m_depthFormat == VK_FORMAT_UNDEFINED)
      return false;

    m_depthImages.resize(m_swapchainImages.size());
    m_depthMemory.resize(m_swapchainImages.size());
    m_depthViews.resize(m_swapchainImages.size());

    const VkImageAspectFlags aspect =
      VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencilComponent(m_depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);

    for (size_t i = 0; i < m_depthImages.size(); ++i)
    {
      if (!createImage(m_device, m_phys,
                       m_swapchainExtent.width, m_swapchainExtent.height,
                       m_depthFormat, VK_IMAGE_TILING_OPTIMAL,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_depthImages[i], m_depthMemory[i]))
      {
        sc::log(sc::LogLevel::Error, "createImage (depth) failed.");
        return false;
      }

      m_depthViews[i] = createImageView(m_device, m_depthImages[i], m_depthFormat, aspect);
      if (!m_depthViews[i])
      {
        sc::log(sc::LogLevel::Error, "createImageView (depth) failed.");
        return false;
      }
    }
    return true;
  }

  static bool createImage(VkDevice device,
                          VkPhysicalDevice phys,
                          uint32_t width,
                          uint32_t height,
                          VkFormat format,
                          VkImageTiling tiling,
                          VkImageUsageFlags usage,
                          VkMemoryPropertyFlags properties,
                          VkImage& image,
                          VkDeviceMemory& memory)
  {
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent.width = width;
    ici.extent.height = height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.format = format;
    ici.tiling = tiling;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = usage;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &ici, nullptr, &image) != VK_SUCCESS)
      return false;

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);

    const uint32_t typeIndex = findMemoryType(phys, memReq.memoryTypeBits, properties);
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

  static VkImageView createImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectMask)
  {
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = aspectMask;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &vci, nullptr, &view) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    return view;
  }

  static bool hasStencilComponent(VkFormat format)
  {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  }

  bool VkRenderer::createPipeline()
  {
    const char* vsPath = "shaders/mesh.vert.spv";
    const char* fsPath = "shaders/mesh.frag.spv";

    auto vsCode = readFile(vsPath);
    auto fsCode = readFile(fsPath);
    if (vsCode.empty() || fsCode.empty())
      return false;

    VkShaderModule vs = createShaderModule(vsCode);
    VkShaderModule fs = createShaderModule(fsCode);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
    {
      if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
      if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(MeshVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;

    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = sizeof(float) * 3;

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cbAttach{};
    cbAttach.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cbAttach.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cbAttach;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = (uint32_t)(sizeof(dynStates) / sizeof(dynStates[0]));
    dyn.pDynamicStates = dynStates;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(Mat4);

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_globalSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkResult lr = vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipelineLayout);
    if (lr != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreatePipelineLayout failed (%d)", (int)lr);
      vkDestroyShaderModule(m_device, vs, nullptr);
      vkDestroyShaderModule(m_device, fs, nullptr);
      return false;
    }

    VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = m_pipelineLayout;
    gpci.renderPass = m_renderPass;
    gpci.subpass = 0;

    VkResult pr = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_pipeline);
    if (pr == VK_SUCCESS)
    {
      VkPipelineInputAssemblyStateCreateInfo iaLine = ia;
      iaLine.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

      VkPipelineDepthStencilStateCreateInfo dsLine = ds;
      dsLine.depthWriteEnable = VK_FALSE;

      gpci.pInputAssemblyState = &iaLine;
      gpci.pDepthStencilState = &dsLine;

      pr = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gpci, nullptr, &m_debugPipeline);
    }
    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);

    if (pr != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateGraphicsPipelines failed (%d)", (int)pr);
      return false;
    }

    sc::log(sc::LogLevel::Info, "Pipelines created (mesh + debug lines).");
    return true;
  }

  void VkRenderer::destroyPipeline()
  {
    if (m_debugPipeline) { vkDestroyPipeline(m_device, m_debugPipeline, nullptr); m_debugPipeline = VK_NULL_HANDLE; }
    if (m_pipeline) { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
  }

  bool VkRenderer::createFramebuffers()
  {
    m_framebuffers.resize(m_swapchainViews.size());
    for (size_t i = 0; i < m_swapchainViews.size(); ++i)
    {
      VkImageView attachments[] = { m_swapchainViews[i], m_depthViews[i] };

      VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
      fci.renderPass = m_renderPass;
      fci.attachmentCount = 2;
      fci.pAttachments = attachments;
      fci.width = m_swapchainExtent.width;
      fci.height = m_swapchainExtent.height;
      fci.layers = 1;

      VkResult r = vkCreateFramebuffer(m_device, &fci, nullptr, &m_framebuffers[i]);
      if (r != VK_SUCCESS)
      {
        sc::log(sc::LogLevel::Error, "vkCreateFramebuffer failed (%d)", (int)r);
        return false;
      }
    }
    return true;
  }

  bool VkRenderer::createCommands()
  {
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = m_gfxFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult r = vkCreateCommandPool(m_device, &pci, nullptr, &m_cmdPool);
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkCreateCommandPool failed (%d)", (int)r);
      return false;
    }

    m_cmdBuffers.resize(m_swapchainImages.size());
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = m_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)m_cmdBuffers.size();

    r = vkAllocateCommandBuffers(m_device, &ai, m_cmdBuffers.data());
    if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkAllocateCommandBuffers failed (%d)", (int)r);
      return false;
    }
    return true;
  }

  bool VkRenderer::createSync()
  {
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      if (vkCreateSemaphore(m_device, &sci, nullptr, &m_imageAvailable[i]) != VK_SUCCESS) return false;
      if (vkCreateSemaphore(m_device, &sci, nullptr, &m_renderFinished[i]) != VK_SUCCESS) return false;
      if (vkCreateFence(m_device, &fci, nullptr, &m_inFlight[i]) != VK_SUCCESS) return false;
    }
    return true;
  }

  bool VkRenderer::createMeshes()
  {
    m_meshes.clear();
    m_meshes.resize(2);

    const MeshVertex triVerts[] =
    {
      { { -0.5f, -0.5f, 0.0f }, { 1.0f, 0.2f, 0.2f } },
      { {  0.5f, -0.5f, 0.0f }, { 0.2f, 1.0f, 0.2f } },
      { {  0.0f,  0.5f, 0.0f }, { 0.2f, 0.4f, 1.0f } }
    };
    const uint32_t triIndices[] = { 0, 1, 2 };

    const MeshVertex cubeVerts[] =
    {
      { { -0.5f, -0.5f, -0.5f }, { 0.9f, 0.2f, 0.2f } },
      { {  0.5f, -0.5f, -0.5f }, { 0.2f, 0.9f, 0.2f } },
      { {  0.5f,  0.5f, -0.5f }, { 0.2f, 0.6f, 0.9f } },
      { { -0.5f,  0.5f, -0.5f }, { 0.9f, 0.9f, 0.2f } },
      { { -0.5f, -0.5f,  0.5f }, { 0.9f, 0.2f, 0.9f } },
      { {  0.5f, -0.5f,  0.5f }, { 0.2f, 0.9f, 0.9f } },
      { {  0.5f,  0.5f,  0.5f }, { 0.9f, 0.5f, 0.2f } },
      { { -0.5f,  0.5f,  0.5f }, { 0.7f, 0.7f, 0.7f } }
    };
    const uint32_t cubeIndices[] =
    {
      0, 1, 2, 2, 3, 0,
      4, 5, 6, 6, 7, 4,
      0, 4, 7, 7, 3, 0,
      1, 5, 6, 6, 2, 1,
      3, 2, 6, 6, 7, 3,
      0, 1, 5, 5, 4, 0
    };

    struct MeshSource
    {
      const MeshVertex* verts = nullptr;
      uint32_t vertCount = 0;
      const uint32_t* indices = nullptr;
      uint32_t indexCount = 0;
    };

    const MeshSource sources[] =
    {
      { triVerts, (uint32_t)(sizeof(triVerts) / sizeof(triVerts[0])), triIndices, (uint32_t)(sizeof(triIndices) / sizeof(triIndices[0])) },
      { cubeVerts, (uint32_t)(sizeof(cubeVerts) / sizeof(cubeVerts[0])), cubeIndices, (uint32_t)(sizeof(cubeIndices) / sizeof(cubeIndices[0])) }
    };

    for (size_t i = 0; i < m_meshes.size(); ++i)
    {
      const MeshSource& src = sources[i];
      GpuMesh& mesh = m_meshes[i];

      const VkDeviceSize vsize = sizeof(MeshVertex) * src.vertCount;
      if (!createBuffer(m_device, m_phys, vsize,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        mesh.vertexBuffer, mesh.vertexMemory))
      {
        sc::log(sc::LogLevel::Error, "createBuffer (mesh vertex) failed.");
        return false;
      }

      void* vdst = nullptr;
      if (vkMapMemory(m_device, mesh.vertexMemory, 0, vsize, 0, &vdst) != VK_SUCCESS)
        return false;
      std::memcpy(vdst, src.verts, (size_t)vsize);
      vkUnmapMemory(m_device, mesh.vertexMemory);

      const VkDeviceSize isize = sizeof(uint32_t) * src.indexCount;
      if (!createBuffer(m_device, m_phys, isize,
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        mesh.indexBuffer, mesh.indexMemory))
      {
        sc::log(sc::LogLevel::Error, "createBuffer (mesh index) failed.");
        return false;
      }

      void* idst = nullptr;
      if (vkMapMemory(m_device, mesh.indexMemory, 0, isize, 0, &idst) != VK_SUCCESS)
        return false;
      std::memcpy(idst, src.indices, (size_t)isize);
      vkUnmapMemory(m_device, mesh.indexMemory);

      mesh.indexCount = src.indexCount;
    }

    sc::log(sc::LogLevel::Info, "Meshes created: %zu", m_meshes.size());
    return true;
  }

  void VkRenderer::destroyMeshes()
  {
    for (GpuMesh& mesh : m_meshes)
    {
      if (mesh.vertexBuffer) vkDestroyBuffer(m_device, mesh.vertexBuffer, nullptr);
      if (mesh.vertexMemory) vkFreeMemory(m_device, mesh.vertexMemory, nullptr);
      if (mesh.indexBuffer) vkDestroyBuffer(m_device, mesh.indexBuffer, nullptr);
      if (mesh.indexMemory) vkFreeMemory(m_device, mesh.indexMemory, nullptr);
      mesh = GpuMesh{};
    }
    m_meshes.clear();
  }

  bool VkRenderer::createDebugDrawBuffers(size_t vertexCapacity)
  {
    destroyDebugDrawBuffers();

    m_debugVertexCapacity = vertexCapacity;
    if (m_debugVertexCapacity == 0)
      return true;

    const VkDeviceSize size = sizeof(DebugVertex) * m_debugVertexCapacity;
    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      if (!createBuffer(m_device, m_phys, size,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_debugVertexBuffers[i], m_debugVertexMemory[i]))
      {
        sc::log(sc::LogLevel::Error, "createBuffer (debug draw) failed.");
        return false;
      }

      if (vkMapMemory(m_device, m_debugVertexMemory[i], 0, size, 0, &m_debugVertexMapped[i]) != VK_SUCCESS)
      {
        sc::log(sc::LogLevel::Error, "vkMapMemory (debug draw) failed.");
        return false;
      }
    }
    return true;
  }

  void VkRenderer::destroyDebugDrawBuffers()
  {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
      if (m_debugVertexMapped[i])
      {
        vkUnmapMemory(m_device, m_debugVertexMemory[i]);
        m_debugVertexMapped[i] = nullptr;
      }
      if (m_debugVertexBuffers[i]) { vkDestroyBuffer(m_device, m_debugVertexBuffers[i], nullptr); m_debugVertexBuffers[i] = VK_NULL_HANDLE; }
      if (m_debugVertexMemory[i]) { vkFreeMemory(m_device, m_debugVertexMemory[i], nullptr); m_debugVertexMemory[i] = VK_NULL_HANDLE; }
    }
    m_debugVertexCapacity = 0;
  }

  void VkRenderer::onSDLEvent(const SDL_Event& e)
  {
    m_debugUI.processEvent(e);
  }

  void VkRenderer::setTelemetry(const JobsTelemetrySnapshot& jobs, const MemStats& mem)
  {
    m_jobsSnap = jobs;
    m_memSnap = mem;
  }

  void VkRenderer::setEcsStats(const EcsStatsSnapshot& ecs, const SchedulerStatsSnapshot& sched)
  {
    m_ecsSnap = ecs;
    m_schedSnap = sched;
  }

  void VkRenderer::setDebugWorld(World* world, Entity camera, Entity triangle, Entity root)
  {
    m_debugUI.setWorldContext(world, camera, triangle, root);
  }

  float VkRenderer::swapchainAspect() const
  {
    if (m_swapchainExtent.height == 0)
      return 1.0f;
    return (float)m_swapchainExtent.width / (float)m_swapchainExtent.height;
  }


  void VkRenderer::destroySwapchainObjects()
  {
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();

    for (auto v : m_depthViews) vkDestroyImageView(m_device, v, nullptr);
    m_depthViews.clear();
    for (auto img : m_depthImages) vkDestroyImage(m_device, img, nullptr);
    m_depthImages.clear();
    for (auto mem : m_depthMemory) vkFreeMemory(m_device, mem, nullptr);
    m_depthMemory.clear();

    for (auto v : m_swapchainViews) vkDestroyImageView(m_device, v, nullptr);
    m_swapchainViews.clear();
    m_swapchainImages.clear();
  }

  bool VkRenderer::recreateSwapchain()
  {
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(m_window, &w, &h);

    // minimizado o tamaño inválido: no recrear aún
    if (w == 0 || h == 0)
      return false;

    vkDeviceWaitIdle(m_device);

    destroySwapchainObjects();
    destroyPipeline();

    if (m_renderPass) { vkDestroyRenderPass(m_device, m_renderPass, nullptr); m_renderPass = VK_NULL_HANDLE; }

    if (m_swapchain) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }

    if (m_cmdPool) { vkDestroyCommandPool(m_device, m_cmdPool, nullptr); m_cmdPool = VK_NULL_HANDLE; }

    if (!createSwapchain()) return false;
    if (!createRenderPass()) return false;
    if (!createDepthResources()) return false;
    if (!m_debugUI.onSwapchainRecreated(m_renderPass, (uint32_t)m_swapchainImages.size())) return false;
    if (!createPipeline()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommands()) return false;

    m_swapchainDirty = false;
    return true;
  }



  bool VkRenderer::beginFrame()
  {

    static bool once = false;
    if (!once)
    {
      once = true;
      sc::log(sc::LogLevel::Info, "renderFrame=%p draws=%zu",
              (void*)m_renderFrame,
              (m_renderFrame ? m_renderFrame->draws.size() : 0));
    }


    // 0) Si resize/present marcó dirty, recrear primero (no tocar fences ni acquire)
    if (m_swapchainDirty)
    {
      if (!recreateSwapchain())
        return false;
      return false;
    }

    // 1) Espera el frame-in-flight actual
    vkWaitForFences(m_device, 1, &m_inFlight[m_frameIndex], VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_inFlight[m_frameIndex]);

    // 2) Acquire
    VkResult r = vkAcquireNextImageKHR(
      m_device, m_swapchain, UINT64_MAX,
      m_imageAvailable[m_frameIndex], VK_NULL_HANDLE,
      &m_imageIndex
    );

    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
      m_swapchainDirty = true;   // marca y recrea en el siguiente beginFrame
      return false;
    }

    if (r == VK_SUBOPTIMAL_KHR)
    {
      // sigue, pero recrea pronto
      m_swapchainDirty = true;
    }
    else if (r != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkAcquireNextImageKHR failed (%d)", (int)r);
      return false;
    }

    // 3) Record commands
    VkCommandBuffer cmd = m_cmdBuffers[m_imageIndex];
    vkResetCommandBuffer(cmd, 0);

    m_debugUI.setFrameStats(m_frameIndex, m_imageIndex, m_swapchainExtent);
    m_debugUI.setTelemetry(m_jobsSnap, m_memSnap);
    m_debugUI.setEcsStats(m_ecsSnap, m_schedSnap);
    m_debugUI.newFrame();

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear[2]{};
    clear[0].color.float32[0] = 0.02f;
    clear[0].color.float32[1] = 0.02f;
    clear[0].color.float32[2] = 0.05f;
    clear[0].color.float32[3] = 1.0f;
    clear[1].depthStencil.depth = 1.0f;
    clear[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass = m_renderPass;
    rpbi.framebuffer = m_framebuffers[m_imageIndex];
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = m_swapchainExtent;
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clear;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_swapchainExtent.width;
    viewport.height = (float)m_swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = m_swapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (m_renderFrame && m_cameraMapped[m_frameIndex])
    {
      CameraUbo ubo{};
      ubo.viewProj = m_renderFrame->viewProj;
      std::memcpy(m_cameraMapped[m_frameIndex], &ubo, sizeof(ubo));
    }

    if (!m_debugUI.isTrianglePaused() && m_renderFrame && !m_renderFrame->draws.empty())
    {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                              &m_globalSets[m_frameIndex], 0, nullptr);
      const GpuMesh* boundMesh = nullptr;
      for (const auto& draw : m_renderFrame->draws)
      {
        if (draw.meshId >= m_meshes.size())
          continue;
        const GpuMesh& mesh = m_meshes[draw.meshId];
        if (mesh.indexCount == 0)
          continue;

        if (&mesh != boundMesh)
        {
          VkDeviceSize offset = 0;
          vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, &offset);
          vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
          boundMesh = &mesh;
        }

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &draw.model);
        vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
      }
    }

    if (m_debugDraw && m_debugPipeline)
    {
      const auto& verts = m_debugDraw->vertices();
      const uint32_t vtxCount = (uint32_t)verts.size();
      if (vtxCount > 0)
      {
        if (vtxCount > m_debugVertexCapacity)
        {
          const size_t newCap = (size_t)vtxCount * 2;
          destroyDebugDrawBuffers();
          if (!createDebugDrawBuffers(newCap))
            sc::log(sc::LogLevel::Warn, "Debug draw buffer resize failed (capacity=%zu).", newCap);
        }
        if (m_debugVertexMapped[m_frameIndex])
        {
          const size_t copyBytes = sizeof(DebugVertex) * vtxCount;
          std::memcpy(m_debugVertexMapped[m_frameIndex], verts.data(), copyBytes);

          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipeline);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                  &m_globalSets[m_frameIndex], 0, nullptr);
          const Mat4 identity = Mat4::identity();
          vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &identity);

          VkDeviceSize offset = 0;
          vkCmdBindVertexBuffers(cmd, 0, 1, &m_debugVertexBuffers[m_frameIndex], &offset);
          vkCmdDraw(cmd, vtxCount, 1, 0, 0);
        }
      }
    }

    m_debugUI.draw(cmd);

    vkCmdEndRenderPass(cmd);

    vkEndCommandBuffer(cmd);
    return true;
  }


  void VkRenderer::endFrame()
  {
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBuffer cmd = m_cmdBuffers[m_imageIndex];

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &m_imageAvailable[m_frameIndex];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &m_renderFinished[m_frameIndex];

    VkResult sr = vkQueueSubmit(m_gfxQueue, 1, &si, m_inFlight[m_frameIndex]);
    if (sr != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkQueueSubmit failed (%d)", (int)sr);
      // si submit falla, lo más seguro es marcar swapchain dirty y seguir intentando en el próximo frame
      m_swapchainDirty = true;
    }

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_renderFinished[m_frameIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_imageIndex;

    VkResult pr = vkQueuePresentKHR(m_gfxQueue, &pi);

    // OUT_OF_DATE: resize o surface invalid -> recrear swapchain en el siguiente frame
    // SUBOPTIMAL: sigue funcionando pero mejor recrearlo (por resize o cambio de modo)
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
    {
      m_swapchainDirty = true;
    }
    else if (pr != VK_SUCCESS)
    {
      sc::log(sc::LogLevel::Error, "vkQueuePresentKHR failed (%d)", (int)pr);
      // errores raros -> marca dirty y luego decides si cerrar
      m_swapchainDirty = true;
    }

    // avanzar frame-in-flight
    m_frameIndex = (m_frameIndex + 1) % MAX_FRAMES;
  }

}
