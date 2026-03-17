// Include platform headers
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <unknwn.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Include Vulkan headers before OpenXR if Vulkan support is enabled
#ifdef OX_OPENGL
#ifdef _WIN32
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#define GL_SILENCE_DEPRECATION
#else
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#endif

#ifdef OX_VULKAN
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#endif

#ifdef OX_METAL
#ifdef __APPLE__
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define XR_USE_GRAPHICS_API_METAL
#define XR_KHR_metal_enable 1
#endif
#endif
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#ifdef OX_OPENGL
#include "graphics_opengl.hpp"
#endif  // OX_OPENGL

#ifdef OX_VULKAN
#include "graphics_vulkan.hpp"
#endif  // OX_VULKAN

#ifdef OX_METAL
#include "graphics_metal.hpp"
#endif  // OX_METAL

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "driver_connection.h"

using namespace ox::runtime;

namespace opengl = ox::client::opengl;
namespace vulkan = ox::client::vulkan;
#ifdef OX_METAL
namespace metal = ox::client::metal;
#endif

// Conditional defines for static builds (disable export attributes)
// Note: XRAPI_ATTR and XRAPI_CALL are already defined by OpenXR headers
#ifdef OX_BUILD_STATIC
#ifndef XRAPI_ATTR
#define XRAPI_ATTR
#endif
#ifndef XRAPI_CALL
#define XRAPI_CALL
#endif
#endif

// Graphics API enumeration
enum class GraphicsAPI { OpenGL, Vulkan, Metal };

// Export macro for Windows DLL
#ifdef _WIN32
#define RUNTIME_EXPORT __declspec(dllexport)
#else
#define RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

// Global driver connection - can be overridden by tests
#ifdef OX_BUILD_STATIC
static IDriverConnection* g_driver = nullptr;
#else
static IDriverConnection* g_driver = &DriverConnection::Instance();
#endif

// For testing: Allow injection of a mock driver connection
// Note: This must be called before creating any OpenXR instances
extern "C" {
RUNTIME_EXPORT void oxSetDriver(IDriverConnection* driver) { g_driver = driver; }
}

// Instance handle management
static std::unordered_map<XrInstance, bool> g_instances;
static std::unordered_map<XrSession, XrInstance> g_sessions;
static std::unordered_map<XrSpace, XrSession> g_spaces;

// Session graphics binding data
struct SessionGraphicsBinding {
    void* bindingData = nullptr;  // Opaque pointer to API-specific binding data
    GraphicsAPI graphicsAPI = GraphicsAPI::OpenGL;
};
static std::unordered_map<XrSession, SessionGraphicsBinding> g_session_graphics;

// Swapchain image data
struct SwapchainData {
    std::vector<uint32_t> glTextureIds;  // OpenGL texture IDs
#ifdef OX_VULKAN
    std::vector<VkImage> vkImages;              // Vulkan images
    std::vector<VkDeviceMemory> vkImageMemory;  // Vulkan image memory
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    VkCommandPool vkCommandPool = VK_NULL_HANDLE;
#endif
#ifdef OX_METAL
    std::vector<void*> metalTextures;   // Metal textures (id<MTLTexture> as opaque pointers)
    void* metalCommandQueue = nullptr;  // Metal command queue (id<MTLCommandQueue> as opaque pointer)
#endif
    uint32_t width;
    uint32_t height;
    int64_t format;
    GraphicsAPI graphicsAPI;  // Track which graphics API this swapchain uses
};
static std::unordered_map<XrSwapchain, SwapchainData> g_swapchains;
static std::array<std::vector<std::byte>, 2> g_submit_buffers;

// Action space metadata
struct ActionSpaceData {
    XrAction action;
    XrPath subaction_path;
};
static std::unordered_map<XrSpace, ActionSpaceData> g_action_spaces;

// Reference space metadata
struct ReferenceSpaceData {
    XrReferenceSpaceType type;
    XrPosef pose_in_reference_space;
};
static std::unordered_map<XrSpace, ReferenceSpaceData> g_reference_spaces;

// Action metadata
struct ActionData {
    XrActionType type;
    XrActionSet action_set;
    std::string name;
    std::vector<XrPath> subaction_paths;
};
static std::unordered_map<XrAction, ActionData> g_actions;

// Path tracking - bidirectional mapping between paths and strings
static std::unordered_map<XrPath, std::string> g_path_to_string;
static std::unordered_map<std::string, XrPath> g_string_to_path;

// Device path mapping (user path -> device index in shared memory)
static std::unordered_map<std::string, int> g_device_path_to_index;
static bool g_device_map_built = false;

// Action binding metadata - maps action to its bindings
struct ActionBinding {
    XrPath binding_path;           // The full binding path (e.g., /user/hand/left/input/trigger/value)
    XrPath subaction_path;         // Which hand (left/right) or XR_NULL_PATH for no subaction
    std::vector<XrPath> profiles;  // List of profiles that use this binding
};
// Map from action handle to list of its bindings
static std::unordered_map<XrAction, std::vector<ActionBinding>> g_action_bindings;

// Interaction profile tracking
static XrPath g_current_interaction_profile = XR_NULL_PATH;
static std::vector<std::string> g_suggested_profiles;

static std::mutex g_instance_mutex;

// Safe string copy helper - modern C++17+ replacement for strncpy
inline void safe_copy_string(char* dest, size_t dest_size, std::string_view src) {
    if (dest_size == 0) return;
    const size_t copy_len = std::min(src.size(), dest_size - 1);
    std::copy_n(src.data(), copy_len, dest);
    dest[copy_len] = '\0';
}

// Helper: Build device path mapping from shared memory
inline void BuildDeviceMap() {
    if (g_device_map_built) {
        return;
    }

    auto* shared_data = g_driver->GetSharedData();
    if (!shared_data) {
        return;
    }

    g_device_path_to_index.clear();
    uint32_t device_count = shared_data->frame_state.device_count.load(std::memory_order_acquire);

    for (uint32_t i = 0; i < device_count && i < MAX_TRACKED_DEVICES; i++) {
        std::string user_path(shared_data->frame_state.device_poses[i].user_path);
        if (!user_path.empty()) {
            g_device_path_to_index[user_path] = i;
        }
    }

    g_device_map_built = true;
}

// Helper: Extract user path from full binding path
// "/user/hand/left/input/trigger/value" -> "/user/hand/left"
inline std::string ExtractUserPath(const std::string& full_path) {
    size_t input_pos = full_path.find("/input/");
    if (input_pos != std::string::npos) {
        return full_path.substr(0, input_pos);
    }
    return full_path;
}

// Helper: Extract component path from full binding path
// "/user/hand/left/input/trigger/value" -> "/input/trigger/value"
inline std::string ExtractComponentPath(const std::string& full_path) {
    size_t input_pos = full_path.find("/input/");
    if (input_pos != std::string::npos) {
        return full_path.substr(input_pos);
    }
    // For output paths like /output/haptic
    size_t output_pos = full_path.find("/output/");
    if (output_pos != std::string::npos) {
        return full_path.substr(output_pos);
    }
    return full_path;
}

// Helper: Find device index from user path
inline int FindDeviceIndex(const std::string& user_path) {
    BuildDeviceMap();
    auto it = g_device_path_to_index.find(user_path);
    if (it != g_device_path_to_index.end()) {
        return it->second;
    }
    return -1;
}

// Helper: Get instance from session
inline XrResult GetInstanceFromSession(XrSession session, XrInstance* instance) {
    auto session_it = g_sessions.find(session);
    if (session_it == g_sessions.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    *instance = session_it->second;
    return XR_SUCCESS;
}

// Helper: Check if a binding matches the profile and subaction
inline bool IsBindingMatch(const ActionBinding& binding, XrPath subaction_path) {
    // Check if subaction path matches (or no subaction requested)
    if (subaction_path != XR_NULL_PATH && binding.subaction_path != XR_NULL_PATH &&
        binding.subaction_path != subaction_path) {
        return false;
    }

    // Check if binding belongs to current interaction profile
    if (g_current_interaction_profile != XR_NULL_PATH) {
        bool profile_match = false;
        for (const auto& profile : binding.profiles) {
            if (profile == g_current_interaction_profile) {
                profile_match = true;
                break;
            }
        }
        if (!profile_match) {
            return false;
        }
    }

    return true;
}

// Helper: Template for getting action state
template <typename StateType>
inline XrResult GetActionState(XrSession session, const XrActionStateGetInfo* getInfo, StateType* state) {
    if (!state || !getInfo) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);

    XrInstance instance;
    XrResult result = GetInstanceFromSession(session, &instance);
    if (result != XR_SUCCESS) {
        return result;
    }

    auto action_it = g_actions.find(getInfo->action);
    if (action_it == g_actions.end()) {
        return XR_SUCCESS;
    }

    // Look up bindings for this specific action
    auto bindings_it = g_action_bindings.find(getInfo->action);
    if (bindings_it == g_action_bindings.end()) {
        // No bindings for this action
        return XR_SUCCESS;
    }

    // Check each binding for this action
    for (const auto& binding : bindings_it->second) {
        if (!IsBindingMatch(binding, getInfo->subactionPath)) {
            continue;
        }

        char binding_path_str[256];
        uint32_t len = 0;
        xrPathToString(instance, binding.binding_path, sizeof(binding_path_str), &len, binding_path_str);

        std::string path_str(binding_path_str);
        std::string user_path = ExtractUserPath(path_str);
        std::string component_path = ExtractComponentPath(path_str);

        auto value = state->currentState;
        auto result = XR_FALSE;
        if constexpr (std::is_same_v<StateType, XrActionStateBoolean>) {
            result = g_driver->GetInputStateBoolean(user_path.c_str(), component_path.c_str(), 0, value);
        } else if constexpr (std::is_same_v<StateType, XrActionStateFloat>) {
            result = g_driver->GetInputStateFloat(user_path.c_str(), component_path.c_str(), 0, value);
        } else if constexpr (std::is_same_v<StateType, XrActionStateVector2f>) {
            result = g_driver->GetInputStateVector2f(user_path.c_str(), component_path.c_str(), 0, value);
        } else {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (result) {
            // return the first matching binding, for now
            state->currentState = value;
            state->isActive = XR_TRUE;
            return XR_SUCCESS;
        }
    }

    return XR_SUCCESS;
}

// Forward declare all functions
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                     PFN_xrVoidFunction* function);

