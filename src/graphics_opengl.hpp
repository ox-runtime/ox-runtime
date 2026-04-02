#pragma once

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// GL_SRGB8_ALPHA8 is not defined in the base Windows GL/gl.h
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

namespace ox {
namespace client {
namespace opengl {

void CreateTextures(std::vector<uint32_t>& glTextureIds, uint32_t width, uint32_t height, uint32_t numImages) {
    if (glTextureIds.empty()) {
        glTextureIds.resize(numImages);
        glGenTextures(numImages, glTextureIds.data());

        // Initialize each texture with minimal settings
        for (uint32_t i = 0; i < numImages; i++) {
            glBindTexture(GL_TEXTURE_2D, glTextureIds[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void DestroyTextures(std::vector<uint32_t>& glTextureIds) {
    if (!glTextureIds.empty()) {
        glDeleteTextures(static_cast<GLsizei>(glTextureIds.size()), glTextureIds.data());
        glTextureIds.clear();
    }
}

bool CopyTextureToMemory(uint32_t textureId, uint32_t width, uint32_t height, std::byte* dest, size_t destSize) {
    // Verify we have enough space (RGBA8)
    size_t requiredSize = width * height * 4;
    if (destSize < requiredSize) {
        spdlog::error("Destination buffer too small for texture data");
        return false;
    }

    // Clear any previous errors
    while (glGetError() != GL_NO_ERROR);

    // Bind the texture and read pixels directly as RGBA
    glBindTexture(GL_TEXTURE_2D, textureId);

    GLenum bindError = glGetError();
    if (bindError != GL_NO_ERROR) {
        spdlog::error("OpenGL error binding texture {}: {}", textureId, static_cast<unsigned int>(bindError));
        return false;
    }

    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, dest);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Check for GL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        spdlog::error("OpenGL error reading texture {}: {}", textureId, static_cast<unsigned int>(error));
        return false;
    }

    return true;
}

void NormalizeFramePixels(std::byte* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height <= 1) {
        return;
    }

    const size_t row_bytes = static_cast<size_t>(width) * 4;
    std::vector<std::byte> scratch(row_bytes);
    for (uint32_t top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
        std::byte* top_row = pixels + static_cast<size_t>(top) * row_bytes;
        std::byte* bottom_row = pixels + static_cast<size_t>(bottom) * row_bytes;
        std::memcpy(scratch.data(), top_row, row_bytes);
        std::memcpy(top_row, bottom_row, row_bytes);
        std::memcpy(bottom_row, scratch.data(), row_bytes);
    }
}

std::vector<int64_t> GetSupportedFormats() {
    // Report only sRGB formats so applications apply sRGB color management.
    return {
        static_cast<int64_t>(GL_SRGB8_ALPHA8),
    };
}

// Detect OpenGL graphics binding from session create info
bool DetectGraphicsBinding(const void* next, void** outBinding) {
    while (next) {
        const XrBaseInStructure* header = reinterpret_cast<const XrBaseInStructure*>(next);
        if (header->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR ||
            header->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR ||
            header->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR ||
            header->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR) {
            if (outBinding) {
                *outBinding = nullptr;  // OpenGL doesn't need binding data
            }
            spdlog::debug("DetectGraphicsBinding: OpenGL graphics binding detected");
            return true;
        }
        next = header->next;
    }
    return false;
}

// Populate swapchain images for OpenGL
void PopulateSwapchainImages(const std::vector<uint32_t>& glTextureIds, uint32_t numImages, XrStructureType imageType,
                             XrSwapchainImageBaseHeader* images) {
    for (uint32_t i = 0; i < numImages; ++i) {
        XrSwapchainImageOpenGLKHR* glImages = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images);
        glImages[i].type = imageType;
        glImages[i].next = nullptr;
        glImages[i].image = (i < glTextureIds.size()) ? glTextureIds[i] : 0;
    }
}

// OpenXR extension function
XRAPI_ATTR XrResult XRAPI_CALL xrGetOpenGLGraphicsRequirementsKHR(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLKHR* graphicsRequirements) {
    spdlog::debug("xrGetOpenGLGraphicsRequirementsKHR called");
    if (!graphicsRequirements) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
    graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
    return XR_SUCCESS;
}

}  // namespace opengl
}  // namespace client
}  // namespace ox
