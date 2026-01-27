#include "sc_vk.h"
#include "sc_log.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vector>
#include <cstring>
#include <algorithm>

namespace sc
{
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

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
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

  bool VkRenderer::createFramebuffers()
  {
    m_framebuffers.resize(m_swapchainViews.size());
    for (size_t i = 0; i < m_swapchainViews.size(); ++i)
    {
      VkImageView attachments[] = { m_swapchainViews[i] };

      VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
      fci.renderPass = m_renderPass;
      fci.attachmentCount = 1;
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


  void VkRenderer::destroySwapchainObjects()
  {
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();

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
    if (m_swapchain) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }

    if (m_renderPass) { vkDestroyRenderPass(m_device, m_renderPass, nullptr); m_renderPass = VK_NULL_HANDLE; }

    if (m_cmdPool) { vkDestroyCommandPool(m_device, m_cmdPool, nullptr); m_cmdPool = VK_NULL_HANDLE; }

    if (!createSwapchain()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommands()) return false;

    m_swapchainDirty = false;
    return true;
  }



  bool VkRenderer::beginFrame()
  {
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

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{};
    clear.color.float32[0] = 0.02f;
    clear.color.float32[1] = 0.02f;
    clear.color.float32[2] = 0.05f;
    clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass = m_renderPass;
    rpbi.framebuffer = m_framebuffers[m_imageIndex];
    rpbi.renderArea.offset = { 0, 0 };
    rpbi.renderArea.extent = m_swapchainExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
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
