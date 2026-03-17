#pragma once

#include <openxr/openxr.h>
#include <ox_driver.h>

#include <atomic>
#include <cstdint>

namespace ox {
namespace runtime {

enum class MessageType : uint32_t {
    CREATE_SESSION = 1,
    DESTROY_SESSION = 2,
    REQUEST_EXIT_SESSION = 3,
};

enum class HandleType : uint32_t {
    INSTANCE = 1,
    SESSION = 2,
    SPACE = 3,
    ACTION_SET = 4,
    ACTION = 5,
    SWAPCHAIN = 6,
};

struct RequestExitSessionRequest {
    uint64_t session_handle;
};

struct SessionStateEvent {
    uint64_t session_handle;
    XrSessionState state;
    uint64_t timestamp;
};

struct RuntimePropertiesResponse {
    char runtime_name[128];
    uint32_t runtime_version_major;
    uint32_t runtime_version_minor;
    uint32_t runtime_version_patch;
    uint32_t padding;
};

struct SystemPropertiesResponse {
    char system_name[256];
    uint32_t max_swapchain_width;
    uint32_t max_swapchain_height;
    uint32_t max_layer_count;
    uint32_t orientation_tracking;
    uint32_t position_tracking;
    uint32_t padding[2];
};

struct ViewConfigurationsResponse {
    struct ViewConfig {
        uint32_t recommended_width;
        uint32_t recommended_height;
        uint32_t recommended_sample_count;
        uint32_t max_sample_count;
    } views[2];
};

struct InteractionProfilesResponse {
    uint32_t profile_count;
    char profiles[8][128];
};

struct alignas(64) Pose {
    XrPosef pose;
    uint64_t timestamp;
    std::atomic<uint32_t> flags;
    uint32_t padding[3];
};

constexpr uint32_t MAX_TRACKED_DEVICES = OX_MAX_DEVICES;

struct DevicePose {
    char user_path[256];
    Pose pose;
    uint32_t is_active;
    uint32_t padding;
};

struct View {
    Pose pose;
    float fov[4];
};

struct alignas(64) FrameState {
    std::atomic<uint64_t> frame_id;
    std::atomic<uint64_t> predicted_display_time;
    std::atomic<uint32_t> view_count;
    std::atomic<uint32_t> flags;
    View views[2];
    std::atomic<uint32_t> device_count;
    uint32_t padding1;
    DevicePose device_poses[MAX_TRACKED_DEVICES];
};

struct SharedData {
    std::atomic<uint32_t> session_state;
    std::atomic<uint64_t> active_session_handle;
    FrameState frame_state;
};

}  // namespace runtime
}  // namespace ox