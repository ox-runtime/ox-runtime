#pragma once

#ifdef __APPLE__
#ifdef __OBJC__

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef void* id;
typedef struct objc_object* MTLDevice_id;
typedef struct objc_object* MTLCommandQueue_id;
typedef struct objc_object* MTLTexture_id;

namespace ox {
namespace client {
namespace metal {

// Map OpenXR format (as int64_t) to MTLPixelFormat
static MTLPixelFormat MapFormatToMetal(int64_t format) {
    switch (format) {
        case 80:  // MTLPixelFormatRGBA8Unorm
            return MTLPixelFormatRGBA8Unorm;
        case 81:  // MTLPixelFormatRGBA8Unorm_sRGB
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case 70:  // MTLPixelFormatBGRA8Unorm
            return MTLPixelFormatBGRA8Unorm;
        case 71:  // MTLPixelFormatBGRA8Unorm_sRGB
            return MTLPixelFormatBGRA8Unorm_sRGB;
        default:
            spdlog::error("Unsupported Metal format: {}", format);
            return MTLPixelFormatInvalid;
    }
}

bool CreateTextures(void* metalCommandQueue, uint32_t width, uint32_t height, int64_t format, uint32_t numImages,
                    void** outTextures) {
    if (!metalCommandQueue || !outTextures || numImages == 0) {
        spdlog::error("CreateTextures: Invalid parameters");
        return false;
    }

    id<MTLCommandQueue> commandQueue = (__bridge id<MTLCommandQueue>)metalCommandQueue;
    id<MTLDevice> device = commandQueue.device;
    if (!device) {
        spdlog::error("CreateTextures: Invalid Metal command queue");
        return false;
    }

    MTLPixelFormat mtlFormat = MapFormatToMetal(format);

    spdlog::info("Creating {} Metal textures: {}x{} format={}", numImages, width, height, static_cast<int>(mtlFormat));

    // Create texture descriptor
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtlFormat width:width height:height mipmapped:NO];

    if (!descriptor) {
        spdlog::error("CreateTextures: Failed to create texture descriptor");
        return false;
    }

    // Configure descriptor with version-appropriate API
    if (@available(macOS 10.12, iOS 10.0, *)) {
        // Metal 1.2+: Use modern storageMode API
        descriptor.storageMode = MTLStorageModePrivate;
        spdlog::debug("Using Metal 1.2+ storageMode API");
    } else {
        // Metal 1.0/1.1: Use legacy resourceOptions
        descriptor.resourceOptions = MTLResourceStorageModePrivate;
        spdlog::debug("Using Metal 1.0 resourceOptions API");
    }

    if (@available(macOS 10.11.4, iOS 9.0, *)) {
        // Metal 1.1+: Set texture usage hints
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        spdlog::debug("Set texture usage flags (Metal 1.1+)");
    } else {
        // Metal 1.0: Usage flags don't exist, skip them
        spdlog::debug("Skipping usage flags (Metal 1.0)");
    }

    // Create textures (same for all Metal versions)
    for (uint32_t i = 0; i < numImages; i++) {
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];

        if (!texture) {
            spdlog::error("CreateTextures: Failed to create texture {}", i);

            // Clean up previously created textures
            for (uint32_t j = 0; j < i; j++) {
                if (outTextures[j]) {
                    CFRelease(outTextures[j]);
                    outTextures[j] = nullptr;
                }
            }
            return false;
        }

        // Retain the texture and store as void*
        outTextures[i] = (void*)CFBridgingRetain(texture);

        spdlog::debug("Created Metal texture {} successfully", i);
    }

    spdlog::info("Successfully created {} Metal textures", numImages);
    return true;
}

void DestroyTextures(void** textures, uint32_t numTextures) {
    if (!textures) {
        return;
    }

    spdlog::debug("Releasing {} Metal textures", numTextures);

    for (uint32_t i = 0; i < numTextures; i++) {
        if (textures[i]) {
            // Release the retained texture
            CFRelease(textures[i]);
            textures[i] = nullptr;
        }
    }

    spdlog::info("Released {} Metal textures", numTextures);
}

std::vector<int64_t> GetSupportedFormats() {
    // Report only sRGB formats so applications apply sRGB color management.
    return {
        static_cast<int64_t>(MTLPixelFormatRGBA8Unorm_sRGB),
        static_cast<int64_t>(MTLPixelFormatBGRA8Unorm_sRGB),
    };
}

void* GetDefaultDevice() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return (__bridge void*)device;
}

