#pragma once

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ox {
namespace client {
namespace vulkan {

struct VulkanGraphicsBinding {
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkInstance instance;
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;
};

struct VulkanSwapchainData {
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkQueue queue;
    VkCommandPool commandPool;
};

void CreateImages(std::vector<VkImage>& vkImages, std::vector<VkDeviceMemory>& vkImageMemory, VkDevice vkDevice,
                  VkPhysicalDevice vkPhysicalDevice, uint32_t width, uint32_t height, int64_t format,
                  uint32_t numImages) {
    if (vkImages.empty() && vkDevice != VK_NULL_HANDLE && vkPhysicalDevice != VK_NULL_HANDLE) {
        vkImages.resize(numImages);
        vkImageMemory.resize(numImages);

        // Create actual Vulkan images
        for (uint32_t i = 0; i < numImages; i++) {
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = static_cast<VkFormat>(format);
            imageInfo.extent.width = width;
            imageInfo.extent.height = height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkResult result = vkCreateImage(vkDevice, &imageInfo, nullptr, &vkImages[i]);
            if (result != VK_SUCCESS) {
                spdlog::error("Failed to create Vulkan image: {}", static_cast<int>(result));
                vkImages[i] = VK_NULL_HANDLE;
                continue;
            }

            // Allocate memory for the image
            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(vkDevice, vkImages[i], &memRequirements);

            // Find suitable memory type
            VkPhysicalDeviceMemoryProperties memProperties;
            vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProperties);

            uint32_t memoryTypeIndex = UINT32_MAX;
            VkMemoryPropertyFlags requiredProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            for (uint32_t j = 0; j < memProperties.memoryTypeCount; j++) {
                if ((memRequirements.memoryTypeBits & (1 << j)) &&
                    (memProperties.memoryTypes[j].propertyFlags & requiredProperties) == requiredProperties) {
                    memoryTypeIndex = j;
                    break;
                }
            }

            if (memoryTypeIndex == UINT32_MAX) {
                spdlog::error("Failed to find suitable memory type for Vulkan image");
                vkDestroyImage(vkDevice, vkImages[i], nullptr);
                vkImages[i] = VK_NULL_HANDLE;
                continue;
            }

            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            result = vkAllocateMemory(vkDevice, &allocInfo, nullptr, &vkImageMemory[i]);
            if (result != VK_SUCCESS) {
                spdlog::error("Failed to allocate Vulkan image memory: {}", static_cast<int>(result));
                vkDestroyImage(vkDevice, vkImages[i], nullptr);
                vkImages[i] = VK_NULL_HANDLE;
                vkImageMemory[i] = VK_NULL_HANDLE;
                continue;
            }

            result = vkBindImageMemory(vkDevice, vkImages[i], vkImageMemory[i], 0);
            if (result != VK_SUCCESS) {
                spdlog::error("Failed to bind Vulkan image memory: {}", static_cast<int>(result));
                vkFreeMemory(vkDevice, vkImageMemory[i], nullptr);
                vkDestroyImage(vkDevice, vkImages[i], nullptr);
                vkImages[i] = VK_NULL_HANDLE;
                vkImageMemory[i] = VK_NULL_HANDLE;
                continue;
            }

            spdlog::debug("Created Vulkan image {} successfully", i);
        }
    } else if (vkDevice == VK_NULL_HANDLE || vkPhysicalDevice == VK_NULL_HANDLE) {
        spdlog::error("No Vulkan device found for session - cannot create swapchain images");
        // Fill with null handles
        vkImages.resize(numImages, VK_NULL_HANDLE);
        vkImageMemory.resize(numImages, VK_NULL_HANDLE);
    }
}

void DestroyImages(std::vector<VkImage>& vkImages, std::vector<VkDeviceMemory>& vkImageMemory,
                   VkCommandPool vkCommandPool, VkDevice vkDevice) {
    if (vkDevice != VK_NULL_HANDLE) {
        // Destroy command pool
        if (vkCommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vkDevice, vkCommandPool, nullptr);
        }

        // Destroy images and memory
        for (size_t i = 0; i < vkImages.size(); i++) {
            if (vkImages[i] != VK_NULL_HANDLE) {
                vkDestroyImage(vkDevice, vkImages[i], nullptr);
            }
            if (i < vkImageMemory.size() && vkImageMemory[i] != VK_NULL_HANDLE) {
                vkFreeMemory(vkDevice, vkImageMemory[i], nullptr);
            }
        }
    }
    vkImages.clear();
    vkImageMemory.clear();
}

bool CopyImageToMemory(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool,
                       VkImage image, uint32_t width, uint32_t height, VkFormat format, std::byte* dest,
                       size_t destSize) {
    size_t requiredSize = width * height * 4;
    if (destSize < requiredSize) {
        spdlog::error("Destination buffer too small for texture data");
        return false;
    }

    // Create staging buffer
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = requiredSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuffer;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        spdlog::error("Failed to create Vulkan staging buffer");
        return false;
    }

    // Allocate host-visible memory
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memRequirements);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    uint32_t memoryTypeIndex = UINT32_MAX;
    VkMemoryPropertyFlags requiredProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & requiredProps) == requiredProps) {
            memoryTypeIndex = i;
            break;
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        spdlog::error("Failed to find suitable Vulkan memory type");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory stagingMemory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        spdlog::error("Failed to allocate Vulkan staging memory");
        return false;
    }

    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    // Create command buffer for copy operation
    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    if (vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuffer) != VK_SUCCESS) {
        vkFreeMemory(device, stagingMemory, nullptr);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        spdlog::error("Failed to allocate Vulkan command buffer");
        return false;
    }

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmdBuffer, &beginInfo);

    // Transition image to TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    // Copy image to buffer
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyImageToBuffer(cmdBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

    // Transition back to COLOR_ATTACHMENT_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmdBuffer);

    // Submit and wait
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // Copy data from staging buffer to destination
    void* data;
    if (vkMapMemory(device, stagingMemory, 0, requiredSize, 0, &data) == VK_SUCCESS) {
        std::memcpy(dest, data, requiredSize);
        vkUnmapMemory(device, stagingMemory);
    }

    // Cleanup
    vkFreeCommandBuffers(device, commandPool, 1, &cmdBuffer);
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);

    return true;
}

