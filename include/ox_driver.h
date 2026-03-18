#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OX_DRIVER_API_VERSION 1

// Platform-specific export macro
#ifdef _WIN32
#define OX_DRIVER_EXPORT __declspec(dllexport)
#else
#define OX_DRIVER_EXPORT __attribute__((visibility("default")))
#endif

// Forward declarations
typedef struct OxDriverCallbacks OxDriverCallbacks;

// 2D position vector (for input components)
typedef struct {
    float x, y;
} OxVector2f;

// 3D position vector
typedef struct {
    float x, y, z;
} OxVector3f;

// Quaternion for orientation
typedef struct {
    float x, y, z, w;
} OxQuaternion;

// 6DOF pose (position + orientation)
typedef struct {
    OxVector3f position;
    OxQuaternion orientation;
} OxPose;

// Field of view (radians)
typedef struct {
    float angle_left;
    float angle_right;
    float angle_up;
    float angle_down;
} OxFov;

// Device information
typedef struct {
    char name[256];          // e.g., "Dummy VR Headset"
    char manufacturer[256];  // e.g., "ox runtime"
    char serial[256];        // e.g., "DUMMY-12345"
    uint32_t vendor_id;
    uint32_t product_id;
} OxDeviceInfo;

// Display capabilities
typedef struct {
    uint32_t display_width;       // Per-eye width in pixels
    uint32_t display_height;      // Per-eye height in pixels
    uint32_t recommended_width;   // Recommended render target width
    uint32_t recommended_height;  // Recommended render target height
    float refresh_rate;           // Hz
    OxFov fov;                    // Field of view
} OxDisplayProperties;

// Tracking capabilities
typedef struct {
    uint32_t has_position_tracking;
    uint32_t has_orientation_tracking;
} OxTrackingCapabilities;

// Device information (controllers, trackers, etc.)
#define OX_MAX_DEVICES 16

typedef struct {
    char user_path[256];  // OpenXR user path: "/user/hand/left", "/user/vive_tracker_htcx/role/waist", etc.
    OxPose pose;
    uint32_t is_active;  // 1 if device is connected/tracked, 0 otherwise
} OxDeviceState;

// Component state result codes
typedef enum {
    OX_COMPONENT_UNAVAILABLE = 0,  // Component doesn't exist on this controller
    OX_COMPONENT_AVAILABLE = 1,    // Component exists and state is valid
} OxComponentResult;

// Session states
typedef enum {
    OX_SESSION_STATE_UNKNOWN = 0,       // No session / undefined
    OX_SESSION_STATE_IDLE = 1,          // Session is idle (no app running or app hasn't started)
    OX_SESSION_STATE_READY = 2,         // Runtime is ready; app should call xrBeginSession
    OX_SESSION_STATE_SYNCHRONIZED = 3,  // Session is synchronized but not yet visible
    OX_SESSION_STATE_VISIBLE = 4,       // App frames are visible to the user
    OX_SESSION_STATE_FOCUSED = 5,       // App has input focus (fully active)
    OX_SESSION_STATE_STOPPING = 6,      // App should call xrEndSession soon
    OX_SESSION_STATE_LOSS_PENDING = 7,  // Runtime is no longer able to operate with the current session, for example
                                        // due to the loss of a display hardware connection
    OX_SESSION_STATE_EXITING = 8,       // App should call xrDestroySession / exit
} OxSessionState;

// Driver callbacks - implement these in your driver
struct OxDriverCallbacks {
    // ========== Lifecycle ==========

    // Called once when driver is loaded
    // Return: 1 on success, 0 on failure
    int (*initialize)(void);

    // Called when runtime shuts down
    void (*shutdown)(void);

    // Return: 1 if the driver is still running, 0 to signal the host to unload the driver.
    // Optional - Defaults to 1 (i.e. running), if this callback is not provided.
    int (*is_driver_running)(void);

    // ========== Device Discovery ==========

    // Check if physical device is connected and ready
    // Return: 1 if connected, 0 if not
    int (*is_device_connected)(void);

    // Get device information (name, manufacturer, serial, etc.)
    void (*get_device_info)(OxDeviceInfo* info);

    // ========== Display Properties ==========

    // Get display specifications
    void (*get_display_properties)(OxDisplayProperties* props);

