#pragma once

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OX_MAKE_VERSION(major, minor) (((major) << 16) | (minor))
#define OX_DRIVER_API_VERSION OX_MAKE_VERSION(1, 0)

// Platform-specific export macro
#ifdef _WIN32
#define OX_DRIVER_EXPORT __declspec(dllexport)
#else
#define OX_DRIVER_EXPORT __attribute__((visibility("default")))
#endif

// Device information (controllers, trackers, etc.)
#define OX_MAX_DEVICES 16

typedef struct {
    char user_path[256];  // OpenXR user path: "/user/hand/left", "/user/vive_tracker_htcx/role/waist", etc.
    XrPosef pose;
    XrBool32 is_active;  // XR_TRUE if device is connected/tracked, XR_FALSE otherwise
} OxDeviceState;

// Driver callbacks - implement these in your driver
typedef struct OxDriver {
    // ========== Lifecycle ==========

    // Called once when driver is loaded
    // Return: 1 on success, 0 on failure
    int (*initialize)(void);

    // Called when runtime shuts down
    void (*shutdown)(void);

    // Return: 1 if the driver is still running, 0 to signal the host to unload the driver.
    // Optional - Defaults to 1 (i.e. running), if this callback is not provided.
    int (*is_driver_running)(void);

    // Optional configuration setters invoked by the runtime
    void (*set_config_bool)(const char* key, XrBool32 value);
    void (*set_config_string)(const char* key, const char* value);
    void (*set_config_int)(const char* key, int64_t value);
    void (*set_config_float)(const char* key, float value);

    // ========== Device Discovery ==========

    // Check if physical device is connected and ready
    // Return: 1 if connected, 0 if not
    int (*is_device_connected)(void);

    // Get system properties (name, vendor, graphics limits, tracking capabilities)
    // The runtime pre-initializes properties with {XR_TYPE_SYSTEM_PROPERTIES}.
    // Fill in: systemName, vendorId, graphicsProperties (maxSwapchainImageWidth/Height),
    // and trackingProperties. The runtime will set systemId and maxLayerCount.
    void (*get_system_properties)(XrSystemProperties* properties);

    // ========== Hot Path - Called Every Frame ==========

    // Update per-eye view for rendering
    // predicted_time: OpenXR predicted display time
    // eye_index: 0 = left, 1 = right
    // out_view: write the eye pose and field-of-view here
    // Note: HMD tracking pose should be reported via update_devices() as device[0] with user_path="/user/head"
    void (*update_view)(XrTime predicted_time, uint32_t eye_index, XrView* out_view);

    // ========== Devices (Controllers, Trackers, etc.) ==========

    // Update all tracked devices (controllers, trackers, etc.)
    // predicted_time: OpenXR predicted display time
    // out_states: array to fill with device states (must have space for OX_MAX_DEVICES)
    // out_count: write the number of devices here (must be <= OX_MAX_DEVICES)
    // This callback is optional - set to NULL if no tracked devices are supported
    void (*update_devices)(XrTime predicted_time, OxDeviceState* out_states, uint32_t* out_count);

    // Get boolean input state (for /click, /touch components)
    // predicted_time: OpenXR predicted display time
    // user_path: OpenXR user path (e.g., "/user/hand/left")
    // component_path: OpenXR component path (e.g., "/input/trigger/click", "/input/a/touch")
    // out_value: write the boolean value here (XR_TRUE or XR_FALSE)
    // Returns: XR_SUCCESS if component exists and value is valid, XR_ERROR_PATH_UNSUPPORTED otherwise
    // This callback is optional - set to NULL if devices are not supported
    XrResult (*get_input_state_bool)(XrTime predicted_time, const char* user_path, const char* component_path,
                                     XrBool32* out_value);

    // Get float input state (for /value, /force components)
    // predicted_time: OpenXR predicted display time
    // user_path: OpenXR user path (e.g., "/user/hand/left")
    // component_path: OpenXR component path (e.g., "/input/trigger/value", "/input/squeeze/value")
    // out_value: write the float value here (typically 0.0 to 1.0)
    // Returns: XR_SUCCESS if component exists and value is valid, XR_ERROR_PATH_UNSUPPORTED otherwise
    // This callback is optional - set to NULL if devices are not supported
    XrResult (*get_input_state_float)(XrTime predicted_time, const char* user_path, const char* component_path,
                                      float* out_value);

    // Get Vector2f input state (for thumbstick/trackpad)
    // predicted_time: OpenXR predicted display time
    // user_path: OpenXR user path (e.g., "/user/hand/left")
    // component_path: OpenXR component path (e.g., "/input/thumbstick", "/input/trackpad")
    // out_value: write the Vector2f value here ({x, y})
    // Returns: XR_SUCCESS if component exists and value is valid, XR_ERROR_PATH_UNSUPPORTED otherwise
    // This callback is optional - set to NULL if devices are not supported
    XrResult (*get_input_state_vector2f)(XrTime predicted_time, const char* user_path, const char* component_path,
                                         XrVector2f* out_value);

    // ========== Interaction Profiles (Optional) ==========

    // Get supported interaction profiles for controllers
    // profiles: array to fill with null-terminated profile path strings
    // max_profiles: size of the profiles array
    // Returns: number of supported profiles (may be > max_profiles)
    // Example profile: "/interaction_profiles/khr/simple_controller"
    // This callback is optional - if NULL, driver supports /interaction_profiles/khr/simple_controller by default
    uint32_t (*get_interaction_profiles)(const char** profiles, uint32_t max_profiles);

    // ========== Session Lifecycle (Optional) ==========

    // Called whenever the OpenXR session state changes.
    // Delivers the same state progression an XR app sees via XrEventDataSessionStateChanged.
    // This callback is optional - set to NULL if not needed.
    void (*on_session_state_changed)(XrSessionState new_state);

    // ========== Frame Submission (Optional) ==========

    // Called when a frame is submitted via xrEndFrame
    // frame_time: XrFrameEndInfo::displayTime passed to xrEndFrame
    // eye_index: 0 = left, 1 = right
    // width, height: dimensions of the texture
    // format: Graphics API-specific format (e.g., GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM)
    // pixel_data: Pointer to raw RGBA pixel data in shared memory (width * height * 4 bytes)
    // data_size: Size of pixel_data in bytes
    // This callback is optional - set to NULL if the driver doesn't support frame submission
    // The driver can use this data directly (zero-copy) - DO NOT free this memory
    // The pixel_data pointer remains valid until the next frame submission
    void (*submit_frame_pixels)(XrTime frame_time, uint32_t eye_index, uint32_t width, uint32_t height, uint32_t format,
                                const void* pixel_data, uint32_t data_size);
} OxDriver;

// Every driver MUST export this function
// The runtime calls this to register the driver's callbacks
// driver: pointer to struct that runtime has allocated
// Return: 1 on success, 0 on failure
typedef int (*OxDriverRegisterFunc)(OxDriver* driver);

#ifdef __cplusplus
}
#endif
