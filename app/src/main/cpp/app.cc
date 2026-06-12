#include "app.hh"
#include "renderer.hh"
#include <android/asset_manager.h>
#include <android/log.h>
#include <array>
#include <set>
#include <string>
#include <vector>

std::vector<char> readFile(AAssetManager *mgr, const std::string &filename) {
  AAsset *asset = AAssetManager_open(mgr, filename.c_str(), AASSET_MODE_BUFFER);
  if (!asset) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Can not be possible to open the file");
    exit(1);
  }

  size_t size = AAsset_getLength(asset);
  std::vector<char> buffer(size);
  AAsset_read(asset, buffer.data(), size);
  AAsset_close(asset);
  return buffer;
}

uint32_t App::findMemoryType(uint32_t typeFilter,
                             VkMemoryPropertyFlags requiredFlags) {
  VkPhysicalDeviceMemoryProperties memProperties{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    bool compatible = typeFilter & (1 << i);
    bool hasFlags = (memProperties.memoryTypes[i].propertyFlags &
                     requiredFlags) == requiredFlags;
    if (compatible && hasFlags)
      return i;
  };

  __android_log_print(ANDROID_LOG_ERROR, "APP",
                      "Failed to find suitable memory type!");
  exit(1);
}

void App::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties, VkBuffer &buffer,
                       VkDeviceMemory &memory) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(logicalDevice, &bufferInfo, nullptr, &buffer) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create buffer!");
    exit(1);
  };

  VkMemoryRequirements memReqs{};
  vkGetBufferMemoryRequirements(logicalDevice, buffer, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex =
      findMemoryType(memReqs.memoryTypeBits, properties);

  if (vkAllocateMemory(logicalDevice, &allocInfo, nullptr, &memory) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to allocate buffer memory!");
    exit(1);
  };

  vkBindBufferMemory(logicalDevice, buffer, memory, 0);
}

