#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/driver_connection.h"

// Declare the injection function
extern "C" void oxSetDriver(ox::runtime::IDriverConnection* driver);

namespace ox {
namespace test {

class MockDriverConnection : public runtime::IDriverConnection {
   public:
    MOCK_METHOD(bool, Connect, (), (override));
    MOCK_METHOD(void, Disconnect, (), (override));
    MOCK_METHOD(bool, IsConnected, (), (const, override));
    MOCK_METHOD(runtime::SharedData*, GetSharedData, (), (override));
    MOCK_METHOD(bool, SendRequest, (runtime::MessageType, const void*, uint32_t), (override));
    MOCK_METHOD(uint64_t, AllocateHandle, (runtime::HandleType), (override));
    MOCK_METHOD(bool, GetNextEvent, (runtime::SessionStateEvent&), (override));
    MOCK_METHOD(const runtime::RuntimePropertiesResponse&, GetRuntimeProperties, (), (const, override));
    MOCK_METHOD(const runtime::SystemPropertiesResponse&, GetSystemProperties, (), (const, override));
    MOCK_METHOD(const runtime::ViewConfigurationsResponse&, GetViewConfigurations, (), (const, override));
    MOCK_METHOD(const runtime::InteractionProfilesResponse&, GetInteractionProfiles, (), (const, override));
    MOCK_METHOD(bool, GetInputStateBoolean, (const char*, const char*, int64_t, XrBool32&), (override));
    MOCK_METHOD(bool, GetInputStateFloat, (const char*, const char*, int64_t, float&), (override));
    MOCK_METHOD(bool, GetInputStateVector2f, (const char*, const char*, int64_t, XrVector2f&), (override));
    MOCK_METHOD(void, SubmitFramePixels, (uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t), (override));
    MOCK_METHOD(void, SetSessionState, (XrSessionState), (override));

    static void SetupDefaultBehaviors(MockDriverConnection* mock) {
        // Initialize shared data
        shared_data_.session_state.store(static_cast<uint32_t>(XR_SESSION_STATE_READY));
        shared_data_.active_session_handle.store(1000);

        // Default connection behavior
        ON_CALL(*mock, Connect()).WillByDefault(testing::Return(true));
        ON_CALL(*mock, IsConnected()).WillByDefault(testing::Return(true));
        ON_CALL(*mock, GetSharedData()).WillByDefault(testing::Return(&shared_data_));
        ON_CALL(*mock, SendRequest(ox::runtime::MessageType::CREATE_SESSION, testing::_, testing::_))
            .WillByDefault(testing::DoAll(testing::Invoke([](ox::runtime::MessageType, const void*, uint32_t) {
                                              // Simulate service setting the session handle
                                              shared_data_.active_session_handle.store(1000);
                                          }),
                                          testing::Return(true)));
        ON_CALL(*mock, SendRequest(testing::_, testing::_, testing::_)).WillByDefault(testing::Return(true));
        ON_CALL(*mock, AllocateHandle(testing::_)).WillByDefault(testing::Return(1000));
        ON_CALL(*mock, GetNextEvent(testing::_)).WillByDefault(testing::Return(false));

        // Default runtime properties
        ON_CALL(*mock, GetRuntimeProperties()).WillByDefault(testing::ReturnRef(runtime_props_));

        // Default system properties
        ON_CALL(*mock, GetSystemProperties()).WillByDefault(testing::ReturnRef(system_props_));

        // Default view configurations
        ON_CALL(*mock, GetViewConfigurations()).WillByDefault(testing::ReturnRef(view_configs_));
        ON_CALL(*mock, GetViewConfigurations()).WillByDefault(testing::ReturnRef(view_configs_));

        // Default interaction profiles
        ON_CALL(*mock, GetInteractionProfiles()).WillByDefault(testing::ReturnRef(interaction_profiles_));

        // Default input state methods
        ON_CALL(*mock, GetInputStateBoolean(testing::_, testing::_, testing::_, testing::_))
            .WillByDefault(testing::DoAll(testing::SetArgReferee<3>(XR_FALSE), testing::Return(true)));
        ON_CALL(*mock, GetInputStateFloat(testing::_, testing::_, testing::_, testing::_))
            .WillByDefault(testing::DoAll(testing::SetArgReferee<3>(0.0f), testing::Return(true)));
        ON_CALL(*mock, GetInputStateVector2f(testing::_, testing::_, testing::_, testing::_))
            .WillByDefault(testing::DoAll(testing::SetArgReferee<3>(XrVector2f{0.0f, 0.0f}), testing::Return(true)));
    }

   private:
    // Default response data
    static const ox::runtime::RuntimePropertiesResponse runtime_props_;
    static const ox::runtime::SystemPropertiesResponse system_props_;
    static const ox::runtime::ViewConfigurationsResponse view_configs_;
    static const ox::runtime::InteractionProfilesResponse interaction_profiles_;
    static ox::runtime::SharedData shared_data_;
};

// Base test fixture for runtime tests
class RuntimeTestBase : public ::testing::Test {
   protected:
    void SetUp() override {
        mock_driver = std::make_unique<ox::test::MockDriverConnection>();

        ox::test::MockDriverConnection::SetupDefaultBehaviors(mock_driver.get());

        oxSetDriver(mock_driver.get());
    }

    void TearDown() override {
        oxSetDriver(nullptr);
        mock_driver.reset();
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
    std::unique_ptr<ox::test::MockDriverConnection> mock_driver;
};

}  // namespace test
}  // namespace ox
