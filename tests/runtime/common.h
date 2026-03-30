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

extern "C" void ox_set_driver(const OxDriverCallbacks* callbacks);

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
        last_session_state = XR_SESSION_STATE_UNKNOWN;

        std::memset(&system_properties, 0, sizeof(system_properties));
        system_properties.type = XR_TYPE_SYSTEM_PROPERTIES;
        system_properties.vendorId = 0x1234;
        std::snprintf(system_properties.systemName, XR_MAX_SYSTEM_NAME_SIZE, "%s", "Mock VR System");
        system_properties.graphicsProperties.maxSwapchainImageWidth = 2048;
        system_properties.graphicsProperties.maxSwapchainImageHeight = 2048;
        system_properties.graphicsProperties.maxLayerCount = 16;
        system_properties.trackingProperties.orientationTracking = XR_TRUE;
        system_properties.trackingProperties.positionTracking = XR_TRUE;

        view_poses[0] = {{0.0f, 0.0f, 0.0f, 1.0f}, {-0.032f, 1.6f, 0.0f}};
        view_poses[1] = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.032f, 1.6f, 0.0f}};
        device_count = 0;
    }

    OxDriverCallbacks BuildCallbacks() const {
        OxDriverCallbacks callbacks{};
        callbacks.initialize = &Initialize;
        callbacks.shutdown = &Shutdown;
        callbacks.is_device_connected = &IsDeviceConnected;
        callbacks.get_system_properties = &GetSystemProperties;
        callbacks.update_view = &UpdateView;
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
    XrSystemProperties system_properties{};
    std::array<XrPosef, 2> view_poses{};
    std::array<OxDeviceState, OX_MAX_DEVICES> devices{};
    uint32_t device_count = 0;
    std::vector<std::string> interaction_profiles;
    std::unordered_map<std::string, XrBool32> bool_inputs;
    std::unordered_map<std::string, float> float_inputs;
    std::unordered_map<std::string, XrVector2f> vector2_inputs;
    std::vector<SubmittedFrame> submitted_frames;
    XrSessionState last_session_state = XR_SESSION_STATE_UNKNOWN;

    static inline FakeDriverState* active = nullptr;

   private:
    static std::string InputKey(const char* user_path, const char* component_path) {
        return std::string(user_path ? user_path : "") + "|" + std::string(component_path ? component_path : "");
    }

    static int Initialize() { return active && active->connected ? 1 : 0; }
    static void Shutdown() {}
    static int IsDeviceConnected() { return active && active->connected ? 1 : 0; }

    static void GetSystemProperties(XrSystemProperties* props) {
        if (active && props) {
            void* next = props->next;
            *props = active->system_properties;
            props->next = next;
        }
    }

    static void UpdateView(XrTime predicted_time, uint32_t eye_index, XrView* out_view) {
        if (active && out_view && eye_index < active->view_poses.size()) {
            out_view->pose = active->view_poses[eye_index];
            out_view->fov = {-0.785398f, 0.785398f, 0.785398f, -0.785398f};
        }
    }

    static void UpdateDevices(XrTime predicted_time, OxDeviceState* out_states, uint32_t* out_count) {
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

    static XrResult GetInputStateBoolean(XrTime predicted_time, const char* user_path, const char* component_path,
                                         XrBool32* out_value) {
        if (!active || !out_value) {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        const auto it = active->bool_inputs.find(InputKey(user_path, component_path));
        if (it == active->bool_inputs.end()) {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        *out_value = it->second ? XR_TRUE : XR_FALSE;
        return XR_SUCCESS;
    }

    static XrResult GetInputStateFloat(XrTime predicted_time, const char* user_path, const char* component_path,
                                       float* out_value) {
        if (!active || !out_value) {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        const auto it = active->float_inputs.find(InputKey(user_path, component_path));
        if (it == active->float_inputs.end()) {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        *out_value = it->second;
        return XR_SUCCESS;
    }

    static XrResult GetInputStateVector2f(XrTime predicted_time, const char* user_path, const char* component_path,
                                          XrVector2f* out_value) {
        if (!active || !out_value) {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        const auto it = active->vector2_inputs.find(InputKey(user_path, component_path));
        if (it == active->vector2_inputs.end()) {
            return XR_ERROR_PATH_UNSUPPORTED;
        }

        *out_value = it->second;
        return XR_SUCCESS;
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

    static void OnSessionStateChanged(XrSessionState new_state) {
        if (active) {
            active->last_session_state = new_state;
        }
    }

    static void SubmitFramePixels(XrTime frame_time, uint32_t eye_index, uint32_t width, uint32_t height,
                                  uint32_t format, const void* pixel_data, uint32_t data_size) {
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
        ox_set_driver(&callbacks_);
    }

    void TearDown() override {
        for (XrInstance instance : created_instances_) {
            xrDestroyInstance(instance);
        }
        created_instances_.clear();
        ox_set_driver(nullptr);
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
