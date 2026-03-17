#pragma once

#include <openxr/openxr.h>
#include <ox_driver.h>

#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

#include "runtime_types.h"

namespace ox {
namespace runtime {

class IDriverConnection {
   public:
    virtual ~IDriverConnection() = default;

    virtual bool Connect() = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;
    virtual SharedData* GetSharedData() = 0;
    virtual bool SendRequest(MessageType type, const void* payload = nullptr, uint32_t payload_size = 0) = 0;
    virtual uint64_t AllocateHandle(HandleType type) = 0;
    virtual bool GetNextEvent(SessionStateEvent& event) = 0;
    virtual const RuntimePropertiesResponse& GetRuntimeProperties() const = 0;
    virtual const SystemPropertiesResponse& GetSystemProperties() const = 0;
    virtual const ViewConfigurationsResponse& GetViewConfigurations() const = 0;
    virtual const InteractionProfilesResponse& GetInteractionProfiles() const = 0;
    virtual bool GetInputStateBoolean(const char* user_path, const char* component_path, int64_t predicted_time,
                                      XrBool32& out_value) = 0;
    virtual bool GetInputStateFloat(const char* user_path, const char* component_path, int64_t predicted_time,
                                    float& out_value) = 0;
    virtual bool GetInputStateVector2f(const char* user_path, const char* component_path, int64_t predicted_time,
                                       XrVector2f& out_value) = 0;
    virtual void SubmitFramePixels(uint32_t eye_index, uint32_t width, uint32_t height, uint32_t format,
                                   const void* pixel_data, uint32_t data_size) = 0;
    virtual void SetSessionState(XrSessionState state) = 0;
};

class DriverConnection : public IDriverConnection {
   public:
    static DriverConnection& Instance();

    bool Connect() override;
    void Disconnect() override;
    bool IsConnected() const override;
    SharedData* GetSharedData() override;
    bool SendRequest(MessageType type, const void* payload = nullptr, uint32_t payload_size = 0) override;
    uint64_t AllocateHandle(HandleType type) override;
    bool GetNextEvent(SessionStateEvent& event) override;
    const RuntimePropertiesResponse& GetRuntimeProperties() const override;
    const SystemPropertiesResponse& GetSystemProperties() const override;
    const ViewConfigurationsResponse& GetViewConfigurations() const override;
    const InteractionProfilesResponse& GetInteractionProfiles() const override;
    bool GetInputStateBoolean(const char* user_path, const char* component_path, int64_t predicted_time,
                              XrBool32& out_value) override;
    bool GetInputStateFloat(const char* user_path, const char* component_path, int64_t predicted_time,
                            float& out_value) override;
    bool GetInputStateVector2f(const char* user_path, const char* component_path, int64_t predicted_time,
                               XrVector2f& out_value) override;
    void SubmitFramePixels(uint32_t eye_index, uint32_t width, uint32_t height, uint32_t format, const void* pixel_data,
                           uint32_t data_size) override;
    void SetSessionState(XrSessionState state) override;

   private:
    DriverConnection();
    ~DriverConnection() override;
    DriverConnection(const DriverConnection&) = delete;
    DriverConnection& operator=(const DriverConnection&) = delete;

    bool LoadDriver();
    bool LoadDriverLibrary(const std::string& library_path);
    void UnloadDriverUnlocked();
    void RefreshMetadata();
    void RefreshFrameState(int64_t predicted_time);
    void QueueSessionStateEvent(XrSessionState state);

    mutable std::mutex mutex_;
    void* driver_handle_;
    OxDriverCallbacks callbacks_;
    bool connected_;
    SharedData shared_data_;
    RuntimePropertiesResponse runtime_props_;
    SystemPropertiesResponse system_props_;
    ViewConfigurationsResponse view_configs_;
    InteractionProfilesResponse interaction_profiles_;
    std::queue<SessionStateEvent> pending_events_;
    uint64_t next_handle_;
};

}  // namespace runtime
}  // namespace ox