void App::init(ANativeWindow *window, AAssetManager *mgr) {
  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "VulkanTits2";
  appInfo.applicationVersion = VK_MAKE_VERSION(20, 4, 2004);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                              VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
  createInfo.enabledExtensionCount = std::size(extensions);
  createInfo.ppEnabledExtensionNames = extensions;
  if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to create Vulkan instance!");
    exit(1);
  };

  VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
  surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  surfaceInfo.window = window; // the ANativeWindow*
  if (vkCreateAndroidSurfaceKHR(instance, &surfaceInfo, nullptr, &surface) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to create android surface!");
    exit(1);
  }

  uint32_t count{};
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "There is no device available to use Vulkan!");
    exit(1);
  };
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physicalDevice = device;
      break;
    };
    if (physicalDevice == VK_NULL_HANDLE) {
      physicalDevice = device;
    };
  };
  if (physicalDevice == VK_NULL_HANDLE) {
    __android_log_print(
        ANDROID_LOG_ERROR, "APP",
        "Kinda hard to message to pop up, because to be here you need to have "
        "a GPU compatible, but IDK, maybe is bussy!?");
    exit(1);
  };

  uint32_t queueCount{};
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount,
                                           nullptr);
  if (queueCount == 0) {
    __android_log_print(
        ANDROID_LOG_ERROR, "APP",
        "There is no queue family available on the current device");
    exit(1);
  };
  std::vector<VkQueueFamilyProperties> families(queueCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount,
                                           families.data());
  for (uint32_t i{}; i < queueCount; i++) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      graphicsFamily = i;
    };
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface,
                                         &supported);
    if (supported == VK_TRUE) {
      presentFamily = i;
    };
  };
  if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX) {
    __android_log_print(
        ANDROID_LOG_ERROR, "APP",
        "There is family nor surface supported to present the graphic!");
    exit(1);
  };
  float queuePriority = 1.0f;
  std::set<uint32_t> uniqueFamilies = {graphicsFamily, presentFamily};
  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

  for (uint32_t family : uniqueFamilies) {
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueInfo);
  };
  const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = std::size(uniqueFamilies);
  deviceInfo.pQueueCreateInfos = queueCreateInfos.data();
  deviceInfo.enabledExtensionCount = std::size(deviceExtensions);
  deviceInfo.ppEnabledExtensionNames = deviceExtensions;

  if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &logicalDevice) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "The device can't be create");
    exit(1);
  };

  // added after App::drawFrame()
  vkGetDeviceQueue(logicalDevice, graphicsFamily, 0, &graphicsQueue);
  vkGetDeviceQueue(logicalDevice, presentFamily, 0, &presentQueue);

  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

  uint32_t formatCount{};
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                       nullptr);
  if (formatCount == 0) {
    __android_log_print(
        ANDROID_LOG_ERROR, "APP",
        "Couldn't find an available surface on this physical device");
    exit(1);
  }
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                       formats.data());

  uint32_t presentModesCount{};
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                            &presentModesCount, nullptr);
  if (presentModesCount == 0) {
    __android_log_print(
        ANDROID_LOG_ERROR, "APP",
        "Couldn't find an available mode on this physical device");
    exit(1);
  }
  std::vector<VkPresentModeKHR> modes(presentModesCount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                            &presentModesCount, modes.data());

  VkSurfaceFormatKHR chosenFormat = formats[0]; // fallback
  for (VkSurfaceFormatKHR &f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosenFormat = f;
      break;
    };
  };
  swapchainFormat = chosenFormat.format;

  VkPresentModeKHR chosenMode =
      VK_PRESENT_MODE_FIFO_KHR; // fallback, the worst case
  for (VkPresentModeKHR m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
      chosenMode = m;
      break;
    }
  };

  VkExtent2D extent = caps.currentExtent;
  swapchainExtent = extent;

  VkSwapchainCreateInfoKHR swapInfo{};
  swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapInfo.surface = surface;
  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    imageCount = caps.maxImageCount;
  };
  swapInfo.minImageCount = imageCount;
  swapInfo.imageFormat = chosenFormat.format;
  swapInfo.imageColorSpace = chosenFormat.colorSpace;
  swapInfo.imageExtent = extent;
  swapInfo.imageArrayLayers = 1;
  swapInfo.imageUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  swapInfo.preTransform = caps.currentTransform;
  swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapInfo.presentMode = chosenMode;
  swapInfo.clipped = VK_TRUE;

  if (graphicsFamily != presentFamily) {
    swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swapInfo.queueFamilyIndexCount = 2;
    uint32_t indices[] = {graphicsFamily, presentFamily};
    swapInfo.pQueueFamilyIndices = indices;
  } else {
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  };

  if (vkCreateSwapchainKHR(logicalDevice, &swapInfo, nullptr, &swapchain) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to create swapchain!");
    exit(1);
  };

  uint32_t imagesCount{};
  vkGetSwapchainImagesKHR(logicalDevice, swapchain, &imagesCount, nullptr);
  if (imagesCount == 0) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Couldn't load any image to the swapchain");
    exit(1);
  };
  swapchainImages.resize(imagesCount);
  vkGetSwapchainImagesKHR(logicalDevice, swapchain, &imagesCount,
                          swapchainImages.data());

  // --- Create the Command Pool ---
  VkCommandPoolCreateInfo cmdPoolInfo{};
  cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT allows you to reset
  // individual command buffers
  cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cmdPoolInfo.queueFamilyIndex = graphicsFamily;

  if (vkCreateCommandPool(logicalDevice, &cmdPoolInfo, nullptr, &commandPool) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to create command pool!");
    exit(1);
  };

  renderer.init(logicalDevice, physicalDevice, mgr, swapchainExtent.width,
                swapchainExtent.height);

  VkCommandBufferAllocateInfo transAlloc{};
  transAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  transAlloc.commandPool = commandPool;
  transAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  transAlloc.commandBufferCount = 1;

  VkCommandBuffer transCmd;

  vkAllocateCommandBuffers(logicalDevice, &transAlloc, &transCmd);

  VkCommandBufferBeginInfo transBegin{};
  transBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  transBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(transCmd, &transBegin);

  renderer.transitionOutputImageInitial(transCmd);

  vkEndCommandBuffer(transCmd);

  VkSubmitInfo transSubmit{};
  transSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  transSubmit.commandBufferCount = 1;
  transSubmit.pCommandBuffers = &transCmd;

  vkQueueSubmit(graphicsQueue, 1, &transSubmit, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue);
  vkFreeCommandBuffers(logicalDevice, commandPool, 1, &transCmd);

  swapchainImageViews.resize(swapchainImages.size());
  for (uint32_t i{}; i < swapchainImages.size(); i++) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = swapchainImages[i];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = swapchainFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(logicalDevice, &viewInfo, nullptr,
                          &swapchainImageViews[i]) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP",
                          "Couldn't load any image to the swapchain");
      exit(1);
    };
  };

  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = swapchainFormat;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = subpass.colorAttachmentCount;
  renderPassInfo.pAttachments = &colorAttachment;
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;

  if (vkCreateRenderPass(logicalDevice, &renderPassInfo, nullptr,
                         &renderPass) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Couldn't create a render pass");
    exit(1);
  };

  framebuffers.resize(swapchainImageViews.size());

  for (uint32_t i{}; i < swapchainImageViews.size(); i++) {
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &swapchainImageViews[i];
    fbInfo.width = swapchainExtent.width;
    fbInfo.height = swapchainExtent.height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(logicalDevice, &fbInfo, nullptr,
                            &framebuffers[i]) != VK_SUCCESS) {
      __android_log_print(
          ANDROID_LOG_ERROR, "APP",
          "Couldn't create a framebuffer in this swapchain views");
      exit(1);
    };
  };

  // ----- Calculator UI state (uploads curves on dirty frames in drawFrame) -----
  calc.init(swapchainExtent.width, swapchainExtent.height);

  // ----- Composite descriptor set (samples renderer.outputImage in fragment) -----
  {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(logicalDevice, &layoutInfo, nullptr,
                                    &compositeSetLayout) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP",
                          "Failed to create composite descriptor set layout");
      exit(1);
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 1;
    if (vkCreateDescriptorPool(logicalDevice, &poolInfo, nullptr,
                               &compositePool) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP",
                          "Failed to create composite descriptor pool");
      exit(1);
    }

    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool     = compositePool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts        = &compositeSetLayout;
    if (vkAllocateDescriptorSets(logicalDevice, &setAlloc, &compositeSet) !=
        VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP",
                          "Failed to allocate composite descriptor set");
      exit(1);
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = renderer.outputSampler;
    imgInfo.imageView   = renderer.outputImageView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = compositeSet;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(logicalDevice, 1, &write, 0, nullptr);
  }

  // ----- Composite graphics pipeline (fullscreen triangle, samples outputImage) -----
  {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &compositeSetLayout;
    if (vkCreatePipelineLayout(logicalDevice, &layoutInfo, nullptr,
                               &pipelineLayout) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP",
                          "Failed to create composite pipeline layout");
      exit(1);
    }

    auto loadModule = [&](const char* path) -> VkShaderModule {
      std::vector<char> code = readFile(mgr, path);
      VkShaderModuleCreateInfo smInfo{};
      smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      smInfo.codeSize = code.size();
      smInfo.pCode    = reinterpret_cast<const uint32_t*>(code.data());
      VkShaderModule m = VK_NULL_HANDLE;
      if (vkCreateShaderModule(logicalDevice, &smInfo, nullptr, &m) !=
          VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "APP",
                            "Failed to create shader module: %s", path);
        exit(1);
      }
      return m;
    };
    VkShaderModule vertModule = loadModule("shaders/composite_vert.spv");
    VkShaderModule fragModule = loadModule("shaders/composite_frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                     VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT |
                                     VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamic;
    pipelineInfo.layout              = pipelineLayout;
    pipelineInfo.renderPass          = renderPass;
    pipelineInfo.subpass             = 0;

    if (vkCreateGraphicsPipelines(logicalDevice, VK_NULL_HANDLE, 1, &pipelineInfo,
                                  nullptr, &graphicsPipeline) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP",
                          "Failed to create composite graphics pipeline");
      exit(1);
    }

    vkDestroyShaderModule(logicalDevice, vertModule, nullptr);
    vkDestroyShaderModule(logicalDevice, fragModule, nullptr);
  }

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  if (vkAllocateCommandBuffers(logicalDevice, &allocInfo, &commandBuffer) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to allocate command buffer!");
    exit(1);
  };

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  if (vkCreateSemaphore(logicalDevice, &semInfo, nullptr,
                        &imageAvailableSemaphore) != VK_SUCCESS) {
    __android_log_print(
        ANDROID_LOG_ERROR, "APP",
        "Failed to create semaphore that awaits for an available image");
    exit(1);
  };
  if (vkCreateSemaphore(logicalDevice, &semInfo, nullptr,
                        &renderFinishedSemaphore) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to create semaphore, renderFinishedSemaphore");
    exit(1);
  };
  if (vkCreateFence(logicalDevice, &fenceInfo, nullptr, &inFlightFence) !=
      VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to create the fence, inFlightFence");
    exit(1);
  };

} //    End App::init