std::vector<int64_t> GetSupportedFormats() {
    // Report only sRGB formats so applications apply sRGB color management.
    return {
        static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB),
        static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB),
    };
}

// Initialize swapchain-specific Vulkan objects
bool InitializeSwapchainData(void* bindingData, VulkanSwapchainData& outData) {
    VulkanGraphicsBinding* binding = static_cast<VulkanGraphicsBinding*>(bindingData);
    if (!binding) {
        return false;
    }

    outData.device = binding->device;
    outData.physicalDevice = binding->physicalDevice;

    // Get the queue
    vkGetDeviceQueue(outData.device, binding->queueFamilyIndex, binding->queueIndex, &outData.queue);

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = binding->queueFamilyIndex;

    if (vkCreateCommandPool(outData.device, &poolInfo, nullptr, &outData.commandPool) != VK_SUCCESS) {
        spdlog::error("Failed to create Vulkan command pool for swapchain");
        return false;
    }

    return true;
}

// Detect Vulkan graphics binding from session create info
bool DetectGraphicsBinding(const void* next, void** outBinding) {
    while (next) {
        const XrBaseInStructure* header = reinterpret_cast<const XrBaseInStructure*>(next);
        if (header->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            const XrGraphicsBindingVulkanKHR* binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(header);
            if (outBinding) {
                VulkanGraphicsBinding* vkBinding = new VulkanGraphicsBinding();
                vkBinding->instance = binding->instance;
                vkBinding->physicalDevice = binding->physicalDevice;
                vkBinding->device = binding->device;
                vkBinding->queueFamilyIndex = binding->queueFamilyIndex;
                vkBinding->queueIndex = binding->queueIndex;
                *outBinding = vkBinding;
            }
            spdlog::debug("DetectGraphicsBinding: Vulkan graphics binding detected");
            return true;
        }
        next = header->next;
    }
    return false;
}

