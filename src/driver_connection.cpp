#include "driver_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace ox {
namespace runtime {

namespace {

int64_t NowNanos() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

void SafeCopy(char* dest, size_t dest_size, const char* src) {
    if (!dest || dest_size == 0) {
        return;
    }

    const char* value = src ? src : "";
    snprintf(dest, dest_size, "%s", value);
}

std::string DriverLibraryFilename() {
#ifdef _WIN32
    return "ox_driver.dll";
#elif defined(__APPLE__)
    return "libox_driver.dylib";
#else
    return "libox_driver.so";
#endif
}

std::string FrontendLibraryFilename() {
#ifdef _WIN32
    return "ox_ipc_frontend.dll";
#elif defined(__APPLE__)
    return "libox_ipc_frontend.dylib";
#else
    return "libox_ipc_frontend.so";
#endif
}

fs::path ModuleDirectory() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ModuleDirectory), &module) == 0) {
        return fs::current_path();
    }

    std::array<char, MAX_PATH> buffer{};
    const DWORD len = GetModuleFileNameA(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) {
        return fs::current_path();
    }

    return fs::path(std::string(buffer.data(), len)).parent_path();
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) == 0 || !info.dli_fname) {
        return fs::current_path();
    }

    return fs::path(info.dli_fname).parent_path();
#endif
}

fs::path NormalizeLibraryPath(const std::string& configured_path) {
    fs::path path(configured_path);
    if (path.is_relative()) {
        path = ModuleDirectory() / path;
    }
    return fs::absolute(path);
}

OxSessionState ToOxSessionState(XrSessionState state) {
    return static_cast<OxSessionState>(static_cast<uint32_t>(state));
}

}  // namespace

DriverConnection& DriverConnection::Instance() {
    static DriverConnection instance;
    return instance;
}

DriverConnection::DriverConnection() : driver_handle_(nullptr), callbacks_{}, connected_(false), next_handle_(1) {
    shared_data_.session_state.store(static_cast<uint32_t>(XR_SESSION_STATE_IDLE), std::memory_order_release);
    shared_data_.active_session_handle.store(0, std::memory_order_release);
    shared_data_.frame_state.frame_id.store(0, std::memory_order_release);
    shared_data_.frame_state.predicted_display_time.store(0, std::memory_order_release);
    shared_data_.frame_state.view_count.store(0, std::memory_order_release);
    shared_data_.frame_state.flags.store(0, std::memory_order_release);
    shared_data_.frame_state.device_count.store(0, std::memory_order_release);
}

DriverConnection::~DriverConnection() { Disconnect(); }

bool DriverConnection::Connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_) {
        return true;
    }

    return LoadDriver();
}

void DriverConnection::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    UnloadDriverUnlocked();
}

void DriverConnection::UnloadDriverUnlocked() {
    if (connected_ && callbacks_.shutdown) {
        callbacks_.shutdown();
    }

    if (driver_handle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(driver_handle_));
#else
        dlclose(driver_handle_);
#endif
        driver_handle_ = nullptr;
    }

    callbacks_ = {};
    connected_ = false;
    while (!pending_events_.empty()) {
        pending_events_.pop();
    }
}

bool DriverConnection::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

SharedData* DriverConnection::GetSharedData() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_) {
        RefreshFrameState(NowNanos());
    }
    return &shared_data_;
}

bool DriverConnection::SendRequest(MessageType type, const void* payload, uint32_t payload_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
        return false;
    }

    switch (type) {
        case MessageType::CREATE_SESSION: {
            const uint64_t handle = AllocateHandle(HandleType::SESSION);
            shared_data_.active_session_handle.store(handle, std::memory_order_release);
            SetSessionState(XR_SESSION_STATE_READY);
            return true;
        }
        case MessageType::DESTROY_SESSION:
            shared_data_.active_session_handle.store(0, std::memory_order_release);
            SetSessionState(XR_SESSION_STATE_IDLE);
            return true;
        case MessageType::REQUEST_EXIT_SESSION:
            if (payload && payload_size >= sizeof(RequestExitSessionRequest)) {
                const auto* request = static_cast<const RequestExitSessionRequest*>(payload);
                shared_data_.active_session_handle.store(request->session_handle, std::memory_order_release);
            }
            SetSessionState(XR_SESSION_STATE_EXITING);
            return true;
        default:
            return false;
    }
}

uint64_t DriverConnection::AllocateHandle(HandleType) { return next_handle_++; }

bool DriverConnection::GetNextEvent(SessionStateEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_events_.empty()) {
        return false;
    }

    event = pending_events_.front();
    pending_events_.pop();
    return true;
}

const RuntimePropertiesResponse& DriverConnection::GetRuntimeProperties() const { return runtime_props_; }

const SystemPropertiesResponse& DriverConnection::GetSystemProperties() const { return system_props_; }

const ViewConfigurationsResponse& DriverConnection::GetViewConfigurations() const { return view_configs_; }

