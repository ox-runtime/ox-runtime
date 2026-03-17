#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <openxr/openxr.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common.h"

#ifdef OX_METAL
#define XR_USE_GRAPHICS_API_METAL
#define XR_KHR_metal_enable 1
#include <Metal/Metal.h>
#include <openxr/openxr_platform.h>
#endif

using namespace ox::test;

#ifdef OX_METAL
// ============================================================================
// Metal Graphics Tests
// ============================================================================

TEST_F(RuntimeTestBase, MetalGraphics_GetRequirements_ReturnsSuccess) {
    // Create instance with Metal extension enabled
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create_info.applicationInfo.applicationName, "TestApp", XR_MAX_APPLICATION_NAME_SIZE);
    create_info.applicationInfo.applicationVersion = 1;
    std::strncpy(create_info.applicationInfo.engineName, "TestEngine", XR_MAX_ENGINE_NAME_SIZE);
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    const char* enabled_extensions[] = {"XR_KHR_metal_enable"};
    create_info.enabledExtensionCount = 1;
    create_info.enabledExtensionNames = enabled_extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(&create_info, &instance);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(instance, XR_NULL_HANDLE);
    created_instances_.push_back(instance);

    // Get the Metal graphics requirements function
    PFN_xrGetMetalGraphicsRequirementsKHR pfnGetMetalGraphicsRequirementsKHR = nullptr;
    result = xrGetInstanceProcAddr(instance, "xrGetMetalGraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetMetalGraphicsRequirementsKHR));
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(pfnGetMetalGraphicsRequirementsKHR, nullptr);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &system_info, &system_id);
    ASSERT_EQ(result, XR_SUCCESS);

    XrGraphicsRequirementsMetalKHR metal_reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
    result = pfnGetMetalGraphicsRequirementsKHR(instance, system_id, &metal_reqs);

    EXPECT_EQ(result, XR_SUCCESS);
    // Metal device should be returned
    EXPECT_NE(metal_reqs.metalDevice, nullptr);
}

TEST_F(RuntimeTestBase, MetalGraphics_CreateSessionWithMetalBinding_ReturnsSuccess) {
    // Create Metal device
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_NE(device, nil);

    // Create Metal command queue
    id<MTLCommandQueue> commandQueue = [device newCommandQueue];
    ASSERT_NE(commandQueue, nil);

    // Create instance with Metal extension enabled
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create_info.applicationInfo.applicationName, "TestApp", XR_MAX_APPLICATION_NAME_SIZE);
    create_info.applicationInfo.applicationVersion = 1;
    std::strncpy(create_info.applicationInfo.engineName, "TestEngine", XR_MAX_ENGINE_NAME_SIZE);
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    const char* enabled_extensions[] = {"XR_KHR_metal_enable"};
    create_info.enabledExtensionCount = 1;
    create_info.enabledExtensionNames = enabled_extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(&create_info, &instance);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(instance, XR_NULL_HANDLE);
    created_instances_.push_back(instance);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &system_info, &system_id);
    ASSERT_EQ(result, XR_SUCCESS);

    // Create session with Metal binding
    XrGraphicsBindingMetalKHR metal_binding{XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    metal_binding.commandQueue = (__bridge void*)commandQueue;

    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &metal_binding;
    session_info.systemId = system_id;

    XrSession session = XR_NULL_HANDLE;
    result = xrCreateSession(instance, &session_info, &session);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_NE(session, XR_NULL_HANDLE);

    if (session != XR_NULL_HANDLE) {
        xrDestroySession(session);
    }
}

TEST_F(RuntimeTestBase, MetalGraphics_CreateSwapchainAndVerifyTextures_ReturnsSuccess) {
    // Create Metal device
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_NE(device, nil);

    // Create Metal command queue
    id<MTLCommandQueue> commandQueue = [device newCommandQueue];
    ASSERT_NE(commandQueue, nil);

    // Create instance with Metal extension enabled
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create_info.applicationInfo.applicationName, "TestApp", XR_MAX_APPLICATION_NAME_SIZE);
    create_info.applicationInfo.applicationVersion = 1;
    std::strncpy(create_info.applicationInfo.engineName, "TestEngine", XR_MAX_ENGINE_NAME_SIZE);
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    const char* enabled_extensions[] = {"XR_KHR_metal_enable"};
    create_info.enabledExtensionCount = 1;
    create_info.enabledExtensionNames = enabled_extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(&create_info, &instance);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(instance, XR_NULL_HANDLE);
    created_instances_.push_back(instance);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &system_info, &system_id);
    ASSERT_EQ(result, XR_SUCCESS);

    // Create session with Metal binding
    XrGraphicsBindingMetalKHR metal_binding{XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
    metal_binding.commandQueue = (__bridge void*)commandQueue;

    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &metal_binding;
    session_info.systemId = system_id;

    XrSession session = XR_NULL_HANDLE;
    result = xrCreateSession(instance, &session_info, &session);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(session, XR_NULL_HANDLE);

    // Create swapchain
    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.width = 1024;
    swapchain_info.height = 1024;
    swapchain_info.format = 81;  // MTLPixelFormatRGBA8Unorm_sRGB
    swapchain_info.mipCount = 1;
    swapchain_info.arraySize = 1;
    swapchain_info.sampleCount = 1;

    XrSwapchain swapchain = XR_NULL_HANDLE;
    result = xrCreateSwapchain(session, &swapchain_info, &swapchain);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(swapchain, XR_NULL_HANDLE);

    // Enumerate images
    uint32_t image_count = 0;
    result = xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_GT(image_count, 0u);

    std::vector<XrSwapchainImageMetalKHR> images(image_count);
    for (auto& img : images) {
        img.type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
    }

    result =
        xrEnumerateSwapchainImages(swapchain, image_count, &image_count, (XrSwapchainImageBaseHeader*)images.data());
    ASSERT_EQ(result, XR_SUCCESS);

    // Verify textures were created and have correct properties
    for (const auto& img : images) {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)img.texture;
        ASSERT_NE(texture, nil);
        EXPECT_EQ(texture.width, 1024u);
        EXPECT_EQ(texture.height, 1024u);
        EXPECT_EQ(texture.pixelFormat, MTLPixelFormatRGBA8Unorm_sRGB);
        EXPECT_EQ(texture.mipmapLevelCount, 1u);
        EXPECT_EQ(texture.arrayLength, 1u);
        EXPECT_EQ(texture.sampleCount, 1u);
    }

    // Cleanup
    xrDestroySwapchain(swapchain);
    xrDestroySession(session);
}

#endif  // OX_METAL