// Populate swapchain images for Vulkan
void PopulateSwapchainImages(const std::vector<VkImage>& vkImages, uint32_t numImages, XrStructureType imageType,
                             XrSwapchainImageBaseHeader* images) {
    for (uint32_t i = 0; i < numImages; ++i) {
        XrSwapchainImageVulkanKHR* vkImagesOut = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        vkImagesOut[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        vkImagesOut[i].next = nullptr;
        vkImagesOut[i].image = (i < vkImages.size()) ? vkImages[i] : VK_NULL_HANDLE;
    }
}

// Helper: Select best Vulkan physical device
static VkPhysicalDevice SelectBestVulkanPhysicalDevice(VkInstance vkInstance) {
    uint32_t deviceCount = 0;
    VkResult result = vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        return VK_NULL_HANDLE;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());
    if (result != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    // Select the best physical device (prefer discrete GPU)
    VkPhysicalDevice selectedDevice = devices[0];
    VkPhysicalDeviceProperties selectedProps = {};
    vkGetPhysicalDeviceProperties(selectedDevice, &selectedProps);

    for (uint32_t i = 1; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            selectedProps.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selectedDevice = devices[i];
            selectedProps = props;
        }
    }

    return selectedDevice;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanGraphicsRequirementsKHR(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* graphicsRequirements) {
    spdlog::debug("xrGetVulkanGraphicsRequirementsKHR called");
    if (!graphicsRequirements) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanGraphicsRequirements2KHR(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* graphicsRequirements) {
    spdlog::debug("xrGetVulkanGraphicsRequirements2KHR called");
    return xrGetVulkanGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId,
                                                                uint32_t bufferCapacityInput,
                                                                uint32_t* bufferCountOutput, char* buffer) {
    spdlog::debug("xrGetVulkanInstanceExtensionsKHR called");
    const char* extensions = "VK_KHR_surface";
    uint32_t len = static_cast<uint32_t>(strlen(extensions)) + 1;
    if (bufferCountOutput) {
        *bufferCountOutput = len;
    }
    if (bufferCapacityInput >= len && buffer) {
        snprintf(buffer, bufferCapacityInput, "%s", extensions);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId,
                                                              uint32_t bufferCapacityInput, uint32_t* bufferCountOutput,
                                                              char* buffer) {
    spdlog::debug("xrGetVulkanDeviceExtensionsKHR called");
    const char* extensions = "VK_KHR_swapchain";
    uint32_t len = static_cast<uint32_t>(strlen(extensions)) + 1;
    if (bufferCountOutput) {
        *bufferCountOutput = len;
    }
    if (bufferCapacityInput >= len && buffer) {
        snprintf(buffer, bufferCapacityInput, "%s", extensions);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId,
                                                            VkInstance vkInstance, VkPhysicalDevice* vkPhysicalDevice) {
    spdlog::debug("xrGetVulkanGraphicsDeviceKHR called");
    if (!vkPhysicalDevice) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!vkInstance) {
        spdlog::error("xrGetVulkanGraphicsDeviceKHR: Vulkan instance is NULL");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    VkPhysicalDevice selectedDevice = SelectBestVulkanPhysicalDevice(vkInstance);
    if (selectedDevice == VK_NULL_HANDLE) {
        spdlog::error("xrGetVulkanGraphicsDeviceKHR: Failed to select physical device");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(selectedDevice, &props);
    *vkPhysicalDevice = selectedDevice;
    spdlog::info("xrGetVulkanGraphicsDeviceKHR: Selected device: {}", props.deviceName);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanGraphicsDevice2KHR(XrInstance instance,
                                                             const XrVulkanGraphicsDeviceGetInfoKHR* getInfo,
                                                             VkPhysicalDevice* vkPhysicalDevice) {
    spdlog::debug("xrGetVulkanGraphicsDevice2KHR called");
    if (!getInfo || !vkPhysicalDevice) {
        spdlog::error("xrGetVulkanGraphicsDevice2KHR: Invalid parameters");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!getInfo->vulkanInstance) {
        spdlog::error("xrGetVulkanGraphicsDevice2KHR: Vulkan instance is NULL");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    VkPhysicalDevice selectedDevice = SelectBestVulkanPhysicalDevice(getInfo->vulkanInstance);
    if (selectedDevice == VK_NULL_HANDLE) {
        spdlog::error("xrGetVulkanGraphicsDevice2KHR: Failed to select physical device");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(selectedDevice, &props);
    *vkPhysicalDevice = selectedDevice;
    spdlog::info("xrGetVulkanGraphicsDevice2KHR: Selected device: {}", props.deviceName);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateVulkanInstanceKHR(XrInstance instance,
                                                         const XrVulkanInstanceCreateInfoKHR* createInfo,
                                                         VkInstance* vkInstance, VkResult* vkResult) {
    spdlog::debug("xrCreateVulkanInstanceKHR called");
    if (!createInfo || !vkInstance || !vkResult) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    PFN_vkCreateInstance vkCreateInstanceFunc =
        (PFN_vkCreateInstance)createInfo->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!vkCreateInstanceFunc) {
        spdlog::error("xrCreateVulkanInstanceKHR: Failed to get vkCreateInstance function");
        *vkResult = VK_ERROR_INITIALIZATION_FAILED;
        return XR_ERROR_RUNTIME_FAILURE;
    }

    const VkInstanceCreateInfo* vkCreateInfo = static_cast<const VkInstanceCreateInfo*>(createInfo->vulkanCreateInfo);
    const VkAllocationCallbacks* vkAllocator = static_cast<const VkAllocationCallbacks*>(createInfo->vulkanAllocator);

    *vkResult = vkCreateInstanceFunc(vkCreateInfo, vkAllocator, vkInstance);
    if (*vkResult != VK_SUCCESS) {
        spdlog::error("xrCreateVulkanInstanceKHR: vkCreateInstance failed with result {}", static_cast<int>(*vkResult));
        return XR_ERROR_RUNTIME_FAILURE;
    }

    spdlog::info("xrCreateVulkanInstanceKHR: Successfully created Vulkan instance");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateVulkanDeviceKHR(XrInstance instance,
                                                       const XrVulkanDeviceCreateInfoKHR* createInfo,
                                                       VkDevice* vkDevice, VkResult* vkResult) {
    spdlog::debug("xrCreateVulkanDeviceKHR called");
    if (!createInfo || !vkDevice || !vkResult) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    PFN_vkCreateDevice vkCreateDeviceFunc =
        (PFN_vkCreateDevice)createInfo->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    if (!vkCreateDeviceFunc) {
        vkCreateDeviceFunc = ::vkCreateDevice;
    }
    if (!vkCreateDeviceFunc) {
        spdlog::error("xrCreateVulkanDeviceKHR: Failed to get vkCreateDevice function");
        *vkResult = VK_ERROR_INITIALIZATION_FAILED;
        return XR_ERROR_RUNTIME_FAILURE;
    }

    const VkDeviceCreateInfo* vkCreateInfo = static_cast<const VkDeviceCreateInfo*>(createInfo->vulkanCreateInfo);
    const VkAllocationCallbacks* vkAllocator = static_cast<const VkAllocationCallbacks*>(createInfo->vulkanAllocator);

    *vkResult = vkCreateDeviceFunc(createInfo->vulkanPhysicalDevice, vkCreateInfo, vkAllocator, vkDevice);
    if (*vkResult != VK_SUCCESS) {
        spdlog::error("xrCreateVulkanDeviceKHR: vkCreateDevice failed with result {}", static_cast<int>(*vkResult));
        return XR_ERROR_RUNTIME_FAILURE;
    }

    spdlog::info("xrCreateVulkanDeviceKHR: Successfully created Vulkan device");
    return XR_SUCCESS;
}

}  // namespace vulkan
}  // namespace client
}  // namespace ox