const InteractionProfilesResponse& DriverConnection::GetInteractionProfiles() const { return interaction_profiles_; }

bool DriverConnection::GetInputStateBoolean(const char* user_path, const char* component_path, int64_t predicted_time,
                                            XrBool32& out_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !callbacks_.get_input_state_boolean) {
        return false;
    }

    uint32_t value = 0;
    if (callbacks_.get_input_state_boolean(predicted_time, user_path, component_path, &value) !=
        OX_COMPONENT_AVAILABLE) {
        return false;
    }

    out_value = value ? XR_TRUE : XR_FALSE;
    return true;
}

bool DriverConnection::GetInputStateFloat(const char* user_path, const char* component_path, int64_t predicted_time,
                                          float& out_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !callbacks_.get_input_state_float) {
        return false;
    }

    return callbacks_.get_input_state_float(predicted_time, user_path, component_path, &out_value) ==
           OX_COMPONENT_AVAILABLE;
}

bool DriverConnection::GetInputStateVector2f(const char* user_path, const char* component_path, int64_t predicted_time,
                                             XrVector2f& out_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || !callbacks_.get_input_state_vector2f) {
        return false;
    }

    OxVector2f value{};
    if (callbacks_.get_input_state_vector2f(predicted_time, user_path, component_path, &value) !=
        OX_COMPONENT_AVAILABLE) {
        return false;
    }

    out_value = {value.x, value.y};
    return true;
}

void DriverConnection::SubmitFramePixels(uint32_t eye_index, uint32_t width, uint32_t height, uint32_t format,
                                         const void* pixel_data, uint32_t data_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_ && callbacks_.submit_frame_pixels) {
        callbacks_.submit_frame_pixels(eye_index, width, height, format, pixel_data, data_size);
    }
}

void DriverConnection::SetSessionState(XrSessionState state) {
    shared_data_.session_state.store(static_cast<uint32_t>(state), std::memory_order_release);
    QueueSessionStateEvent(state);

    if (connected_ && callbacks_.on_session_state_changed) {
        callbacks_.on_session_state_changed(ToOxSessionState(state));
    }
}

bool DriverConnection::LoadDriver() {
    if (const char* configured_path = std::getenv("OX_RUNTIME_DRIVER")) {
        return LoadDriverLibrary(NormalizeLibraryPath(configured_path).string());
    }

    if (const char* use_simulator = std::getenv("OX_USE_SIMULATOR")) {
        if (std::string(use_simulator) == "1") {
            fs::path path = ModuleDirectory() / "drivers" / "ox-simulator" / DriverLibraryFilename();
            return LoadDriverLibrary(fs::absolute(path).string());
        }
    }

    fs::path fallback = ModuleDirectory() / FrontendLibraryFilename();
    return LoadDriverLibrary(fs::absolute(fallback).string());
}

bool DriverConnection::LoadDriverLibrary(const std::string& library_path) {
#ifdef _WIN32
    driver_handle_ = LoadLibraryA(library_path.c_str());
    if (!driver_handle_) {
        spdlog::error("Failed to load driver library: {}", library_path);
        return false;
    }

    auto register_func = reinterpret_cast<OxDriverRegisterFunc>(
        GetProcAddress(static_cast<HMODULE>(driver_handle_), "ox_driver_register"));
#else
    driver_handle_ = dlopen(library_path.c_str(), RTLD_NOW);
    if (!driver_handle_) {
        spdlog::error("Failed to load driver library: {}", dlerror());
        return false;
    }

    auto register_func = reinterpret_cast<OxDriverRegisterFunc>(dlsym(driver_handle_, "ox_driver_register"));
#endif

    if (!register_func) {
        spdlog::error("Failed to resolve ox_driver_register");
        UnloadDriverUnlocked();
        return false;
    }

    callbacks_ = {};
    if (!register_func(&callbacks_)) {
        spdlog::error("Driver registration failed");
        UnloadDriverUnlocked();
        return false;
    }

    if (!callbacks_.initialize || !callbacks_.is_device_connected || !callbacks_.get_device_info ||
        !callbacks_.get_display_properties || !callbacks_.get_tracking_capabilities || !callbacks_.update_view_pose) {
        spdlog::error("Driver missing required callbacks");
        UnloadDriverUnlocked();
        return false;
    }

    if (!callbacks_.initialize()) {
        spdlog::error("Driver initialize() failed");
        UnloadDriverUnlocked();
        return false;
    }

    if (!callbacks_.is_device_connected()) {
        spdlog::error("Driver reported no connected device");
        UnloadDriverUnlocked();
        return false;
    }

    connected_ = true;
    RefreshMetadata();
    RefreshFrameState(NowNanos());
    spdlog::info("Loaded runtime driver: {}", library_path);
    return true;
}