    // Get tracking capabilities
    void (*get_tracking_capabilities)(OxTrackingCapabilities* caps);

    // ========== Hot Path - Called Every Frame ==========

    // Update per-eye view poses for rendering
    // predicted_time: nanoseconds since epoch
    // eye_index: 0 = left, 1 = right
    // out_pose: write the eye pose here (typically HMD pose + IPD offset)
    // Note: HMD tracking pose should be reported via update_devices() as device[0] with user_path="/user/head"
    void (*update_view_pose)(int64_t predicted_time, uint32_t eye_index, OxPose* out_pose);

    // ========== Devices (Controllers, Trackers, etc.) ==========

    // Update all tracked devices (controllers, trackers, etc.)
    // predicted_time: nanoseconds since epoch
    // out_states: array to fill with device states (must have space for OX_MAX_DEVICES)
    // out_count: write the number of devices here (must be <= OX_MAX_DEVICES)
    // This callback is optional - set to NULL if no tracked devices are supported
    void (*update_devices)(int64_t predicted_time, OxDeviceState* out_states, uint32_t* out_count);

    // Get boolean input state (for /click, /touch components)
    // predicted_time: nanoseconds since epoch
    // user_path: OpenXR user path (e.g., "/user/hand/left")
    // component_path: OpenXR component path (e.g., "/input/trigger/click", "/input/a/touch")
    // out_value: write the boolean value here (0 or 1)
    // Returns: OX_COMPONENT_AVAILABLE if component exists, OX_COMPONENT_UNAVAILABLE otherwise
    // This callback is optional - set to NULL if devices are not supported
    OxComponentResult (*get_input_state_boolean)(int64_t predicted_time, const char* user_path,
                                                 const char* component_path, uint32_t* out_value);

    // Get float input state (for /value, /force components)
    // predicted_time: nanoseconds since epoch
    // user_path: OpenXR user path (e.g., "/user/hand/left")
    // component_path: OpenXR component path (e.g., "/input/trigger/value", "/input/squeeze/value")
    // out_value: write the float value here (typically 0.0 to 1.0)
    // Returns: OX_COMPONENT_AVAILABLE if component exists, OX_COMPONENT_UNAVAILABLE otherwise
    // This callback is optional - set to NULL if devices are not supported
    OxComponentResult (*get_input_state_float)(int64_t predicted_time, const char* user_path,
                                               const char* component_path, float* out_value);

    // Get Vector2f input state (for thumbstick/trackpad)
    // predicted_time: nanoseconds since epoch
    // user_path: OpenXR user path (e.g., "/user/hand/left")
    // component_path: OpenXR component path (e.g., "/input/thumbstick", "/input/trackpad")
    // out_value: write the Vector2f value here ({x, y})
    // Returns: OX_COMPONENT_AVAILABLE if component exists, OX_COMPONENT_UNAVAILABLE otherwise
    // This callback is optional - set to NULL if devices are not supported
    OxComponentResult (*get_input_state_vector2f)(int64_t predicted_time, const char* user_path,
                                                  const char* component_path, OxVector2f* out_value);

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
    void (*on_session_state_changed)(OxSessionState new_state);

    // ========== Frame Submission (Optional) ==========

    // Called when a frame is submitted via xrEndFrame
    // eye_index: 0 = left, 1 = right
    // width, height: dimensions of the texture
    // format: Graphics API-specific format (e.g., GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM)
    // pixel_data: Pointer to raw RGBA pixel data in shared memory (width * height * 4 bytes)
    // data_size: Size of pixel_data in bytes
    // This callback is optional - set to NULL if the driver doesn't support frame submission
    // The driver can use this data directly (zero-copy) - DO NOT free this memory
    // The pixel_data pointer remains valid until the next frame submission
    void (*submit_frame_pixels)(uint32_t eye_index, uint32_t width, uint32_t height, uint32_t format,
                                const void* pixel_data, uint32_t data_size);
};

// Every driver MUST export this function
// The runtime calls this to register the driver's callbacks
// callbacks: pointer to struct that runtime has allocated
// Return: 1 on success, 0 on failure
typedef int (*OxDriverRegisterFunc)(OxDriverCallbacks* callbacks);

#ifdef __cplusplus
}
#endif