bool CopyTextureToMemory(void* commandQueue, void* texture, uint32_t width, uint32_t height, void* dest,
                         size_t destSize) {
    if (!commandQueue || !texture || !dest) {
        spdlog::error("CopyTextureToMemory: Invalid parameters");
        return false;
    }

    id<MTLCommandQueue> mtlCommandQueue = (__bridge id<MTLCommandQueue>)commandQueue;
    id<MTLTexture> mtlTexture = (__bridge id<MTLTexture>)texture;
    id<MTLDevice> device = mtlCommandQueue.device;
    if (!device) {
        spdlog::error("CopyTextureToMemory: Invalid Metal command queue");
        return false;
    }

    // Verify we have enough space (RGBA8)
    size_t requiredSize = width * height * 4;
    if (destSize < requiredSize) {
        spdlog::error("CopyTextureToMemory: Destination buffer too small");
        return false;
    }

    // Get bytes per row (must be aligned to 256 bytes for Metal)
    NSUInteger bytesPerRow = width * 4;
    NSUInteger alignedBytesPerRow = (bytesPerRow + 255) & ~255;  // Align to 256 bytes

    id<MTLCommandBuffer> commandBuffer = [mtlCommandQueue commandBuffer];
    if (!commandBuffer) {
        spdlog::error("CopyTextureToMemory: Failed to create command buffer");
        return false;
    }

    const NSUInteger stagingSize = alignedBytesPerRow * height;
    id<MTLBuffer> stagingBuffer = [device newBufferWithLength:stagingSize options:MTLResourceStorageModeShared];
    if (!stagingBuffer) {
        spdlog::error("CopyTextureToMemory: Failed to create staging buffer");
        return false;
    }

    id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
    if (!blitEncoder) {
        spdlog::error("CopyTextureToMemory: Failed to create blit encoder");
        return false;
    }

    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    const MTLOrigin origin = region.origin;
    const MTLSize size = MTLSizeMake(region.size.width, region.size.height, 1);
    [blitEncoder copyFromTexture:mtlTexture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:origin
                      sourceSize:size
                        toBuffer:stagingBuffer
               destinationOffset:0
          destinationBytesPerRow:alignedBytesPerRow
        destinationBytesPerImage:stagingSize];
    [blitEncoder endEncoding];

    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    if (@available(macOS 10.13, iOS 11.0, *)) {
        if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
            spdlog::error("CopyTextureToMemory: Blit command buffer failed with status {}",
                          static_cast<int>(commandBuffer.status));
            return false;
        }
    }

    const uint8_t* sourceBytes = static_cast<const uint8_t*>(stagingBuffer.contents);
    if (!sourceBytes) {
        spdlog::error("CopyTextureToMemory: Staging buffer has no contents");
        return false;
    }

    // If alignment matches, copy directly
    if (alignedBytesPerRow == bytesPerRow) {
        memcpy(dest, sourceBytes, requiredSize);
    } else {
        // Copy to destination, removing padding
        uint8_t* dst = static_cast<uint8_t*>(dest);
        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst + y * bytesPerRow, sourceBytes + y * alignedBytesPerRow, bytesPerRow);
        }
    }

    return true;
}

// Detect Metal graphics binding from session create info
bool DetectGraphicsBinding(const void* next, void** outBinding) {
    while (next) {
        const XrBaseInStructure* header = reinterpret_cast<const XrBaseInStructure*>(next);
        if (header->type == XR_TYPE_GRAPHICS_BINDING_METAL_KHR) {
            const XrGraphicsBindingMetalKHR* metalBinding = reinterpret_cast<const XrGraphicsBindingMetalKHR*>(header);
            if (outBinding) {
                *outBinding = metalBinding->commandQueue;
            }
            spdlog::debug("DetectGraphicsBinding: Metal graphics binding - commandQueue={}",
                          reinterpret_cast<uintptr_t>(metalBinding->commandQueue));
            return true;
        }
        next = header->next;
    }
    return false;
}

// Populate swapchain images for Metal
void PopulateSwapchainImages(const std::vector<void*>& metalTextures, uint32_t numImages, XrStructureType imageType,
                             XrSwapchainImageBaseHeader* images) {
    for (uint32_t i = 0; i < numImages; ++i) {
        XrSwapchainImageMetalKHR* metalImages = reinterpret_cast<XrSwapchainImageMetalKHR*>(images);
        metalImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
        metalImages[i].next = nullptr;
        metalImages[i].texture = (i < metalTextures.size()) ? metalTextures[i] : nullptr;
    }
}

void* GetMetalDefaultDevice() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return (__bridge void*)device;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetMetalGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                                                 XrGraphicsRequirementsMetalKHR* graphicsRequirements) {
    spdlog::debug("xrGetMetalGraphicsRequirementsKHR called");
    if (!graphicsRequirements) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
    graphicsRequirements->next = nullptr;
    graphicsRequirements->metalDevice = GetMetalDefaultDevice();
    return XR_SUCCESS;
}

}  // namespace metal
}  // namespace client
}  // namespace ox

extern "C" void* GetMetalDefaultDevice() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return (__bridge void*)device;
}

#endif  // __OBJC__
#endif  // __APPLE__
