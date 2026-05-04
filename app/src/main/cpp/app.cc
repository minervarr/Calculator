#include <array>
#include <set>
#include <vector>
#include "app.hh"
#include <android/log.h>
#include <android/asset_manager.h>
#include <string>

std::vector<char> readFile(AAssetManager* mgr, const std::string& filename) {
  AAsset* asset = AAssetManager_open(mgr, filename.c_str(), AASSET_MODE_BUFFER);
  if(!asset) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Can not be possible to open the file");
    exit(1);
  }

  size_t size = AAsset_getLength(asset);
  std::vector<char> buffer(size);
  AAsset_read(asset, buffer.data(), size);
  AAsset_close(asset);
  return buffer;
}

void App::init(ANativeWindow* window, AAssetManager* mgr)
{
  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "VulkanTits2";
  appInfo.applicationVersion = VK_MAKE_VERSION(20, 4, 2004);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  const char* extensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
  };
  createInfo.enabledExtensionCount = std::size(extensions);
  createInfo.ppEnabledExtensionNames = extensions;
  if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create Vulkan instance!");
    exit(1);
  };

  VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
  surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  surfaceInfo.window = window; // the ANativeWindow*
  if (vkCreateAndroidSurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS){
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create android surface!");
    exit(1);
  }

  uint32_t count{};
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0){
    __android_log_print(ANDROID_LOG_ERROR, "APP", "There is no device available to use Vulkan!");
    exit(1);
  };
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  for (VkPhysicalDevice device : devices){
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){
      physicalDevice = device;
      break;
    };
    if (physicalDevice == VK_NULL_HANDLE){
      physicalDevice = device;
    };
  };
  if (physicalDevice == VK_NULL_HANDLE){
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Kinda hard to message to pop up, because to be here you need to have a GPU compatible, but IDK, maybe is bussy!?");
    exit(1);
  };

  uint32_t queueCount{};
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
  if (queueCount == 0){
    __android_log_print(ANDROID_LOG_ERROR, "APP", "There is no queue family available on the current device");
    exit(1);
  };
  std::vector<VkQueueFamilyProperties> families(queueCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, families.data());
  for (uint32_t i{}; i < queueCount; i++){
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
      graphicsFamily = i;
    };
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(
        physicalDevice,
        i,
        surface,
        &supported
    );
    if (supported == VK_TRUE){
      presentFamily = i;
    };
  };
  if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX){
    __android_log_print(ANDROID_LOG_ERROR, "APP", "There is family nor surface supported to present the graphic!");
    exit(1);
  };
  float queuePriority = 1.0f;
  std::set<uint32_t> uniqueFamilies = {graphicsFamily, presentFamily};
  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

  for (uint32_t family: uniqueFamilies) {
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueInfo);
  };
  const char* deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
  };

  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = std::size(uniqueFamilies);
  deviceInfo.pQueueCreateInfos = queueCreateInfos.data();
  deviceInfo.enabledExtensionCount = std::size(deviceExtensions);
  deviceInfo.ppEnabledExtensionNames = deviceExtensions;

  if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &logicalDevice) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "The device can't be create");
    exit(1);
  };

  // added after App::drawFrame()
  vkGetDeviceQueue(logicalDevice, graphicsFamily, 0, &graphicsQueue);
  vkGetDeviceQueue(logicalDevice, presentFamily, 0, &presentQueue);

  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

  uint32_t formatCount{};
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
  if (formatCount == 0) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't find an available surface on this physical device");
    exit(1);
  }
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
  
  uint32_t presentModesCount{};
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModesCount, nullptr);
  if (presentModesCount == 0) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't find an available mode on this physical device");
    exit(1);
  }
  std::vector<VkPresentModeKHR> modes(presentModesCount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModesCount, modes.data());
  
  VkSurfaceFormatKHR chosenFormat = formats[0]; //fallback
  for (VkSurfaceFormatKHR& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosenFormat = f;
      break;
    };
  };
  swapchainFormat = chosenFormat.format;

  VkPresentModeKHR chosenMode = VK_PRESENT_MODE_FIFO_KHR; // fallback, the worst case
  for (VkPresentModeKHR m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
      chosenMode = m;
      break;
    }
  };
  
  VkExtent2D extent = caps.currentExtent;
  swapchainExtent = extent;

  VkSwapchainCreateInfoKHR swapInfo{};
  swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR; swapInfo.surface = surface;
  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    imageCount = caps.maxImageCount;
  };
  swapInfo.minImageCount = imageCount;
  swapInfo.imageFormat = chosenFormat.format;
  swapInfo.imageColorSpace = chosenFormat.colorSpace;
  swapInfo.imageExtent = extent;
  swapInfo.imageArrayLayers = 1;
  swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
  
  if (vkCreateSwapchainKHR(logicalDevice, &swapInfo, nullptr, &swapchain) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create swapchain!");
    exit(1);
  };
  
  uint32_t imagesCount{};
  vkGetSwapchainImagesKHR(logicalDevice, swapchain, &imagesCount, nullptr);
  if (imagesCount == 0) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't load any image to the swapchain");
    exit(1);
  };
  swapchainImages.resize(imagesCount);
  vkGetSwapchainImagesKHR(logicalDevice, swapchain, &imagesCount, swapchainImages.data());
  
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
    if (vkCreateImageView(logicalDevice, &viewInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't load any image to the swapchain");
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
  
  if (vkCreateRenderPass(logicalDevice, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't create a render pass");
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
    if (vkCreateFramebuffer(logicalDevice, &fbInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't create a framebuffer in this swapchain views");
      exit(1);
    };
  };

  std::vector<char> vertCode = readFile(mgr, "shaders/triangle.vert.spv");
  std::vector<char> fragCode = readFile(mgr, "shaders/triangle.frag.spv");

  VkShaderModuleCreateInfo vertInfo{};
  vertInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  vertInfo.codeSize = vertCode.size();
  vertInfo.pCode = reinterpret_cast<const uint32_t*>(vertCode.data());
  
  VkShaderModule vertModule;
  if (vkCreateShaderModule(logicalDevice, &vertInfo, nullptr, &vertModule) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't create a vertex shader module");
    exit(1);
  };

  VkShaderModuleCreateInfo fragInfo{};
  fragInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  fragInfo.codeSize = fragCode.size();
  fragInfo.pCode = reinterpret_cast<const uint32_t*>(fragCode.data());
  
  VkShaderModule fragModule;
  if (vkCreateShaderModule(logicalDevice, &fragInfo, nullptr, &fragModule) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Couldn't create a vertex shader module");
    exit(1);
  };

  VkPipelineShaderStageCreateInfo vertStage{};
  vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertStage.module = vertModule;
  vertStage.pName = "main"; //  the main function on the triangle.vert file;
  
  VkPipelineShaderStageCreateInfo fragStage{};
  fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragStage.module = fragModule;
  fragStage.pName = "main"; //  the same name on the triangle .frag file;
  
  VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 0;
  vertexInput.vertexAttributeDescriptionCount = 0;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)swapchainExtent.width;
  viewport.height = (float)swapchainExtent.height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapchainExtent;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT  | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;
  
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 0;
  pipelineLayoutInfo.pushConstantRangeCount = 0;
  if (vkCreatePipelineLayout(logicalDevice, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create pipeline layout! Causa!");
    exit(1);
  };
  
  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.layout = pipelineLayout;
  pipelineInfo.renderPass = renderPass;
  pipelineInfo.subpass = 0;
  if (vkCreateGraphicsPipelines(logicalDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "failed to create graphics pipeline!");
    exit(1);
  };

  vkDestroyShaderModule(logicalDevice, vertModule, nullptr);
  vkDestroyShaderModule(logicalDevice, fragModule, nullptr);

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = graphicsFamily;
  if (vkCreateCommandPool(logicalDevice, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "failed to create graphics pipeline!");
    exit(1);
  };

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  
  if (vkAllocateCommandBuffers(logicalDevice, &allocInfo, &commandBuffer) != VK_SUCCESS){
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to allocate command buffer!");
    exit(1);
  };

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  
  if(vkCreateSemaphore(logicalDevice, &semInfo, nullptr, &imageAvailableSemaphore) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create semaphore that awaits for an available image");
    exit(1);
  };
  if(vkCreateSemaphore(logicalDevice, &semInfo, nullptr, &renderFinishedSemaphore) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create semaphore, renderFinishedSemaphore");
    exit(1);
  };
  if(vkCreateFence(logicalDevice, &fenceInfo, nullptr, &inFlightFence) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, "APP", "Failed to create the fence, inFlightFence");
    exit(1);
  };

} //    End App::init

void App::drawFrame() {
  // Wait for previous frame
  vkWaitForFences(logicalDevice, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
  vkResetFences(logicalDevice, 1, &inFlightFence);
  
  // Acquire next image
  uint32_t imageIndex;
  vkAcquireNextImageKHR(logicalDevice, swapchain, UINT64_MAX, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

  // Reset and record command buffer
  vkResetCommandBuffer(commandBuffer, 0);

  // Begin command buffer
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(commandBuffer, &beginInfo);

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
  
  vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
  vkCmdDraw(commandBuffer, 3, 1, 0 ,0);
  vkCmdEndRenderPass(commandBuffer);
  vkEndCommandBuffer(commandBuffer);

  // Submit
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphore};
  VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

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
}//     End App::drawFrame

void App::cleanup()
{
  vkDestroyPipeline(logicalDevice, graphicsPipeline, nullptr);
  vkDestroyPipelineLayout(logicalDevice, pipelineLayout, nullptr);
  for (VkFramebuffer& fb : framebuffers) {
    vkDestroyFramebuffer(logicalDevice, fb, nullptr);
  };
  vkDestroyRenderPass(logicalDevice, renderPass, nullptr);
  for (VkImageView& view : swapchainImageViews) {
    vkDestroyImageView(logicalDevice, view, nullptr);
  };
  vkDestroySemaphore(logicalDevice, imageAvailableSemaphore, nullptr);
  vkDestroySemaphore(logicalDevice, renderFinishedSemaphore, nullptr);
  vkDestroyFence(logicalDevice, inFlightFence, nullptr);
  vkDestroyCommandPool(logicalDevice, commandPool, nullptr);
  vkDestroySwapchainKHR(logicalDevice, swapchain, nullptr);
  vkDestroyDevice(logicalDevice, nullptr);
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
}
//    End App::cleanup