void DriverConnection::RefreshMetadata() {
    std::memset(&runtime_props_, 0, sizeof(runtime_props_));
    std::memset(&system_props_, 0, sizeof(system_props_));
    std::memset(&view_configs_, 0, sizeof(view_configs_));
    std::memset(&interaction_profiles_, 0, sizeof(interaction_profiles_));

    SafeCopy(runtime_props_.runtime_name, sizeof(runtime_props_.runtime_name), "ox-runtime");
#ifdef OX_VERSION_MAJOR
    runtime_props_.runtime_version_major = OX_VERSION_MAJOR;
    runtime_props_.runtime_version_minor = OX_VERSION_MINOR;
    runtime_props_.runtime_version_patch = OX_VERSION_PATCH;
#endif

    OxDeviceInfo device_info{};
    callbacks_.get_device_info(&device_info);
    SafeCopy(system_props_.system_name, sizeof(system_props_.system_name), device_info.name);

    OxDisplayProperties display_props{};
    callbacks_.get_display_properties(&display_props);
    system_props_.max_swapchain_width = display_props.display_width;
    system_props_.max_swapchain_height = display_props.display_height;
    system_props_.max_layer_count = 16;

    OxTrackingCapabilities tracking_caps{};
    callbacks_.get_tracking_capabilities(&tracking_caps);
    system_props_.orientation_tracking = tracking_caps.has_orientation_tracking;
    system_props_.position_tracking = tracking_caps.has_position_tracking;

    for (uint32_t index = 0; index < 2; ++index) {
        view_configs_.views[index].recommended_width = display_props.recommended_width;
        view_configs_.views[index].recommended_height = display_props.recommended_height;
        view_configs_.views[index].recommended_sample_count = 1;
        view_configs_.views[index].max_sample_count = 4;
    }

    if (callbacks_.get_interaction_profiles) {
        const char* profiles[8] = {};
        interaction_profiles_.profile_count = std::min<uint32_t>(callbacks_.get_interaction_profiles(profiles, 8), 8);
        for (uint32_t index = 0; index < interaction_profiles_.profile_count; ++index) {
            SafeCopy(interaction_profiles_.profiles[index], sizeof(interaction_profiles_.profiles[index]),
                     profiles[index]);
        }
    } else {
        interaction_profiles_.profile_count = 1;
        SafeCopy(interaction_profiles_.profiles[0], sizeof(interaction_profiles_.profiles[0]),
                 "/interaction_profiles/khr/simple_controller");
    }
}

void DriverConnection::RefreshFrameState(int64_t predicted_time) {
    OxDisplayProperties display_props{};
    callbacks_.get_display_properties(&display_props);

    shared_data_.frame_state.predicted_display_time.store(predicted_time, std::memory_order_release);
    shared_data_.frame_state.frame_id.fetch_add(1, std::memory_order_acq_rel);
    shared_data_.frame_state.view_count.store(2, std::memory_order_release);

    for (uint32_t eye_index = 0; eye_index < 2; ++eye_index) {
        OxPose pose{};
        callbacks_.update_view_pose(predicted_time, eye_index, &pose);
        auto& view = shared_data_.frame_state.views[eye_index];
        view.pose.pose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
        view.pose.pose.position = {pose.position.x, pose.position.y, pose.position.z};
        view.pose.timestamp = static_cast<uint64_t>(predicted_time);
        view.fov[0] = display_props.fov.angle_left;
        view.fov[1] = display_props.fov.angle_right;
        view.fov[2] = display_props.fov.angle_up;
        view.fov[3] = display_props.fov.angle_down;
    }

    uint32_t device_count = 0;
    std::array<OxDeviceState, OX_MAX_DEVICES> devices{};
    if (callbacks_.update_devices) {
        callbacks_.update_devices(predicted_time, devices.data(), &device_count);
    }

    device_count = std::min<uint32_t>(device_count, MAX_TRACKED_DEVICES);
    shared_data_.frame_state.device_count.store(device_count, std::memory_order_release);

    for (uint32_t index = 0; index < device_count; ++index) {
        auto& dest = shared_data_.frame_state.device_poses[index];
        SafeCopy(dest.user_path, sizeof(dest.user_path), devices[index].user_path);
        dest.pose.pose.orientation = {devices[index].pose.orientation.x, devices[index].pose.orientation.y,
                                      devices[index].pose.orientation.z, devices[index].pose.orientation.w};
        dest.pose.pose.position = {devices[index].pose.position.x, devices[index].pose.position.y,
                                   devices[index].pose.position.z};
        dest.pose.timestamp = static_cast<uint64_t>(predicted_time);
        dest.is_active = devices[index].is_active;
    }
}

void DriverConnection::QueueSessionStateEvent(XrSessionState state) {
    SessionStateEvent event{};
    event.session_handle = shared_data_.active_session_handle.load(std::memory_order_acquire);
    event.state = state;
    event.timestamp = static_cast<uint64_t>(NowNanos());
    pending_events_.push(event);
}

}  // namespace runtime
}  // namespace ox