void App::drawFrame() {
  // Wait for previous frame
  vkWaitForFences(logicalDevice, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
  vkResetFences(logicalDevice, 1, &inFlightFence);

  // Acquire next image
  uint32_t imageIndex;
  vkAcquireNextImageKHR(logicalDevice, swapchain, UINT64_MAX,
                        imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

  lastImageIndex = imageIndex;

  // Reset and record command buffer
  vkResetCommandBuffer(commandBuffer, 0);

  // Re-emit UI curves only when the calculator state changed.
  if (calc.dirty) {
    calc.rebuildCurves(scratchCurves);
    renderer.uploadCurves(scratchCurves.data(),
                          (uint32_t)(scratchCurves.size() /
                                     Renderer::CURVE_FLOATS));
    calc.dirty = false;
  }
  {
    static int once = 0;
    if (once < 3) {
      once++;
      uint32_t n = (uint32_t)(scratchCurves.size() / Renderer::CURVE_FLOATS);
      float bx0 = n > 0 ? scratchCurves[10] : -1.0f;
      float by0 = n > 0 ? scratchCurves[11] : -1.0f;
      float bx1 = n > 0 ? scratchCurves[12] : -1.0f;
      float by1 = n > 0 ? scratchCurves[13] : -1.0f;
      __android_log_print(ANDROID_LOG_DEBUG, "DBG",
          "frame %d: extent=%ux%u curves=%u curve0_bbox=(%.1f,%.1f,%.1f,%.1f)",
          once, swapchainExtent.width, swapchainExtent.height, n,
          bx0, by0, bx1, by1);
    }
  }

  // Begin command buffer
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(commandBuffer, &beginInfo);

  // Tiling + coverage compute passes; output ends up in renderer.outputImage (GENERAL).
  renderer.dispatch(commandBuffer);

  // Transition outputImage GENERAL -> SHADER_READ_ONLY_OPTIMAL so the composite
  // fragment shader can sample it. Synchronizes coverage write -> fragment read.
  {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    b.image               = renderer.outputImage;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
  }
  // Begin render pass
  VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  VkRenderPassBeginInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  renderPassInfo.renderPass = renderPass;
  renderPassInfo.framebuffer = framebuffers[imageIndex];
  renderPassInfo.renderArea.offset = {0, 0};
  renderPassInfo.renderArea.extent = swapchainExtent;
  renderPassInfo.clearValueCount = 1;
  renderPassInfo.pClearValues = &clearColor;

  vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                       VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{0.0f, 0.0f,
                (float)swapchainExtent.width, (float)swapchainExtent.height,
                0.0f, 1.0f};
  VkRect2D   sc{{0, 0}, swapchainExtent};
  vkCmdSetViewport(commandBuffer, 0, 1, &vp);
  vkCmdSetScissor (commandBuffer, 0, 1, &sc);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    graphicsPipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout, 0, 1, &compositeSet, 0, nullptr);
  vkCmdDraw(commandBuffer, 3, 1, 0, 0);

  vkCmdEndRenderPass(commandBuffer);

  // Transition outputImage back to GENERAL for next frame's coverage write.
  {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    b.image               = renderer.outputImage;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
  }

  vkEndCommandBuffer(commandBuffer);

  // Submit
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphore};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  VkSemaphore signalSemaphores[] = {renderFinishedSemaphore};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFence);

  // Present
  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = signalSemaphores;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &swapchain;
  presentInfo.pImageIndices = &imageIndex;

  vkQueuePresentKHR(presentQueue, &presentInfo);

} //     End App::drawFrame

