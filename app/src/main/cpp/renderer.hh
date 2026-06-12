#pragma once
#include <vulkan/vulkan.h>
#include <android/asset_manager.h>
#include <cstdint>

struct Renderer {
  // Layout constants shared with shaders. Must stay in sync with
  // tiling.slang and coverage.slang offset math.
  static constexpr uint32_t MAX_CURVES          = 512;
  static constexpr uint32_t CURVE_FLOATS        = 16;
  static constexpr uint32_t TILE_SIZE           = 16;
  static constexpr uint32_t MAX_CURVES_PER_TILE = 16;
  static constexpr uint32_t TILE_STRIDE_U32     = MAX_CURVES_PER_TILE + 1;

  // Device + assets
  VkDevice         device         = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  AAssetManager*   assetManager   = nullptr;

  // Dimensions + curve count (push-constant inputs)
  uint32_t screenWidth  = 0;
  uint32_t screenHeight = 0;
  uint32_t curveCount   = 0;

  // Compute-side descriptors
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool      descriptorPool      = VK_NULL_HANDLE;
  VkDescriptorSet       descriptorSet       = VK_NULL_HANDLE;

  // Buffers
  VkBuffer       curveBuffer       = VK_NULL_HANDLE;
  VkDeviceMemory curveBufferMemory = VK_NULL_HANDLE;
  void*          curveBufferMapped = nullptr;
  VkBuffer       tileBuffer        = VK_NULL_HANDLE;
  VkDeviceMemory tileBufferMemory  = VK_NULL_HANDLE;

  // Output image (sampled by composite pass in app.cc)
  VkImage        outputImage       = VK_NULL_HANDLE;
  VkDeviceMemory outputImageMemory = VK_NULL_HANDLE;
  VkImageView    outputImageView   = VK_NULL_HANDLE;
  VkSampler      outputSampler     = VK_NULL_HANDLE;

  // Pipelines (single shared layout for both compute pipelines)
  VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
  VkPipeline       tilingPipeline        = VK_NULL_HANDLE;
  VkPipeline       coveragePipeline      = VK_NULL_HANDLE;

  // Public API
  void init(VkDevice dev, VkPhysicalDevice phyDev, AAssetManager* mgr,
            uint32_t width, uint32_t height);
  void cleanup();

  void uploadCurves(const float* curveData, uint32_t count);

  void transitionOutputImageInitial(VkCommandBuffer cmd);
  void dispatch(VkCommandBuffer cmd);

 private:
  void createDescriptorLayoutAndPool();
  void createBuffers();
  void createOutputImage();
  void createPipelines();
  void writeDescriptors();

  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);
  VkShaderModule loadShader(const char* path);
};
