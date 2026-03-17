#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common.h"

using namespace ox::test;

// Define static members of MockDriverConnection
const ox::runtime::RuntimePropertiesResponse ox::test::MockDriverConnection::runtime_props_ = []() {
    ox::runtime::RuntimePropertiesResponse props = {};
    snprintf(props.runtime_name, sizeof(props.runtime_name), "%s", "ox Mock Runtime");
    props.runtime_version_major = 1;
    props.runtime_version_minor = 0;
    props.runtime_version_patch = 0;
    props.padding = 0;
    return props;
}();

const ox::runtime::SystemPropertiesResponse ox::test::MockDriverConnection::system_props_ = []() {
    ox::runtime::SystemPropertiesResponse props = {};
    snprintf(props.system_name, sizeof(props.system_name), "%s", "Mock VR System");
    props.max_swapchain_width = 2048;
    props.max_swapchain_height = 2048;
    props.max_layer_count = 16;
    props.orientation_tracking = 1;
    props.position_tracking = 1;
    props.padding[0] = 0;
    props.padding[1] = 0;
    return props;
}();

const ox::runtime::ViewConfigurationsResponse ox::test::MockDriverConnection::view_configs_ = []() {
    ox::runtime::ViewConfigurationsResponse configs = {};
    configs.views[0].recommended_width = 1832;
    configs.views[0].recommended_height = 1920;
    configs.views[0].recommended_sample_count = 1;
    configs.views[0].max_sample_count = 4;
    configs.views[1].recommended_width = 1832;
    configs.views[1].recommended_height = 1920;
    configs.views[1].recommended_sample_count = 1;
    configs.views[1].max_sample_count = 4;
    return configs;
}();

const ox::runtime::InteractionProfilesResponse ox::test::MockDriverConnection::interaction_profiles_ = []() {
    ox::runtime::InteractionProfilesResponse profiles = {};
    profiles.profile_count = 1;
    snprintf(profiles.profiles[0], sizeof(profiles.profiles[0]), "%s", "/interaction_profiles/khr/simple_controller");
    return profiles;
}();

ox::runtime::SharedData ox::test::MockDriverConnection::shared_data_ = {};

// ============================================================================
// Instance Tests
// ============================================================================

TEST_F(RuntimeTestBase, CreateInstance_ValidParams_ReturnsSuccess) {
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(create_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "TestApp");
    create_info.applicationInfo.applicationVersion = 1;
    snprintf(create_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "TestEngine");
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(&create_info, &instance);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_NE(instance, XR_NULL_HANDLE);

    if (instance != XR_NULL_HANDLE) {
        created_instances_.push_back(instance);
    }
}

TEST_F(RuntimeTestBase, CreateInstance_NullCreateInfo_ReturnsError) {
    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(nullptr, &instance);

    EXPECT_EQ(result, XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(instance, XR_NULL_HANDLE);
}

TEST_F(RuntimeTestBase, CreateInstance_NullInstanceOut_ReturnsError) {
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(create_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "TestApp");
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrResult result = xrCreateInstance(&create_info, nullptr);

    EXPECT_EQ(result, XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(RuntimeTestBase, DestroyInstance_ValidInstance_ReturnsSuccess) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrResult result = xrDestroyInstance(instance);
    EXPECT_EQ(result, XR_SUCCESS);

    // Remove from cleanup list since we manually destroyed it
    created_instances_.erase(std::remove(created_instances_.begin(), created_instances_.end(), instance),
                             created_instances_.end());
}

TEST_F(RuntimeTestBase, DestroyInstance_NullHandle_ReturnsError) {
    XrResult result = xrDestroyInstance(XR_NULL_HANDLE);
    EXPECT_EQ(result, XR_ERROR_HANDLE_INVALID);
}

TEST_F(RuntimeTestBase, GetInstanceProperties_ValidInstance_ReturnsSuccess) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    XrResult result = xrGetInstanceProperties(instance, &props);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_GT(std::strlen(props.runtimeName), 0u) << "Runtime name should not be empty";
}

TEST_F(RuntimeTestBase, GetInstanceProperties_NullProperties_ReturnsError) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrResult result = xrGetInstanceProperties(instance, nullptr);
    EXPECT_EQ(result, XR_ERROR_VALIDATION_FAILURE);
}

// ============================================================================
// String Conversion Tests
// ============================================================================

TEST_F(RuntimeTestBase, StringToPath_ValidPath_ReturnsSuccess) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrPath path = XR_NULL_PATH;
    XrResult result = xrStringToPath(instance, "/user/hand/left", &path);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_NE(path, XR_NULL_PATH);
}

TEST_F(RuntimeTestBase, StringToPath_NullString_ReturnsError) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrPath path = XR_NULL_PATH;
    XrResult result = xrStringToPath(instance, nullptr, &path);

    EXPECT_EQ(result, XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(RuntimeTestBase, PathToString_ValidPath_ReturnsSuccess) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    // First create a path
    XrPath path = XR_NULL_PATH;
    const char* original_path_str = "/user/hand/left";
    XrResult result = xrStringToPath(instance, original_path_str, &path);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_NE(path, XR_NULL_PATH);

    // Now convert it back
    uint32_t buffer_size = 0;
    result = xrPathToString(instance, path, 0, &buffer_size, nullptr);
    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_GT(buffer_size, 0u);

    std::string buffer(buffer_size, '\0');
    uint32_t written = 0;
    result = xrPathToString(instance, path, buffer_size, &written, &buffer[0]);
    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_EQ(written, buffer_size);
    EXPECT_STREQ(buffer.c_str(), original_path_str);
}

// ============================================================================
// System Tests
// ============================================================================

TEST_F(RuntimeTestBase, GetSystem_ValidFormFactor_ReturnsSuccess) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrResult result = xrGetSystem(instance, &system_info, &system_id);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_NE(system_id, XR_NULL_SYSTEM_ID);
}

TEST_F(RuntimeTestBase, GetSystem_NullSystemInfo_ReturnsError) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrResult result = xrGetSystem(instance, nullptr, &system_id);

    EXPECT_EQ(result, XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(RuntimeTestBase, GetSystemProperties_ValidSystem_ReturnsSuccess) {
    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrResult result = xrGetSystem(instance, &system_info, &system_id);
    ASSERT_EQ(result, XR_SUCCESS);

    XrSystemProperties sys_props{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(instance, system_id, &sys_props);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_GT(std::strlen(sys_props.systemName), 0u) << "System name should not be empty";
}

// ============================================================================
// Extension Tests
// ============================================================================

TEST_F(RuntimeTestBase, EnumerateInstanceExtensionProperties_GetCount_ReturnsSuccess) {
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_GT(count, 0u) << "Should have at least one extension";
}

TEST_F(RuntimeTestBase, EnumerateInstanceExtensionProperties_GetExtensions_ReturnsSuccess) {
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    ASSERT_EQ(result, XR_SUCCESS);
    ASSERT_GT(count, 0u);

    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());

    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_EQ(extensions.size(), count);

    // Check that at least one extension has a non-empty name
    bool has_named_extension = false;
    for (const auto& ext : extensions) {
        if (std::strlen(ext.extensionName) > 0) {
            has_named_extension = true;
            break;
        }
    }
    EXPECT_TRUE(has_named_extension) << "At least one extension should have a name";
}
