#pragma once
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <vector>
#include "renderer.hh"
#include "ui.hh"

class App {
  private:
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t presentFamily = UINT32_MAX;
    VkDevice logicalDevice = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    Renderer renderer;
    Calculator calc;
    std::vector<float> scratchCurves;
    VkDescriptorSetLayout compositeSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      compositePool      = VK_NULL_HANDLE;
    VkDescriptorSet       compositeSet       = VK_NULL_HANDLE;
    uint32_t lastImageIndex = 0;
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags requiredFlags);
    void createBuffer(   VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
    void saveScreenshot(AAssetManager* mgr);
  public:
    void init(ANativeWindow* window, AAssetManager* mgr);
    void cleanup();
    void drawFrame();
    void onTouch(float px, float py);
    bool initialized = false;
    bool dirty = false;
};
