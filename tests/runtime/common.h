#pragma once

#include <gtest/gtest.h>
#include <openxr/openxr.h>
#include <ox_driver.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void oxSetDriverCallbacks(const OxDriverCallbacks* callbacks);

namespace ox {
namespace test {

class FakeDriverState {
   public:
    FakeDriverState() { ResetDefaults(); }

    void ResetDefaults() {
        connected = true;
        interaction_profiles = {"/interaction_profiles/khr/simple_controller"};
        bool_inputs.clear();
        float_inputs.clear();
        vector2_inputs.clear();
        submitted_frames.clear();
        last_session_state = OX_SESSION_STATE_UNKNOWN;

        std::memset(&device_info, 0, sizeof(device_info));
        std::snprintf(device_info.name, sizeof(device_info.name), "%s", "Mock VR System");
        std::snprintf(device_info.manufacturer, sizeof(device_info.manufacturer), "%s", "ox-runtime tests");
        std::snprintf(device_info.serial, sizeof(device_info.serial), "%s", "TEST-0001");

        std::memset(&display_properties, 0, sizeof(display_properties));
        display_properties.display_width = 2048;
        display_properties.display_height = 2048;
        display_properties.recommended_width = 1832;
        display_properties.recommended_height = 1920;
        display_properties.refresh_rate = 90.0f;
        display_properties.fov = {-0.8f, 0.8f, 0.8f, -0.8f};

        tracking_capabilities = {1, 1};

        view_poses[0] = {{-0.032f, 1.6f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
        view_poses[1] = {{0.032f, 1.6f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
        device_count = 0;
    }

    OxDriverCallbacks BuildCallbacks() const {
        OxDriverCallbacks callbacks{};
        callbacks.initialize = &Initialize;
        callbacks.shutdown = &Shutdown;
        callbacks.is_device_connected = &IsDeviceConnected;
        callbacks.get_device_info = &GetDeviceInfo;
        callbacks.get_display_properties = &GetDisplayProperties;
        callbacks.get_tracking_capabilities = &GetTrackingCapabilities;
        callbacks.update_view_pose = &UpdateViewPose;
        callbacks.update_devices = &UpdateDevices;
        callbacks.get_input_state_boolean = &GetInputStateBoolean;
        callbacks.get_input_state_float = &GetInputStateFloat;
        callbacks.get_input_state_vector2f = &GetInputStateVector2f;
        callbacks.get_interaction_profiles = &GetInteractionProfiles;
        callbacks.on_session_state_changed = &OnSessionStateChanged;
        callbacks.submit_frame_pixels = &SubmitFramePixels;
        return callbacks;
    }

    struct SubmittedFrame {
        uint32_t eye_index;
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t data_size;
    };

    bool connected = true;
    OxDeviceInfo device_info{};
    OxDisplayProperties display_properties{};
    OxTrackingCapabilities tracking_capabilities{};
    std::array<OxPose, 2> view_poses{};
    std::array<OxDeviceState, OX_MAX_DEVICES> devices{};
    uint32_t device_count = 0;
    std::vector<std::string> interaction_profiles;
    std::unordered_map<std::string, uint32_t> bool_inputs;
    std::unordered_map<std::string, float> float_inputs;
    std::unordered_map<std::string, OxVector2f> vector2_inputs;
    std::vector<SubmittedFrame> submitted_frames;
    OxSessionState last_session_state = OX_SESSION_STATE_UNKNOWN;

    static inline FakeDriverState* active = nullptr;

   private:
    static std::string InputKey(const char* user_path, const char* component_path) {
        return std::string(user_path ? user_path : "") + "|" + std::string(component_path ? component_path : "");
    }

    static int Initialize() { return active && active->connected ? 1 : 0; }
    static void Shutdown() {}
    static int IsDeviceConnected() { return active && active->connected ? 1 : 0; }

    static void GetDeviceInfo(OxDeviceInfo* info) {
        if (active && info) {
            *info = active->device_info;
        }
    }

    static void GetDisplayProperties(OxDisplayProperties* props) {
        if (active && props) {
            *props = active->display_properties;
        }
    }

    static void GetTrackingCapabilities(OxTrackingCapabilities* caps) {
        if (active && caps) {
            *caps = active->tracking_capabilities;
        }
    }

    static void UpdateViewPose(int64_t predicted_time, uint32_t eye_index, OxPose* out_pose) {
        if (active && out_pose && eye_index < active->view_poses.size()) {
            *out_pose = active->view_poses[eye_index];
        }
    }

    static void UpdateDevices(int64_t predicted_time, OxDeviceState* out_states, uint32_t* out_count) {
        if (!active || !out_count) {
            return;
        }

        *out_count = active->device_count;
        if (!out_states) {
            return;
        }

        for (uint32_t index = 0; index < active->device_count && index < OX_MAX_DEVICES; ++index) {
            out_states[index] = active->devices[index];
        }
    }

    static OxComponentResult GetInputStateBoolean(int64_t predicted_time, const char* user_path,
                                                  const char* component_path, uint32_t* out_value) {
        if (!active || !out_value) {
            return OX_COMPONENT_UNAVAILABLE;
        }

        const auto it = active->bool_inputs.find(InputKey(user_path, component_path));
        if (it == active->bool_inputs.end()) {
            return OX_COMPONENT_UNAVAILABLE;
        }

        *out_value = it->second;
        return OX_COMPONENT_AVAILABLE;
    }

    static OxComponentResult GetInputStateFloat(int64_t predicted_time, const char* user_path,
                                                const char* component_path, float* out_value) {
        if (!active || !out_value) {
            return OX_COMPONENT_UNAVAILABLE;
        }

        const auto it = active->float_inputs.find(InputKey(user_path, component_path));
        if (it == active->float_inputs.end()) {
            return OX_COMPONENT_UNAVAILABLE;
        }

        *out_value = it->second;
        return OX_COMPONENT_AVAILABLE;
    }

    static OxComponentResult GetInputStateVector2f(int64_t predicted_time, const char* user_path,
                                                   const char* component_path, OxVector2f* out_value) {
        if (!active || !out_value) {
            return OX_COMPONENT_UNAVAILABLE;
        }

        const auto it = active->vector2_inputs.find(InputKey(user_path, component_path));
        if (it == active->vector2_inputs.end()) {
            return OX_COMPONENT_UNAVAILABLE;
        }

        *out_value = it->second;
        return OX_COMPONENT_AVAILABLE;
    }

    static uint32_t GetInteractionProfiles(const char** profiles, uint32_t max_profiles) {
        if (!active) {
            return 0;
        }

        const uint32_t count = static_cast<uint32_t>(active->interaction_profiles.size());
        for (uint32_t index = 0; index < count && index < max_profiles; ++index) {
            profiles[index] = active->interaction_profiles[index].c_str();
        }
        return count;
    }

    static void OnSessionStateChanged(OxSessionState new_state) {
        if (active) {
            active->last_session_state = new_state;
        }
    }

    static void SubmitFramePixels(uint32_t eye_index, uint32_t width, uint32_t height, uint32_t format,
                                  const void* pixel_data, uint32_t data_size) {
        if (active) {
            active->submitted_frames.push_back({eye_index, width, height, format, data_size});
        }
    }
};

// Base test fixture for runtime tests
class RuntimeTestBase : public ::testing::Test {
   protected:
    void SetUp() override {
        driver_state.ResetDefaults();
        FakeDriverState::active = &driver_state;
        callbacks_ = driver_state.BuildCallbacks();
        oxSetDriverCallbacks(&callbacks_);
    }

    void TearDown() override {
        for (XrInstance instance : created_instances_) {
            xrDestroyInstance(instance);
        }
        created_instances_.clear();
        oxSetDriverCallbacks(nullptr);
        FakeDriverState::active = nullptr;
    }

    // Helper to create a basic instance
    XrInstance CreateBasicInstance(const std::string& app_name = "TestApp") {
        XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        snprintf(create_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", app_name.c_str());
        create_info.applicationInfo.applicationVersion = 1;
        snprintf(create_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "TestEngine");
        create_info.applicationInfo.engineVersion = 1;
        create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

        XrInstance instance = XR_NULL_HANDLE;
        XrResult result = xrCreateInstance(&create_info, &instance);

        if (result == XR_SUCCESS && instance != XR_NULL_HANDLE) {
            created_instances_.push_back(instance);
        }

        return instance;
    }

    std::vector<XrInstance> created_instances_;
    FakeDriverState driver_state;
    OxDriverCallbacks callbacks_{};
};

}  // namespace test
}  // namespace ox