void App::onTouch(float px, float py) {
  calc.onTouch(px, py);
  if (calc.dirty) dirty = true;
}

void App::saveScreenshot(AAssetManager *mgr) { // saveScreenshot
  VkImageCreateInfo imgInfo{};
  imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
  imgInfo.extent = {swapchainExtent.width, swapchainExtent.height, 1};
  imgInfo.mipLevels = 1;
  imgInfo.arrayLayers = 1;
  imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling = VK_IMAGE_TILING_LINEAR;
  imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage dstImage;
  vkCreateImage(logicalDevice, &imgInfo, nullptr, &dstImage);

  VkMemoryRequirements memReqs{};
  vkGetImageMemoryRequirements(logicalDevice, dstImage, &memReqs);

  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

  uint32_t memIndex = UINT32_MAX;
  VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i{}; i < memProps.memoryTypeCount; i++) {
    bool compatible = (memReqs.memoryTypeBits & (1 << i)) != 0;
    bool hasFlags =
        (memProps.memoryTypes[i].propertyFlags & required) == required;
    if (compatible && hasFlags) {
      memIndex = i;
      break;
    };
  };

  VkDeviceMemory dstMemory;
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = memIndex;
  vkAllocateMemory(logicalDevice, &allocInfo, nullptr, &dstMemory);
  vkBindImageMemory(logicalDevice, dstImage, dstMemory, 0);

  VkCommandBufferAllocateInfo cmdAlloc{};
  cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAlloc.commandPool = commandPool;
  cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAlloc.commandBufferCount = 1;

  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(logicalDevice, &cmdAlloc, &cmd);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = dstImage;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
  VkImageMemoryBarrier srcBarrier{};
  srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  srcBarrier.image =
      swapchainImages[lastImageIndex]; // capture first swapchain image
  srcBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &srcBarrier);
  VkImageCopy copyRegion{};
  copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copyRegion.extent = {swapchainExtent.width, swapchainExtent.height, 1};

  vkCmdCopyImage(cmd, swapchainImages[lastImageIndex],
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);

  vkEndCommandBuffer(cmd);

  // Submit and wait for completion
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;

  vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue);

  // ── Step 3: Map memory and read pixels ────────────────────────────────────
  VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout subLayout{};
  vkGetImageSubresourceLayout(logicalDevice, dstImage, &subResource,
                              &subLayout);

  const uint8_t *data;
  vkMapMemory(logicalDevice, dstMemory, 0, VK_WHOLE_SIZE, 0, (void **)&data);
  data += subLayout.offset;

  // ── Step 4: Write PPM file ────────────────────────────────────────────────
  // No library needed, no compression, perfect quality for 8-bit per channel
  const char *path = "/data/data/io.nava.calculator/triangle.ppm";
  FILE *f = fopen(path, "wb");
  if (f) {
    // PPM header
    fprintf(f, "P6\n%d %d\n255\n", swapchainExtent.width,
            swapchainExtent.height);

    // Write pixels row by row
    // Swapchain is BGRA, PPM expects RGB — swap B and R channels
    for (uint32_t y = 0; y < swapchainExtent.height; y++) {
      const uint8_t *row = data + y * subLayout.rowPitch;
      for (uint32_t x = 0; x < swapchainExtent.width; x++) {
        uint8_t b = row[x * 4 + 0];
        uint8_t g = row[x * 4 + 1];
        uint8_t r = row[x * 4 + 2];
        fwrite(&r, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&b, 1, 1, f);
      }
    }
    fclose(f);
    __android_log_print(ANDROID_LOG_DEBUG, "APP", "Screenshot saved to %s",
                        path);
  } else {
    __android_log_print(ANDROID_LOG_ERROR, "APP",
                        "Failed to open file for screenshot");
  }

  // ── Cleanup ───────────────────────────────────────────────────────────────
  vkUnmapMemory(logicalDevice, dstMemory);
  vkFreeCommandBuffers(logicalDevice, commandPool, 1, &cmd);
  vkDestroyImage(logicalDevice, dstImage, nullptr);
  vkFreeMemory(logicalDevice, dstMemory, nullptr);
} //  End App::saveScreenshot

void App::cleanup() {
  vkDestroyPipeline(logicalDevice, graphicsPipeline, nullptr);
  vkDestroyPipelineLayout(logicalDevice, pipelineLayout, nullptr);
  vkDestroyDescriptorPool(logicalDevice, compositePool, nullptr);
  vkDestroyDescriptorSetLayout(logicalDevice, compositeSetLayout, nullptr);
  for (VkFramebuffer &fb : framebuffers) {
    vkDestroyFramebuffer(logicalDevice, fb, nullptr);
  };
  vkDestroyRenderPass(logicalDevice, renderPass, nullptr);
  for (VkImageView &view : swapchainImageViews) {
    vkDestroyImageView(logicalDevice, view, nullptr);
  };
  vkDestroySemaphore(logicalDevice, imageAvailableSemaphore, nullptr);
  vkDestroySemaphore(logicalDevice, renderFinishedSemaphore, nullptr);
  vkDestroyFence(logicalDevice, inFlightFence, nullptr);
  vkDestroyCommandPool(logicalDevice, commandPool, nullptr);
  vkDestroySwapchainKHR(logicalDevice, swapchain, nullptr);
  renderer.cleanup();
  vkDestroyDevice(logicalDevice, nullptr);
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
}
//    End App::cleanup