// Function map for xrGetInstanceProcAddr
static std::unordered_map<std::string, PFN_xrVoidFunction> g_clientFunctionMap;

static void InitializeFunctionMap();

// xrEnumerateApiLayerProperties
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput,
                                                             uint32_t* propertyCountOutput,
                                                             XrApiLayerProperties* properties) {
    spdlog::debug("xrEnumerateApiLayerProperties called");
    if (propertyCountOutput) {
        *propertyCountOutput = 0;
    }
    return XR_SUCCESS;
}

// xrEnumerateInstanceExtensionProperties
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName,
                                                                      uint32_t propertyCapacityInput,
                                                                      uint32_t* propertyCountOutput,
                                                                      XrExtensionProperties* properties) {
    spdlog::debug("xrEnumerateInstanceExtensionProperties called");

    std::vector<const char*> extensions;

#ifdef OX_OPENGL
    extensions.push_back("XR_KHR_opengl_enable");
#endif
#ifdef OX_VULKAN
    extensions.push_back("XR_KHR_vulkan_enable");
    extensions.push_back("XR_KHR_vulkan_enable2");
#endif
#ifdef OX_METAL
    extensions.push_back("XR_KHR_metal_enable");
#endif

    extensions.push_back("XR_HTCX_vive_tracker_interaction");

    const uint32_t extensionCount = static_cast<uint32_t>(extensions.size());

    if (propertyCountOutput) {
        *propertyCountOutput = extensionCount;
    }

    if (propertyCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (!properties) {
        spdlog::error("xrEnumerateInstanceExtensionProperties: Null properties");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    uint32_t count = propertyCapacityInput < extensionCount ? propertyCapacityInput : extensionCount;
    for (uint32_t i = 0; i < count; i++) {
        properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        properties[i].next = nullptr;
        properties[i].extensionVersion = 1;
        safe_copy_string(properties[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, extensions[i]);
    }

    return XR_SUCCESS;
}

// xrCreateInstance
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
    spdlog::debug("xrCreateInstance called");
    if (!createInfo || !instance) {
        spdlog::error("xrCreateInstance: Invalid parameters");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Initialize function map
    if (g_clientFunctionMap.empty()) {
        InitializeFunctionMap();
    }

    // Connect to service
    if (!g_driver->Connect()) {
        spdlog::error("Failed to connect to driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    // Create instance handle
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    uint64_t handle = g_driver->AllocateHandle(HandleType::INSTANCE);
    if (handle == 0) {
        spdlog::error("Failed to allocate instance handle from runtime driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    XrInstance newInstance = reinterpret_cast<XrInstance>(handle);
    g_instances[newInstance] = true;
    *instance = newInstance;

    spdlog::info("OpenXR instance created successfully");
    return XR_SUCCESS;
}

// xrDestroyInstance
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
    spdlog::debug("xrDestroyInstance called");
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) {
        spdlog::error("xrDestroyInstance: Invalid instance handle");
        return XR_ERROR_HANDLE_INVALID;
    }

    g_instances.erase(it);

    // Disconnect from driver if no more instances
    if (g_instances.empty()) {
        g_driver->Disconnect();
    }

    spdlog::info("OpenXR instance destroyed");
    return XR_SUCCESS;
}

// xrGetInstanceProperties
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* instanceProperties) {
    spdlog::debug("xrGetInstanceProperties called");
    if (!instanceProperties) {
        spdlog::error("xrGetInstanceProperties: Null instanceProperties");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    if (g_instances.find(instance) == g_instances.end()) {
        spdlog::error("xrGetInstanceProperties: Invalid instance handle");
        return XR_ERROR_HANDLE_INVALID;
    }

    // Get runtime properties from cached metadata
    auto& props = g_driver->GetRuntimeProperties();
    XrVersion version =
        XR_MAKE_VERSION(props.runtime_version_major, props.runtime_version_minor, props.runtime_version_patch);
    instanceProperties->runtimeVersion = version;
    safe_copy_string(instanceProperties->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, props.runtime_name);

    return XR_SUCCESS;
}

// xrPollEvent - returns session state change events
XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    spdlog::debug("xrPollEvent called");
    if (!eventData) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Get next event from service
    SessionStateEvent service_event;
    if (g_driver->GetNextEvent(service_event)) {
        XrEventDataSessionStateChanged* stateEvent = reinterpret_cast<XrEventDataSessionStateChanged*>(eventData);
        stateEvent->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        stateEvent->next = nullptr;
        stateEvent->session = reinterpret_cast<XrSession>(service_event.session_handle);
        stateEvent->time = service_event.timestamp;
        stateEvent->state = service_event.state;

        spdlog::info("Session state event from driver connection");
        return XR_SUCCESS;
    }

    return XR_EVENT_UNAVAILABLE;
}

// String conversion maps
static const std::unordered_map<XrResult, const char*> g_resultStrings = {
    {XR_SUCCESS, "XR_SUCCESS"},
    {XR_TIMEOUT_EXPIRED, "XR_TIMEOUT_EXPIRED"},
    {XR_SESSION_LOSS_PENDING, "XR_SESSION_LOSS_PENDING"},
    {XR_EVENT_UNAVAILABLE, "XR_EVENT_UNAVAILABLE"},
    {XR_SPACE_BOUNDS_UNAVAILABLE, "XR_SPACE_BOUNDS_UNAVAILABLE"},
    {XR_SESSION_NOT_FOCUSED, "XR_SESSION_NOT_FOCUSED"},
    {XR_FRAME_DISCARDED, "XR_FRAME_DISCARDED"},
    {XR_ERROR_VALIDATION_FAILURE, "XR_ERROR_VALIDATION_FAILURE"},
    {XR_ERROR_RUNTIME_FAILURE, "XR_ERROR_RUNTIME_FAILURE"},
    {XR_ERROR_OUT_OF_MEMORY, "XR_ERROR_OUT_OF_MEMORY"},
    {XR_ERROR_API_VERSION_UNSUPPORTED, "XR_ERROR_API_VERSION_UNSUPPORTED"},
    {XR_ERROR_INITIALIZATION_FAILED, "XR_ERROR_INITIALIZATION_FAILED"},
    {XR_ERROR_FUNCTION_UNSUPPORTED, "XR_ERROR_FUNCTION_UNSUPPORTED"},
    {XR_ERROR_FEATURE_UNSUPPORTED, "XR_ERROR_FEATURE_UNSUPPORTED"},
    {XR_ERROR_EXTENSION_NOT_PRESENT, "XR_ERROR_EXTENSION_NOT_PRESENT"},
    {XR_ERROR_LIMIT_REACHED, "XR_ERROR_LIMIT_REACHED"},
    {XR_ERROR_SIZE_INSUFFICIENT, "XR_ERROR_SIZE_INSUFFICIENT"},
    {XR_ERROR_HANDLE_INVALID, "XR_ERROR_HANDLE_INVALID"},
    {XR_ERROR_INSTANCE_LOST, "XR_ERROR_INSTANCE_LOST"},
    {XR_ERROR_SESSION_RUNNING, "XR_ERROR_SESSION_RUNNING"},
    {XR_ERROR_SESSION_NOT_RUNNING, "XR_ERROR_SESSION_NOT_RUNNING"},
    {XR_ERROR_SESSION_LOST, "XR_ERROR_SESSION_LOST"},
    {XR_ERROR_SYSTEM_INVALID, "XR_ERROR_SYSTEM_INVALID"},
    {XR_ERROR_PATH_INVALID, "XR_ERROR_PATH_INVALID"},
    {XR_ERROR_PATH_COUNT_EXCEEDED, "XR_ERROR_PATH_COUNT_EXCEEDED"},
    {XR_ERROR_PATH_FORMAT_INVALID, "XR_ERROR_PATH_FORMAT_INVALID"},
    {XR_ERROR_PATH_UNSUPPORTED, "XR_ERROR_PATH_UNSUPPORTED"},
    {XR_ERROR_LAYER_INVALID, "XR_ERROR_LAYER_INVALID"},
    {XR_ERROR_LAYER_LIMIT_EXCEEDED, "XR_ERROR_LAYER_LIMIT_EXCEEDED"},
    {XR_ERROR_SWAPCHAIN_RECT_INVALID, "XR_ERROR_SWAPCHAIN_RECT_INVALID"},
    {XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED"},
    {XR_ERROR_ACTION_TYPE_MISMATCH, "XR_ERROR_ACTION_TYPE_MISMATCH"},
    {XR_ERROR_SESSION_NOT_READY, "XR_ERROR_SESSION_NOT_READY"},
    {XR_ERROR_SESSION_NOT_STOPPING, "XR_ERROR_SESSION_NOT_STOPPING"},
    {XR_ERROR_TIME_INVALID, "XR_ERROR_TIME_INVALID"},
    {XR_ERROR_REFERENCE_SPACE_UNSUPPORTED, "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED"},
    {XR_ERROR_FILE_ACCESS_ERROR, "XR_ERROR_FILE_ACCESS_ERROR"},
    {XR_ERROR_FILE_CONTENTS_INVALID, "XR_ERROR_FILE_CONTENTS_INVALID"},
    {XR_ERROR_FORM_FACTOR_UNSUPPORTED, "XR_ERROR_FORM_FACTOR_UNSUPPORTED"},
    {XR_ERROR_FORM_FACTOR_UNAVAILABLE, "XR_ERROR_FORM_FACTOR_UNAVAILABLE"},
    {XR_ERROR_API_LAYER_NOT_PRESENT, "XR_ERROR_API_LAYER_NOT_PRESENT"},
    {XR_ERROR_CALL_ORDER_INVALID, "XR_ERROR_CALL_ORDER_INVALID"},
    {XR_ERROR_GRAPHICS_DEVICE_INVALID, "XR_ERROR_GRAPHICS_DEVICE_INVALID"},
    {XR_ERROR_POSE_INVALID, "XR_ERROR_POSE_INVALID"},
    {XR_ERROR_INDEX_OUT_OF_RANGE, "XR_ERROR_INDEX_OUT_OF_RANGE"},
    {XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED"},
    {XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED, "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED"},
    {XR_ERROR_NAME_DUPLICATED, "XR_ERROR_NAME_DUPLICATED"},
    {XR_ERROR_NAME_INVALID, "XR_ERROR_NAME_INVALID"},
    {XR_ERROR_ACTIONSET_NOT_ATTACHED, "XR_ERROR_ACTIONSET_NOT_ATTACHED"},
    {XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED"},
    {XR_ERROR_LOCALIZED_NAME_DUPLICATED, "XR_ERROR_LOCALIZED_NAME_DUPLICATED"},
    {XR_ERROR_LOCALIZED_NAME_INVALID, "XR_ERROR_LOCALIZED_NAME_INVALID"},
    {XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING, "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING"},
};

static const std::unordered_map<XrStructureType, const char*> g_structureTypeStrings = {
    {XR_TYPE_UNKNOWN, "XR_TYPE_UNKNOWN"},
    {XR_TYPE_API_LAYER_PROPERTIES, "XR_TYPE_API_LAYER_PROPERTIES"},
    {XR_TYPE_EXTENSION_PROPERTIES, "XR_TYPE_EXTENSION_PROPERTIES"},
    {XR_TYPE_INSTANCE_CREATE_INFO, "XR_TYPE_INSTANCE_CREATE_INFO"},
    {XR_TYPE_SYSTEM_GET_INFO, "XR_TYPE_SYSTEM_GET_INFO"},
    {XR_TYPE_SYSTEM_PROPERTIES, "XR_TYPE_SYSTEM_PROPERTIES"},
    {XR_TYPE_VIEW_LOCATE_INFO, "XR_TYPE_VIEW_LOCATE_INFO"},
    {XR_TYPE_VIEW, "XR_TYPE_VIEW"},
    {XR_TYPE_SESSION_CREATE_INFO, "XR_TYPE_SESSION_CREATE_INFO"},
    {XR_TYPE_SWAPCHAIN_CREATE_INFO, "XR_TYPE_SWAPCHAIN_CREATE_INFO"},
    {XR_TYPE_SESSION_BEGIN_INFO, "XR_TYPE_SESSION_BEGIN_INFO"},
    {XR_TYPE_VIEW_STATE, "XR_TYPE_VIEW_STATE"},
    {XR_TYPE_FRAME_END_INFO, "XR_TYPE_FRAME_END_INFO"},
    {XR_TYPE_HAPTIC_VIBRATION, "XR_TYPE_HAPTIC_VIBRATION"},
    {XR_TYPE_EVENT_DATA_BUFFER, "XR_TYPE_EVENT_DATA_BUFFER"},
    {XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING, "XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING"},
    {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED"},
    {XR_TYPE_ACTION_STATE_BOOLEAN, "XR_TYPE_ACTION_STATE_BOOLEAN"},
    {XR_TYPE_ACTION_STATE_FLOAT, "XR_TYPE_ACTION_STATE_FLOAT"},
    {XR_TYPE_ACTION_STATE_VECTOR2F, "XR_TYPE_ACTION_STATE_VECTOR2F"},
    {XR_TYPE_ACTION_STATE_POSE, "XR_TYPE_ACTION_STATE_POSE"},
    {XR_TYPE_ACTION_SET_CREATE_INFO, "XR_TYPE_ACTION_SET_CREATE_INFO"},
    {XR_TYPE_ACTION_CREATE_INFO, "XR_TYPE_ACTION_CREATE_INFO"},
    {XR_TYPE_INSTANCE_PROPERTIES, "XR_TYPE_INSTANCE_PROPERTIES"},
    {XR_TYPE_FRAME_WAIT_INFO, "XR_TYPE_FRAME_WAIT_INFO"},
    {XR_TYPE_COMPOSITION_LAYER_PROJECTION, "XR_TYPE_COMPOSITION_LAYER_PROJECTION"},
    {XR_TYPE_COMPOSITION_LAYER_QUAD, "XR_TYPE_COMPOSITION_LAYER_QUAD"},
    {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, "XR_TYPE_REFERENCE_SPACE_CREATE_INFO"},
    {XR_TYPE_ACTION_SPACE_CREATE_INFO, "XR_TYPE_ACTION_SPACE_CREATE_INFO"},
    {XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, "XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING"},
    {XR_TYPE_VIEW_CONFIGURATION_VIEW, "XR_TYPE_VIEW_CONFIGURATION_VIEW"},
    {XR_TYPE_SPACE_LOCATION, "XR_TYPE_SPACE_LOCATION"},
    {XR_TYPE_SPACE_VELOCITY, "XR_TYPE_SPACE_VELOCITY"},
    {XR_TYPE_FRAME_STATE, "XR_TYPE_FRAME_STATE"},
    {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES, "XR_TYPE_VIEW_CONFIGURATION_PROPERTIES"},
    {XR_TYPE_FRAME_BEGIN_INFO, "XR_TYPE_FRAME_BEGIN_INFO"},
    {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, "XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW"},
    {XR_TYPE_EVENT_DATA_EVENTS_LOST, "XR_TYPE_EVENT_DATA_EVENTS_LOST"},
    {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, "XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING"},
    {XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED, "XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED"},
    {XR_TYPE_INTERACTION_PROFILE_STATE, "XR_TYPE_INTERACTION_PROFILE_STATE"},
    {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, "XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO"},
    {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, "XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO"},
    {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, "XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO"},
    {XR_TYPE_ACTION_STATE_GET_INFO, "XR_TYPE_ACTION_STATE_GET_INFO"},
    {XR_TYPE_HAPTIC_ACTION_INFO, "XR_TYPE_HAPTIC_ACTION_INFO"},
    {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, "XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO"},
    {XR_TYPE_ACTIONS_SYNC_INFO, "XR_TYPE_ACTIONS_SYNC_INFO"},
    {XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, "XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO"},
    {XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO, "XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO"},
    {XR_TYPE_COMPOSITION_LAYER_CUBE_KHR, "XR_TYPE_COMPOSITION_LAYER_CUBE_KHR"},
    {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, "XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR"},
    {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, "XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR"},
    {XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR, "XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR"},
    {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR, "XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR"},
    {XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR, "XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR"},
    {XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR, "XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR"},
    {XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR, "XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR"},
    {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR, "XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR"},
    {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR, "XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR"},
    {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, "XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR"},
    {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, "XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR"},
    {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR, "XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR"},
};

// Rest of the runtime functions (simplified versions)
XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value,
                                                char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    auto it = g_resultStrings.find(value);
    if (it != g_resultStrings.end()) {
        safe_copy_string(buffer, XR_MAX_RESULT_STRING_SIZE, it->second);
    } else {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_RESULT_%d", value);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStructureTypeToString(XrInstance instance, XrStructureType value,
                                                       char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    auto it = g_structureTypeStrings.find(value);
    if (it != g_structureTypeStrings.end()) {
        safe_copy_string(buffer, XR_MAX_STRUCTURE_NAME_SIZE, it->second);
    } else {
        std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", value);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
    spdlog::debug("xrGetSystem called");
    if (!getInfo || !systemId) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *systemId = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId,
                                                     XrSystemProperties* properties) {
    spdlog::debug("xrGetSystemProperties called");
    if (!properties) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Get system properties from cached metadata
    auto& sys_props = g_driver->GetSystemProperties();
    properties->systemId = systemId;
    safe_copy_string(properties->systemName, XR_MAX_SYSTEM_NAME_SIZE, sys_props.system_name);
    properties->graphicsProperties.maxSwapchainImageWidth = sys_props.max_swapchain_width;
    properties->graphicsProperties.maxSwapchainImageHeight = sys_props.max_swapchain_height;
    properties->graphicsProperties.maxLayerCount = sys_props.max_layer_count;
    properties->trackingProperties.orientationTracking = sys_props.orientation_tracking;
    properties->trackingProperties.positionTracking = sys_props.position_tracking;

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                                             uint32_t viewConfigurationTypeCapacityInput,
                                                             uint32_t* viewConfigurationTypeCountOutput,
                                                             XrViewConfigurationType* viewConfigurationTypes) {
    spdlog::debug("xrEnumerateViewConfigurations called");
    const XrViewConfigurationType configs[] = {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};

    if (viewConfigurationTypeCountOutput) {
        *viewConfigurationTypeCountOutput = 1;
    }

    if (viewConfigurationTypeCapacityInput > 0 && viewConfigurationTypes) {
        viewConfigurationTypes[0] = configs[0];
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetViewConfigurationProperties(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    XrViewConfigurationProperties* configurationProperties) {
    spdlog::debug("xrGetViewConfigurationProperties called");
    if (!configurationProperties) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    configurationProperties->viewConfigurationType = viewConfigurationType;
    configurationProperties->fovMutable = XR_FALSE;

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                                 XrViewConfigurationType viewConfigurationType,
                                                                 uint32_t viewCapacityInput, uint32_t* viewCountOutput,
                                                                 XrViewConfigurationView* views) {
    spdlog::debug("xrEnumerateViewConfigurationViews called");

    if (viewCountOutput) {
        *viewCountOutput = 2;  // Stereo
    }

    if (viewCapacityInput > 0 && views) {
        // Get view configurations from cached metadata
        auto& view_configs = g_driver->GetViewConfigurations();

        for (uint32_t i = 0; i < 2 && i < viewCapacityInput; i++) {
            views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

            auto& config = view_configs.views[i];
            views[i].recommendedImageRectWidth = config.recommended_width;
            views[i].maxImageRectWidth = config.recommended_width * 2;
            views[i].recommendedImageRectHeight = config.recommended_height;
            views[i].maxImageRectHeight = config.recommended_height * 2;
            views[i].recommendedSwapchainSampleCount = config.recommended_sample_count;
            views[i].maxSwapchainSampleCount = config.max_sample_count;
        }
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                                XrViewConfigurationType viewConfigurationType,
                                                                uint32_t environmentBlendModeCapacityInput,
                                                                uint32_t* environmentBlendModeCountOutput,
                                                                XrEnvironmentBlendMode* environmentBlendModes) {
    spdlog::debug("xrEnumerateEnvironmentBlendModes called");

    if (environmentBlendModeCountOutput) {
        *environmentBlendModeCountOutput = 1;
    }

    if (environmentBlendModeCapacityInput > 0 && environmentBlendModes) {
        environmentBlendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                               XrSession* session) {
    spdlog::debug("xrCreateSession called");
    if (!createInfo || !session) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Check for graphics binding in the next chain (required for rendering)
    const void* next = createInfo->next;
    bool hasGraphicsBinding = false;
    SessionGraphicsBinding graphicsBinding;

    // Try each graphics API's detection function
#ifdef OX_OPENGL
    if (!hasGraphicsBinding && opengl::DetectGraphicsBinding(next, &graphicsBinding.bindingData)) {
        hasGraphicsBinding = true;
        graphicsBinding.graphicsAPI = GraphicsAPI::OpenGL;
    }
#endif
#ifdef OX_VULKAN
    if (!hasGraphicsBinding && vulkan::DetectGraphicsBinding(next, &graphicsBinding.bindingData)) {
        hasGraphicsBinding = true;
        graphicsBinding.graphicsAPI = GraphicsAPI::Vulkan;
    }
#endif
#ifdef OX_METAL
    if (!hasGraphicsBinding && metal::DetectGraphicsBinding(next, &graphicsBinding.bindingData)) {
        hasGraphicsBinding = true;
        graphicsBinding.graphicsAPI = GraphicsAPI::Metal;
    }
#endif

    if (hasGraphicsBinding) {
        spdlog::info("Session created with graphics binding");
    }

    g_driver->SendRequest(MessageType::CREATE_SESSION);

    auto* shared_data = g_driver->GetSharedData();
    if (!shared_data) {
        spdlog::error("xrCreateSession: No driver connection");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    // Wait briefly for the driver connection to set the session handle
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t handle = shared_data->active_session_handle.load(std::memory_order_acquire);

    if (handle == 0) {
        spdlog::error("xrCreateSession: Driver connection did not create session");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    XrSession newSession = reinterpret_cast<XrSession>(handle);
    g_sessions[newSession] = instance;
    if (hasGraphicsBinding) {
        g_session_graphics[newSession] = graphicsBinding;
    }
    *session = newSession;

    spdlog::info("Session created");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session) {
    spdlog::debug("xrDestroySession called");

    g_driver->SendRequest(MessageType::DESTROY_SESSION);

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    g_sessions.erase(session);
    g_session_graphics.erase(session);

    spdlog::info("Session destroyed");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) {
    spdlog::debug("xrBeginSession called");
    g_driver->SetSessionState(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session) {
    spdlog::debug("xrEndSession called");
    g_driver->SetSessionState(XR_SESSION_STATE_IDLE);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession session) {
    spdlog::debug("xrRequestExitSession called");

    RequestExitSessionRequest request;
    request.session_handle = reinterpret_cast<uint64_t>(session);

    if (!g_driver->SendRequest(MessageType::REQUEST_EXIT_SESSION, &request, sizeof(request))) {
        spdlog::error("Failed to send REQUEST_EXIT_SESSION message");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t spaceCapacityInput,
                                                          uint32_t* spaceCountOutput, XrReferenceSpaceType* spaces) {
    spdlog::debug("xrEnumerateReferenceSpaces called");
    const XrReferenceSpaceType supportedSpaces[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                    XR_REFERENCE_SPACE_TYPE_STAGE};

    if (spaceCountOutput) {
        *spaceCountOutput = 3;
    }

    if (spaceCapacityInput > 0 && spaces) {
        uint32_t count = spaceCapacityInput < 3 ? spaceCapacityInput : 3;
        for (uint32_t i = 0; i < count; i++) {
            spaces[i] = supportedSpaces[i];
        }
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo,
                                                      XrSpace* space) {
    spdlog::debug("xrCreateReferenceSpace called");
    if (!createInfo || !space) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    uint64_t handle = g_driver->AllocateHandle(HandleType::SPACE);
    if (handle == 0) {
        spdlog::error("Failed to allocate space handle from runtime driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    XrSpace newSpace = reinterpret_cast<XrSpace>(handle);
    g_spaces[newSpace] = session;

    // Store reference space metadata
    ReferenceSpaceData space_data;
    space_data.type = createInfo->referenceSpaceType;
    space_data.pose_in_reference_space = createInfo->poseInReferenceSpace;
    g_reference_spaces[newSpace] = space_data;

    *space = newSpace;

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space) {
    spdlog::debug("xrDestroySpace called");
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    g_spaces.erase(space);
    g_reference_spaces.erase(space);
    g_action_spaces.erase(space);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    spdlog::debug("xrLocateSpace called");
    if (!location) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);

    // Check if this is an action space
    auto it = g_action_spaces.find(space);
    if (it != g_action_spaces.end()) {
        // This is an action space - get controller pose from shared memory
        auto* shared_data = g_driver->GetSharedData();
        if (!shared_data) {
            location->locationFlags = 0;
            return XR_SUCCESS;
        }

        // Determine device index from subaction path
        int device_index = -1;
        if (it->second.subaction_path != 0) {
            // Convert path to string to get user path
            char subaction_path_str[256];
            uint32_t len = 0;
            auto sessions_it = g_spaces.find(space);
            if (sessions_it != g_spaces.end()) {
                auto instance_it = g_sessions.find(sessions_it->second);
                if (instance_it != g_sessions.end()) {
                    xrPathToString(instance_it->second, it->second.subaction_path, sizeof(subaction_path_str), &len,
                                   subaction_path_str);
                    std::string user_path(subaction_path_str);
                    device_index = FindDeviceIndex(user_path);
                }
            }
        }

        if (device_index >= 0 && device_index < MAX_TRACKED_DEVICES) {
            // Read device pose from shared memory
            auto& device_data = shared_data->frame_state.device_poses[device_index];

            if (device_data.is_active) {
                location->locationFlags =
                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

                location->pose = device_data.pose.pose;
            } else {
                // Device not active
                location->locationFlags = 0;
            }
        } else {
            location->locationFlags = 0;
        }

        return XR_SUCCESS;
    }

    // Check if this is a VIEW reference space - return actual headset pose from simulator
    auto ref_it = g_reference_spaces.find(space);
    if (ref_it != g_reference_spaces.end() && ref_it->second.type == XR_REFERENCE_SPACE_TYPE_VIEW) {
        auto* shared_data = g_driver->GetSharedData();
        if (shared_data && shared_data->frame_state.view_count.load(std::memory_order_acquire) >= 2) {
            // Calculate center between left and right eye poses
            auto& left_eye = shared_data->frame_state.views[0].pose.pose;
            auto& right_eye = shared_data->frame_state.views[1].pose.pose;

            location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                      XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                      XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

            // Average position between eyes
            location->pose.position.x = (left_eye.position.x + right_eye.position.x) * 0.5f;
            location->pose.position.y = (left_eye.position.y + right_eye.position.y) * 0.5f;
            location->pose.position.z = (left_eye.position.z + right_eye.position.z) * 0.5f;

            // Use left eye orientation (both eyes should have same orientation)
            location->pose.orientation = left_eye.orientation;

            return XR_SUCCESS;
        }
    }

    // Regular reference space (LOCAL, STAGE) - return identity pose
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    location->pose.orientation.x = 0.0f;
    location->pose.orientation.y = 0.0f;
    location->pose.orientation.z = 0.0f;
    location->pose.orientation.w = 1.0f;
    location->pose.position.x = 0.0f;
    location->pose.position.y = 0.0f;
    location->pose.position.z = 0.0f;

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* locateInfo,
                                              XrSpaceLocations* spaceLocations) {
    spdlog::debug("xrLocateSpaces called");
    if (!locateInfo || !spaceLocations) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (locateInfo->spaceCount == 0 || !locateInfo->spaces) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Time validation: must be a positive non-zero XrTime
    if (locateInfo->time <= 0) {
        return XR_ERROR_TIME_INVALID;
    }

    if (!spaceLocations->locations || spaceLocations->locationCount < locateInfo->spaceCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    spaceLocations->type = XR_TYPE_SPACE_LOCATIONS;
    spaceLocations->locationCount = locateInfo->spaceCount;

    // Reuse xrLocateSpace for each space; xrLocateSpace does its own locking/validation
    for (uint32_t i = 0; i < locateInfo->spaceCount; ++i) {
        XrSpaceLocation spaceLoc;
        XrResult res = xrLocateSpace(locateInfo->spaces[i], locateInfo->baseSpace, locateInfo->time, &spaceLoc);
        if (XR_FAILED(res)) {
            return res;
        }
        spaceLocations->locations[i].pose = spaceLoc.pose;
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo* frameWaitInfo,
                                           XrFrameState* frameState) {
    spdlog::debug("xrWaitFrame called");
    if (!frameState) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Read frame data from shared memory
    auto* shared_data = g_driver->GetSharedData();
    if (shared_data) {
        frameState->predictedDisplayTime =
            shared_data->frame_state.predicted_display_time.load(std::memory_order_acquire);
        frameState->predictedDisplayPeriod = 11111111;  // ~90 FPS
    } else {
        frameState->predictedDisplayTime = 0;
        frameState->predictedDisplayPeriod = 11111111;
    }

    frameState->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) {
    spdlog::debug("xrBeginFrame called");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    spdlog::debug("xrEndFrame called");
    if (!frameEndInfo) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (!g_driver->IsConnected()) {
        spdlog::error("Driver connection not available");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    // Process submitted layers
    if (frameEndInfo->layerCount > 0 && frameEndInfo->layers) {
        spdlog::debug("xrEndFrame: Processing submitted layers");

        // We only handle projection layers for now
        for (uint32_t layerIdx = 0; layerIdx < frameEndInfo->layerCount; layerIdx++) {
            const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[layerIdx];
            if (!layer) continue;

            if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const XrCompositionLayerProjection* projLayer =
                    reinterpret_cast<const XrCompositionLayerProjection*>(layer);

                // Process each view (eye)
                for (uint32_t viewIdx = 0; viewIdx < projLayer->viewCount && viewIdx < 2; viewIdx++) {
                    const XrCompositionLayerProjectionView& view = projLayer->views[viewIdx];
                    XrSwapchain swapchain = view.subImage.swapchain;

                    // Look up swapchain data
                    std::lock_guard<std::mutex> lock(g_instance_mutex);
                    auto swapchainIt = g_swapchains.find(swapchain);
                    if (swapchainIt == g_swapchains.end()) {
                        spdlog::error("Invalid swapchain in submitted layer");
                        continue;
                    }

                    const SwapchainData& swapchainData = swapchainIt->second;
                    // Get the current swapchain image index (we use 0 for now, real impl tracks acquired index)
                    uint32_t imageIdx = 0;
                    size_t destSize = static_cast<size_t>(swapchainData.width) * swapchainData.height * 4;
                    bool copySuccess = false;

                    auto& submitBuffer = g_submit_buffers[viewIdx];
                    submitBuffer.resize(destSize);

                    // Copy texture pixels based on graphics API
                    switch (swapchainData.graphicsAPI) {
#ifdef OX_OPENGL
                        case GraphicsAPI::OpenGL:
                            if (imageIdx < swapchainData.glTextureIds.size()) {
                                copySuccess = opengl::CopyTextureToMemory(swapchainData.glTextureIds[imageIdx],
                                                                          swapchainData.width, swapchainData.height,
                                                                          submitBuffer.data(), destSize);
                            }
                            break;
#endif
#ifdef OX_VULKAN
                        case GraphicsAPI::Vulkan:
                            if (imageIdx < swapchainData.vkImages.size()) {
                                copySuccess = vulkan::CopyImageToMemory(
                                    swapchainData.vkDevice, swapchainData.vkPhysicalDevice, swapchainData.vkQueue,
                                    swapchainData.vkCommandPool, swapchainData.vkImages[imageIdx], swapchainData.width,
                                    swapchainData.height, static_cast<VkFormat>(swapchainData.format),
                                    submitBuffer.data(), destSize);
                            }
                            break;
#endif
#ifdef OX_METAL
                        case GraphicsAPI::Metal:
                            if (imageIdx < swapchainData.metalTextures.size()) {
                                copySuccess = metal::CopyTextureToMemory(swapchainData.metalTextures[imageIdx],
                                                                         swapchainData.width, swapchainData.height,
                                                                         submitBuffer.data(), destSize);
                            }
                            break;
#endif
                        default:
                            spdlog::error("Unsupported graphics API for texture copy");
                            break;
                    }

                    if (copySuccess) {
                        g_driver->SubmitFramePixels(viewIdx, swapchainData.width, swapchainData.height,
                                                    static_cast<uint32_t>(swapchainData.format), submitBuffer.data(),
                                                    static_cast<uint32_t>(destSize));
                        spdlog::debug(("Copied texture for eye " + std::to_string(viewIdx)).c_str());
                    } else {
                        spdlog::error(("Failed to copy texture for eye " + std::to_string(viewIdx)).c_str());
                    }
                }
            }
        }
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                             XrViewState* viewState, uint32_t viewCapacityInput,
                                             uint32_t* viewCountOutput, XrView* views) {
    spdlog::debug("xrLocateViews called");
    if (!viewLocateInfo || !viewState || !viewCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *viewCountOutput = 2;

    if (viewCapacityInput == 0) {
        return XR_SUCCESS;
    }

    viewState->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;

    // Read pose data from shared memory
    auto* shared_data = g_driver->GetSharedData();
    if (views && viewCapacityInput >= 2 && shared_data) {
        uint32_t view_count = shared_data->frame_state.view_count.load(std::memory_order_acquire);

        for (uint32_t i = 0; i < 2 && i < viewCapacityInput; i++) {
            views[i].type = XR_TYPE_VIEW;
            views[i].next = nullptr;

            auto& view_data = shared_data->frame_state.views[i];

            views[i].pose = view_data.pose.pose;

            views[i].fov.angleLeft = view_data.fov[0];
            views[i].fov.angleRight = view_data.fov[1];
            views[i].fov.angleUp = view_data.fov[2];
            views[i].fov.angleDown = view_data.fov[3];
        }
    }

    return XR_SUCCESS;
}

// Action system stubs
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo,
                                                 XrActionSet* actionSet) {
    spdlog::debug("xrCreateActionSet called");
    if (!createInfo || !actionSet) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    uint64_t handle = g_driver->AllocateHandle(HandleType::ACTION_SET);
    if (handle == 0) {
        spdlog::error("Failed to allocate action set handle from runtime driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *actionSet = reinterpret_cast<XrActionSet>(handle);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet) {
    spdlog::debug("xrDestroyActionSet called");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo,
                                              XrAction* action) {
    spdlog::debug("xrCreateAction called");
    if (!createInfo || !action) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    uint64_t handle = g_driver->AllocateHandle(HandleType::ACTION);
    if (handle == 0) {
        spdlog::error("Failed to allocate action handle from runtime driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *action = reinterpret_cast<XrAction>(handle);

    // Store action metadata
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    ActionData& data = g_actions[*action];
    data.type = createInfo->actionType;
    data.action_set = actionSet;
    data.name = createInfo->actionName;
    if (createInfo->countSubactionPaths > 0 && createInfo->subactionPaths) {
        data.subaction_paths.assign(createInfo->subactionPaths,
                                    createInfo->subactionPaths + createInfo->countSubactionPaths);
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action) {
    spdlog::debug("xrDestroyAction called");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) {
    spdlog::debug("xrSuggestInteractionProfileBindings called");
    if (!suggestedBindings) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Convert XrPath to string for storage
    char profile_path[256];
    uint32_t out_len = 0;
    XrResult result =
        xrPathToString(instance, suggestedBindings->interactionProfile, sizeof(profile_path), &out_len, profile_path);
    if (result == XR_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_instance_mutex);
        // Store this profile as a suggested profile
        std::string profile_str(profile_path);
        if (std::find(g_suggested_profiles.begin(), g_suggested_profiles.end(), profile_str) ==
            g_suggested_profiles.end()) {
            g_suggested_profiles.push_back(profile_str);
            spdlog::debug(("Suggested profile: " + profile_str).c_str());
        }

        // Store the bindings for this profile
        for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; i++) {
            const XrActionSuggestedBinding& binding = suggestedBindings->suggestedBindings[i];
            XrPath binding_path = binding.binding;

            // Find the subaction path from the action metadata
            XrPath subaction_path = XR_NULL_PATH;
            auto action_it = g_actions.find(binding.action);
            if (action_it != g_actions.end()) {
                // For actions with subaction paths, extract from the binding path
                if (!action_it->second.subaction_paths.empty()) {
                    char binding_path_str[256];
                    uint32_t len = 0;
                    xrPathToString(instance, binding_path, sizeof(binding_path_str), &len, binding_path_str);
                    std::string path_str(binding_path_str);
                    // Check if path contains /user/hand/left or /user/hand/right
                    if (path_str.find("/user/hand/left") != std::string::npos) {
                        subaction_path = action_it->second.subaction_paths[0];  // Assuming first is left
                    } else if (path_str.find("/user/hand/right") != std::string::npos) {
                        subaction_path = action_it->second.subaction_paths.size() > 1
                                             ? action_it->second.subaction_paths[1]
                                             : action_it->second.subaction_paths[0];
                    }
                }
            }

            // Get or create bindings list for this action
            auto& bindings_list = g_action_bindings[binding.action];

            // Check if this binding path already exists for this action
            bool found = false;
            for (auto& action_binding : bindings_list) {
                if (action_binding.binding_path == binding_path) {
                    // Update existing binding: add profile if not already present
                    bool profile_exists = false;
                    for (const auto& profile : action_binding.profiles) {
                        if (profile == suggestedBindings->interactionProfile) {
                            profile_exists = true;
                            break;
                        }
                    }
                    if (!profile_exists) {
                        action_binding.profiles.push_back(suggestedBindings->interactionProfile);
                    }
                    action_binding.subaction_path = subaction_path;
                    found = true;
                    break;
                }
            }

            if (!found) {
                // Add new binding for this action
                bindings_list.push_back(
                    ActionBinding{binding_path, subaction_path, {suggestedBindings->interactionProfile}});
            }
        }
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session,
                                                         const XrSessionActionSetsAttachInfo* attachInfo) {
    spdlog::debug("xrAttachSessionActionSets called");

    // When action sets are attached, select the best matching interaction profile
    std::lock_guard<std::mutex> lock(g_instance_mutex);

    // Get driver-supported profiles from service
    const auto& driver_profiles = g_driver->GetInteractionProfiles();

    // Try to find a match between suggested profiles and driver-supported profiles
    for (const auto& suggested : g_suggested_profiles) {
        for (uint32_t i = 0; i < driver_profiles.profile_count && i < 8; i++) {
            std::string driver_profile(driver_profiles.profiles[i]);
            if (suggested == driver_profile) {
                // Found a match! Set this as the active profile
                auto it = g_sessions.find(session);
                if (it != g_sessions.end()) {
                    XrInstance instance = it->second;
                    XrResult result = xrStringToPath(instance, suggested.c_str(), &g_current_interaction_profile);
                    if (result == XR_SUCCESS) {
                        spdlog::info(("Activated interaction profile: " + suggested).c_str());
                        return XR_SUCCESS;
                    }
                }
            }
        }
    }

    // If no match found but driver has profiles, use the first one
    if (driver_profiles.profile_count > 0) {
        auto it = g_sessions.find(session);
        if (it != g_sessions.end()) {
            XrInstance instance = it->second;
            std::string profile(driver_profiles.profiles[0]);
            XrResult result = xrStringToPath(instance, profile.c_str(), &g_current_interaction_profile);
            if (result == XR_SUCCESS) {
                spdlog::info(("Activated default driver profile: " + profile).c_str());
            }
        }
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                              XrInteractionProfileState* interactionProfile) {
    spdlog::debug("xrGetCurrentInteractionProfile called");
    if (!interactionProfile) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    interactionProfile->interactionProfile = g_current_interaction_profile;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
    spdlog::debug("xrSyncActions called");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                       XrActionStateBoolean* state) {
    spdlog::debug("xrGetActionStateBoolean called");

    // Initialize to inactive state
    state->currentState = XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_FALSE;

    return GetActionState(session, getInfo, state);
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                                     XrActionStateFloat* state) {
    spdlog::debug("xrGetActionStateFloat called");

    // Initialize to inactive state
    state->currentState = 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_FALSE;

    return GetActionState(session, getInfo, state);
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                                        XrActionStateVector2f* state) {
    spdlog::debug("xrGetActionStateVector2f called");

    // Initialize to inactive state
    state->currentState.x = 0.0f;
    state->currentState.y = 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_FALSE;

    return GetActionState(session, getInfo, state);
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo,
                                                    XrActionStatePose* state) {
    spdlog::debug("xrGetActionStatePose called");
    if (!state) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    state->isActive = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo,
                                                   XrSpace* space) {
    spdlog::debug("xrCreateActionSpace called");
    if (!createInfo || !space) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    uint64_t handle = g_driver->AllocateHandle(HandleType::SPACE);
    if (handle == 0) {
        spdlog::error("Failed to allocate action space handle from runtime driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    XrSpace newSpace = reinterpret_cast<XrSpace>(handle);
    g_spaces[newSpace] = session;

    // Store action space metadata
    ActionSpaceData data;
    data.action = createInfo->action;
    data.subaction_path = createInfo->subactionPath;
    g_action_spaces[newSpace] = data;

    *space = newSpace;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType,
                                                             XrExtent2Df* bounds) {
    spdlog::debug("xrGetReferenceSpaceBoundsRect called");
    if (!bounds) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Return failure to indicate bounds are not available
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo* enumerateInfo,
                                 uint32_t sourceCapacityInput, uint32_t* sourceCountOutput, XrPath* sources) {
    spdlog::debug("xrEnumerateBoundSourcesForAction called");
    if (!enumerateInfo) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (sourceCountOutput) {
        *sourceCountOutput = 0;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInputSourceLocalizedName(XrSession session,
                                                             const XrInputSourceLocalizedNameGetInfo* getInfo,
                                                             uint32_t bufferCapacityInput, uint32_t* bufferCountOutput,
                                                             char* buffer) {
    spdlog::debug("xrGetInputSourceLocalizedName called");
    if (!getInfo) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const char* name = "Unknown";
    uint32_t len = strlen(name) + 1;
    if (bufferCountOutput) {
        *bufferCountOutput = len;
    }
    if (bufferCapacityInput > 0 && buffer) {
        safe_copy_string(buffer, bufferCapacityInput, name);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                                     const XrHapticBaseHeader* hapticFeedback) {
    spdlog::debug("xrApplyHapticFeedback called");
    if (!hapticActionInfo || !hapticFeedback) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Haptic feedback not implemented yet
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) {
    spdlog::debug("xrStopHapticFeedback called");
    if (!hapticActionInfo) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Haptic feedback not implemented yet
    return XR_SUCCESS;
}

// Swapchain functions
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t formatCapacityInput,
                                                           uint32_t* formatCountOutput, int64_t* formats) {
    spdlog::debug("xrEnumerateSwapchainFormats called");

    // Get supported formats from each graphics API
    std::vector<int64_t> supportedFormats;

#ifdef OX_OPENGL
    auto glFormats = opengl::GetSupportedFormats();
    supportedFormats.insert(supportedFormats.end(), glFormats.begin(), glFormats.end());
#endif

#ifdef OX_VULKAN
    auto vkFormats = vulkan::GetSupportedFormats();
    supportedFormats.insert(supportedFormats.end(), vkFormats.begin(), vkFormats.end());
#endif

#ifdef OX_METAL
    auto metalFormats = metal::GetSupportedFormats();
    supportedFormats.insert(supportedFormats.end(), metalFormats.begin(), metalFormats.end());
#endif

    const uint32_t formatCount = static_cast<uint32_t>(supportedFormats.size());

    if (formatCountOutput) {
        *formatCountOutput = formatCount;
    }

    if (formatCapacityInput > 0 && formats) {
        uint32_t count = formatCapacityInput < formatCount ? formatCapacityInput : formatCount;
        for (uint32_t i = 0; i < count; i++) {
            formats[i] = supportedFormats[i];
        }
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                                 XrSwapchain* swapchain) {
    spdlog::debug("xrCreateSwapchain called");
    if (!createInfo || !swapchain) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Add Metal-specific validations
    if (createInfo->mipCount > 1) {
        spdlog::error("Metal swapchains do not support mipmap chains (mipCount > 1)");
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    if (createInfo->arraySize > 1) {
        spdlog::error("Metal swapchains do not support texture arrays (arraySize > 1)");
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    uint64_t handle = g_driver->AllocateHandle(HandleType::SWAPCHAIN);
    if (handle == 0) {
        spdlog::error("Failed to allocate swapchain handle from runtime driver");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *swapchain = reinterpret_cast<XrSwapchain>(handle);

    // Store swapchain data for later texture creation
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    SwapchainData data;
    data.width = createInfo->width;
    data.height = createInfo->height;
    data.format = createInfo->format;
#ifdef OX_VULKAN
    data.vkDevice = VK_NULL_HANDLE;          // Initialize Vulkan device
    data.vkPhysicalDevice = VK_NULL_HANDLE;  // Initialize Vulkan physical device
#endif

    // Determine graphics API from session and store relevant device handles
    auto graphicsIt = g_session_graphics.find(session);
    if (graphicsIt != g_session_graphics.end()) {
        data.graphicsAPI = graphicsIt->second.graphicsAPI;

        switch (data.graphicsAPI) {
#ifdef OX_OPENGL
            case GraphicsAPI::OpenGL:
                // OpenGL doesn't need additional data storage here
                break;
#endif
#ifdef OX_VULKAN
            case GraphicsAPI::Vulkan: {
                vulkan::VulkanSwapchainData vkData;
                if (vulkan::InitializeSwapchainData(graphicsIt->second.bindingData, vkData)) {
                    data.vkDevice = vkData.device;
                    data.vkPhysicalDevice = vkData.physicalDevice;
                    data.vkQueue = vkData.queue;
                    data.vkCommandPool = vkData.commandPool;
                }
                break;
            }
#endif
#ifdef OX_METAL
            case GraphicsAPI::Metal:
                // Store command queue for texture creation
                data.metalCommandQueue = graphicsIt->second.bindingData;
                spdlog::debug("Stored Metal command queue for swapchain");
                break;
#endif
            default:
                spdlog::error("Unsupported graphics API for swapchain data");
                break;
        }
    }

    g_swapchains[*swapchain] = data;

    spdlog::debug("Swapchain created successfully");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain) {
    spdlog::debug("xrDestroySwapchain called");

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
        // Delete OpenGL textures if they were created
#ifdef OX_OPENGL
        if (!it->second.glTextureIds.empty()) {
            opengl::DestroyTextures(it->second.glTextureIds);
        }
#endif

        // Destroy Vulkan images if they were created
#ifdef OX_VULKAN
        if (it->second.graphicsAPI == GraphicsAPI::Vulkan) {
            vulkan::DestroyImages(it->second.vkImages, it->second.vkImageMemory, it->second.vkCommandPool,
                                  it->second.vkDevice);
        }
#endif

#ifdef OX_METAL
        // Destroy Metal textures if they were created
        if (it->second.graphicsAPI == GraphicsAPI::Metal && !it->second.metalTextures.empty()) {
            metal::DestroyTextures(it->second.metalTextures.data(),
                                   static_cast<uint32_t>(it->second.metalTextures.size()));
            it->second.metalTextures.clear();
        }
#endif

        g_swapchains.erase(it);
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                          uint32_t* imageCountOutput,
                                                          XrSwapchainImageBaseHeader* images) {
    spdlog::debug("xrEnumerateSwapchainImages called");
    const uint32_t numImages = 3;

    if (imageCountOutput) {
        *imageCountOutput = numImages;
    }

    if (imageCapacityInput == 0 || !images) {
        return XR_SUCCESS;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    auto it = g_swapchains.find(swapchain);

    if (it == g_swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }

    XrStructureType imageType = images[0].type;

    // Create resources based on API
    switch (it->second.graphicsAPI) {
#ifdef OX_OPENGL
        case GraphicsAPI::OpenGL:
            opengl::CreateTextures(it->second.glTextureIds, it->second.width, it->second.height, numImages);
            break;
#endif
#ifdef OX_VULKAN
        case GraphicsAPI::Vulkan:
            vulkan::CreateImages(it->second.vkImages, it->second.vkImageMemory, it->second.vkDevice,
                                 it->second.vkPhysicalDevice, it->second.width, it->second.height, it->second.format,
                                 numImages);
            break;
#endif
#ifdef OX_METAL
        case GraphicsAPI::Metal: {
            // Allocate space for texture pointers
            it->second.metalTextures.resize(numImages, nullptr);
            metal::CreateTextures(it->second.metalCommandQueue, it->second.width, it->second.height, it->second.format,
                                  numImages, it->second.metalTextures.data());
            break;
        }
#endif
        default:
            spdlog::error("Unsupported graphics API for swapchain creation");
            return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    }

    // Populate the image array using API-specific helper functions
    uint32_t count = std::min(imageCapacityInput, numImages);
    switch (it->second.graphicsAPI) {
#ifdef OX_OPENGL
        case GraphicsAPI::OpenGL:
            opengl::PopulateSwapchainImages(it->second.glTextureIds, count, imageType, images);
            break;
#endif
#ifdef OX_VULKAN
        case GraphicsAPI::Vulkan:
            vulkan::PopulateSwapchainImages(it->second.vkImages, count, imageType, images);
            break;
#endif
#ifdef OX_METAL
        case GraphicsAPI::Metal:
            metal::PopulateSwapchainImages(it->second.metalTextures, count, imageType, images);
            break;
#endif
        default:
            // For other graphics APIs, just set the base headers
            for (uint32_t i = 0; i < count; ++i) {
                images[i].type = imageType;
                images[i].next = nullptr;
            }
            break;
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                       const XrSwapchainImageAcquireInfo* acquireInfo,
                                                       uint32_t* index) {
    spdlog::debug("xrAcquireSwapchainImage called");
    if (index) {
        *index = 0;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    spdlog::debug("xrWaitSwapchainImage called");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                       const XrSwapchainImageReleaseInfo* releaseInfo) {
    spdlog::debug("xrReleaseSwapchainImage called");
    return XR_SUCCESS;
}

// Path/string functions
XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString, XrPath* path) {
    // spdlog::debug("xrStringToPath called");
    if (!pathString || !path) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Note: Caller must hold g_instance_mutex
    // Check if we've already created this path
    std::string path_str(pathString);
    auto it = g_string_to_path.find(path_str);
    if (it != g_string_to_path.end()) {
        *path = it->second;
    } else {
        // Create new path using hash
        *path = static_cast<XrPath>(std::hash<std::string>{}(pathString));
        g_path_to_string[*path] = path_str;
        g_string_to_path[path_str] = *path;
    }

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t bufferCapacityInput,
                                              uint32_t* bufferCountOutput, char* buffer) {
    // spdlog::debug("xrPathToString called");

    // Note: Caller must hold g_instance_mutex
    // Look up the path string
    auto it = g_path_to_string.find(path);
    const char* str = (it != g_path_to_string.end()) ? it->second.c_str() : "/unknown/path";
    uint32_t len = strlen(str) + 1;

    if (bufferCountOutput) {
        *bufferCountOutput = len;
    }

    if (bufferCapacityInput > 0 && buffer) {
        safe_copy_string(buffer, bufferCapacityInput, str);
    }

    return XR_SUCCESS;
}

// Vive Tracker extension
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViveTrackerPathsHTCX(XrInstance instance, uint32_t pathCapacityInput,
                                                               uint32_t* pathCountOutput,
                                                               XrViveTrackerPathsHTCX* paths) {
    spdlog::debug("xrEnumerateViveTrackerPathsHTCX called");
    if (!pathCountOutput) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::lock_guard<std::mutex> lock(g_instance_mutex);

    // Build device map if not already built
    BuildDeviceMap();

    // Count trackers - look for devices with /user/vive_tracker_htcx paths
    std::vector<std::pair<std::string, int>> tracker_devices;
    for (const auto& [user_path, device_index] : g_device_path_to_index) {
        if (user_path.find("/user/vive_tracker_htcx/role/") == 0) {
            tracker_devices.push_back({user_path, device_index});
        }
    }

    uint32_t tracker_count = static_cast<uint32_t>(tracker_devices.size());
    *pathCountOutput = tracker_count;

    if (pathCapacityInput == 0 || !paths) {
        return XR_SUCCESS;
    }

    // Fill in tracker paths
    uint32_t count = std::min(pathCapacityInput, tracker_count);
    for (uint32_t i = 0; i < count; i++) {
        paths[i].type = static_cast<XrStructureType>(1000103000);  // XR_TYPE_VIVE_TRACKER_PATHS_HTCX
        paths[i].next = nullptr;

        // Convert user path string to XrPath
        const std::string& user_path = tracker_devices[i].first;
        xrStringToPath(instance, user_path.c_str(), &paths[i].persistentPath);
        paths[i].rolePath = paths[i].persistentPath;  // For simplicity, use same path
    }

    return XR_SUCCESS;
}

// Function map initialization
static void InitializeFunctionMap() {
    g_clientFunctionMap["xrEnumerateApiLayerProperties"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateApiLayerProperties);
    g_clientFunctionMap["xrEnumerateInstanceExtensionProperties"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateInstanceExtensionProperties);
    g_clientFunctionMap["xrCreateInstance"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateInstance);
    g_clientFunctionMap["xrDestroyInstance"] = reinterpret_cast<PFN_xrVoidFunction>(xrDestroyInstance);
    g_clientFunctionMap["xrGetInstanceProperties"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetInstanceProperties);
    g_clientFunctionMap["xrPollEvent"] = reinterpret_cast<PFN_xrVoidFunction>(xrPollEvent);
    g_clientFunctionMap["xrResultToString"] = reinterpret_cast<PFN_xrVoidFunction>(xrResultToString);
    g_clientFunctionMap["xrStructureTypeToString"] = reinterpret_cast<PFN_xrVoidFunction>(xrStructureTypeToString);
    g_clientFunctionMap["xrGetSystem"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetSystem);
    g_clientFunctionMap["xrGetSystemProperties"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetSystemProperties);
    g_clientFunctionMap["xrEnumerateViewConfigurations"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateViewConfigurations);
    g_clientFunctionMap["xrGetViewConfigurationProperties"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrGetViewConfigurationProperties);
    g_clientFunctionMap["xrEnumerateViewConfigurationViews"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateViewConfigurationViews);
    g_clientFunctionMap["xrEnumerateEnvironmentBlendModes"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateEnvironmentBlendModes);
    g_clientFunctionMap["xrCreateSession"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateSession);
    g_clientFunctionMap["xrDestroySession"] = reinterpret_cast<PFN_xrVoidFunction>(xrDestroySession);
    g_clientFunctionMap["xrBeginSession"] = reinterpret_cast<PFN_xrVoidFunction>(xrBeginSession);
    g_clientFunctionMap["xrEndSession"] = reinterpret_cast<PFN_xrVoidFunction>(xrEndSession);
    g_clientFunctionMap["xrRequestExitSession"] = reinterpret_cast<PFN_xrVoidFunction>(xrRequestExitSession);
    g_clientFunctionMap["xrEnumerateReferenceSpaces"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateReferenceSpaces);
    g_clientFunctionMap["xrCreateReferenceSpace"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateReferenceSpace);
    g_clientFunctionMap["xrDestroySpace"] = reinterpret_cast<PFN_xrVoidFunction>(xrDestroySpace);
    g_clientFunctionMap["xrLocateSpace"] = reinterpret_cast<PFN_xrVoidFunction>(xrLocateSpace);
    g_clientFunctionMap["xrLocateSpaces"] = reinterpret_cast<PFN_xrVoidFunction>(xrLocateSpaces);
    g_clientFunctionMap["xrWaitFrame"] = reinterpret_cast<PFN_xrVoidFunction>(xrWaitFrame);
    g_clientFunctionMap["xrBeginFrame"] = reinterpret_cast<PFN_xrVoidFunction>(xrBeginFrame);
    g_clientFunctionMap["xrEndFrame"] = reinterpret_cast<PFN_xrVoidFunction>(xrEndFrame);
    g_clientFunctionMap["xrLocateViews"] = reinterpret_cast<PFN_xrVoidFunction>(xrLocateViews);
    g_clientFunctionMap["xrCreateActionSet"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateActionSet);
    g_clientFunctionMap["xrDestroyActionSet"] = reinterpret_cast<PFN_xrVoidFunction>(xrDestroyActionSet);
    g_clientFunctionMap["xrCreateAction"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateAction);
    g_clientFunctionMap["xrDestroyAction"] = reinterpret_cast<PFN_xrVoidFunction>(xrDestroyAction);
    g_clientFunctionMap["xrSuggestInteractionProfileBindings"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrSuggestInteractionProfileBindings);
    g_clientFunctionMap["xrAttachSessionActionSets"] = reinterpret_cast<PFN_xrVoidFunction>(xrAttachSessionActionSets);
    g_clientFunctionMap["xrGetCurrentInteractionProfile"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrGetCurrentInteractionProfile);
    g_clientFunctionMap["xrSyncActions"] = reinterpret_cast<PFN_xrVoidFunction>(xrSyncActions);
    g_clientFunctionMap["xrGetActionStateBoolean"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetActionStateBoolean);
    g_clientFunctionMap["xrGetActionStateFloat"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetActionStateFloat);
    g_clientFunctionMap["xrGetActionStateVector2f"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetActionStateVector2f);
    g_clientFunctionMap["xrGetActionStatePose"] = reinterpret_cast<PFN_xrVoidFunction>(xrGetActionStatePose);
    g_clientFunctionMap["xrCreateActionSpace"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateActionSpace);
    g_clientFunctionMap["xrGetReferenceSpaceBoundsRect"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrGetReferenceSpaceBoundsRect);
    g_clientFunctionMap["xrEnumerateBoundSourcesForAction"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateBoundSourcesForAction);
    g_clientFunctionMap["xrGetInputSourceLocalizedName"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrGetInputSourceLocalizedName);
    g_clientFunctionMap["xrApplyHapticFeedback"] = reinterpret_cast<PFN_xrVoidFunction>(xrApplyHapticFeedback);
    g_clientFunctionMap["xrStopHapticFeedback"] = reinterpret_cast<PFN_xrVoidFunction>(xrStopHapticFeedback);
    g_clientFunctionMap["xrEnumerateSwapchainFormats"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateSwapchainFormats);
    g_clientFunctionMap["xrCreateSwapchain"] = reinterpret_cast<PFN_xrVoidFunction>(xrCreateSwapchain);
    g_clientFunctionMap["xrDestroySwapchain"] = reinterpret_cast<PFN_xrVoidFunction>(xrDestroySwapchain);
    g_clientFunctionMap["xrEnumerateSwapchainImages"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateSwapchainImages);
    g_clientFunctionMap["xrAcquireSwapchainImage"] = reinterpret_cast<PFN_xrVoidFunction>(xrAcquireSwapchainImage);
    g_clientFunctionMap["xrWaitSwapchainImage"] = reinterpret_cast<PFN_xrVoidFunction>(xrWaitSwapchainImage);
    g_clientFunctionMap["xrReleaseSwapchainImage"] = reinterpret_cast<PFN_xrVoidFunction>(xrReleaseSwapchainImage);
    g_clientFunctionMap["xrStringToPath"] = reinterpret_cast<PFN_xrVoidFunction>(xrStringToPath);
    g_clientFunctionMap["xrPathToString"] = reinterpret_cast<PFN_xrVoidFunction>(xrPathToString);
#ifdef OX_OPENGL
    g_clientFunctionMap["xrGetOpenGLGraphicsRequirementsKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(opengl::xrGetOpenGLGraphicsRequirementsKHR);
#endif
#ifdef OX_VULKAN
    g_clientFunctionMap["xrGetVulkanGraphicsRequirementsKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrGetVulkanGraphicsRequirementsKHR);
    g_clientFunctionMap["xrGetVulkanGraphicsRequirements2KHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrGetVulkanGraphicsRequirements2KHR);
    g_clientFunctionMap["xrGetVulkanInstanceExtensionsKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrGetVulkanInstanceExtensionsKHR);
    g_clientFunctionMap["xrGetVulkanDeviceExtensionsKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrGetVulkanDeviceExtensionsKHR);
    g_clientFunctionMap["xrGetVulkanGraphicsDeviceKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrGetVulkanGraphicsDeviceKHR);
    g_clientFunctionMap["xrGetVulkanGraphicsDevice2KHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrGetVulkanGraphicsDevice2KHR);
    g_clientFunctionMap["xrCreateVulkanInstanceKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrCreateVulkanInstanceKHR);
    g_clientFunctionMap["xrCreateVulkanDeviceKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(vulkan::xrCreateVulkanDeviceKHR);
#endif
#ifdef OX_METAL
    g_clientFunctionMap["xrGetMetalGraphicsRequirementsKHR"] =
        reinterpret_cast<PFN_xrVoidFunction>(metal::xrGetMetalGraphicsRequirementsKHR);
#endif
    g_clientFunctionMap["xrEnumerateViveTrackerPathsHTCX"] =
        reinterpret_cast<PFN_xrVoidFunction>(xrEnumerateViveTrackerPathsHTCX);
}

// xrGetInstanceProcAddr
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                     PFN_xrVoidFunction* function) {
    spdlog::debug("xrGetInstanceProcAddr called");
    if (!name || !function) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    spdlog::debug(("xrGetInstanceProcAddr called for: " + std::string(name)).c_str());

    if (g_clientFunctionMap.empty()) {
        InitializeFunctionMap();
    }

    auto it = g_clientFunctionMap.find(name);
    if (it != g_clientFunctionMap.end()) {
        *function = it->second;
        return XR_SUCCESS;
    }

    spdlog::debug(("xrGetInstanceProcAddr: Function NOT FOUND: " + std::string(name)).c_str());
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// Negotiation function
extern "C" RUNTIME_EXPORT XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest) {
    spdlog::debug("xrNegotiateLoaderRuntimeInterface called");
    if (!loaderInfo || !runtimeRequest) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
    runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr;

    return XR_SUCCESS;
}
