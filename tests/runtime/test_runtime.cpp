#include <gtest/gtest.h>
#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common.h"

using namespace ox::test;

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

namespace {

XrSystemId GetTestSystemId(XrInstance instance) {
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    EXPECT_EQ(xrGetSystem(instance, &system_info, &system_id), XR_SUCCESS);
    return system_id;
}

XrSession CreateTestSession(XrInstance instance, XrSystemId system_id) {
    XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
    create_info.systemId = system_id;

    XrSession session = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreateSession(instance, &create_info, &session), XR_SUCCESS);
    return session;
}

XrActionSet CreateTestActionSet(XrInstance instance, const char* name) {
    XrActionSetCreateInfo create_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(create_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", name);
    std::snprintf(create_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", name);
    create_info.priority = 0;

    XrActionSet action_set = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreateActionSet(instance, &create_info, &action_set), XR_SUCCESS);
    return action_set;
}

XrAction CreateBooleanAction(XrActionSet action_set, const char* name, const XrPath* subaction_paths, uint32_t count) {
    XrActionCreateInfo create_info{XR_TYPE_ACTION_CREATE_INFO};
    create_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::snprintf(create_info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(create_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", name);
    create_info.countSubactionPaths = count;
    create_info.subactionPaths = subaction_paths;

    XrAction action = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreateAction(action_set, &create_info, &action), XR_SUCCESS);
    return action;
}

XrAction CreateFloatAction(XrActionSet action_set, const char* name, const XrPath* subaction_paths, uint32_t count) {
    XrActionCreateInfo create_info{XR_TYPE_ACTION_CREATE_INFO};
    create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::snprintf(create_info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(create_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", name);
    create_info.countSubactionPaths = count;
    create_info.subactionPaths = subaction_paths;

    XrAction action = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreateAction(action_set, &create_info, &action), XR_SUCCESS);
    return action;
}

XrAction CreateVector2Action(XrActionSet action_set, const char* name, const XrPath* subaction_paths, uint32_t count) {
    XrActionCreateInfo create_info{XR_TYPE_ACTION_CREATE_INFO};
    create_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::snprintf(create_info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(create_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", name);
    create_info.countSubactionPaths = count;
    create_info.subactionPaths = subaction_paths;

    XrAction action = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreateAction(action_set, &create_info, &action), XR_SUCCESS);
    return action;
}

void SuggestBindings(XrInstance instance, XrPath profile, const std::vector<XrActionSuggestedBinding>& bindings) {
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    EXPECT_EQ(xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
}

void AttachActionSet(XrSession session, XrActionSet action_set) {
    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.countActionSets = 1;
    attach_info.actionSets = &action_set;
    EXPECT_EQ(xrAttachSessionActionSets(session, &attach_info), XR_SUCCESS);
}

XrPath MakePath(XrInstance instance, const char* path_string) {
    XrPath path = XR_NULL_PATH;
    EXPECT_EQ(xrStringToPath(instance, path_string, &path), XR_SUCCESS);
    return path;
}

}  // namespace

TEST_F(RuntimeTestBase, BooleanBindingsMatchActualSubactionPathWhenSubactionOrderIsReversed) {
    driver_state.interaction_profiles = {"/interaction_profiles/oculus/touch_controller"};
    driver_state.bool_inputs["/user/hand/left|/input/x/click"] = XR_TRUE;
    driver_state.bool_inputs["/user/hand/right|/input/a/touch"] = XR_TRUE;

    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);
    const XrSystemId system_id = GetTestSystemId(instance);
    ASSERT_NE(system_id, XR_NULL_SYSTEM_ID);
    const XrSession session = CreateTestSession(instance, system_id);
    ASSERT_NE(session, XR_NULL_HANDLE);

    const XrPath left_hand = MakePath(instance, "/user/hand/left");
    const XrPath right_hand = MakePath(instance, "/user/hand/right");
    const XrPath reversed_subactions[] = {right_hand, left_hand};
    const XrPath profile = MakePath(instance, "/interaction_profiles/oculus/touch_controller");

    const XrActionSet action_set = CreateTestActionSet(instance, "buttons");
    const XrAction x_click = CreateBooleanAction(action_set, "x_click", reversed_subactions, 2);
    const XrAction a_touch = CreateBooleanAction(action_set, "a_touch", reversed_subactions, 2);

    SuggestBindings(instance, profile,
                    {{x_click, MakePath(instance, "/user/hand/left/input/x/click")},
                     {a_touch, MakePath(instance, "/user/hand/right/input/a/touch")}});
    AttachActionSet(session, action_set);

    XrActionStateBoolean x_state{XR_TYPE_ACTION_STATE_BOOLEAN};
    XrActionStateGetInfo x_get{XR_TYPE_ACTION_STATE_GET_INFO};
    x_get.action = x_click;
    x_get.subactionPath = left_hand;
    ASSERT_EQ(xrGetActionStateBoolean(session, &x_get, &x_state), XR_SUCCESS);
    EXPECT_EQ(x_state.isActive, XR_TRUE);
    EXPECT_EQ(x_state.currentState, XR_TRUE);

    XrActionStateBoolean a_state{XR_TYPE_ACTION_STATE_BOOLEAN};
    XrActionStateGetInfo a_get{XR_TYPE_ACTION_STATE_GET_INFO};
    a_get.action = a_touch;
    a_get.subactionPath = right_hand;
    ASSERT_EQ(xrGetActionStateBoolean(session, &a_get, &a_state), XR_SUCCESS);
    EXPECT_EQ(a_state.isActive, XR_TRUE);
    EXPECT_EQ(a_state.currentState, XR_TRUE);
}

TEST_F(RuntimeTestBase, FloatBindingsStillMatchActualSubactionPathWhenSubactionOrderIsReversed) {
    driver_state.interaction_profiles = {"/interaction_profiles/oculus/touch_controller"};
    driver_state.float_inputs["/user/hand/right|/input/trigger/value"] = 0.8f;

    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);
    const XrSystemId system_id = GetTestSystemId(instance);
    ASSERT_NE(system_id, XR_NULL_SYSTEM_ID);
    const XrSession session = CreateTestSession(instance, system_id);
    ASSERT_NE(session, XR_NULL_HANDLE);

    const XrPath left_hand = MakePath(instance, "/user/hand/left");
    const XrPath right_hand = MakePath(instance, "/user/hand/right");
    const XrPath reversed_subactions[] = {right_hand, left_hand};
    const XrPath profile = MakePath(instance, "/interaction_profiles/oculus/touch_controller");

    const XrActionSet action_set = CreateTestActionSet(instance, "floats");
    const XrAction trigger = CreateFloatAction(action_set, "trigger", reversed_subactions, 2);

    SuggestBindings(instance, profile, {{trigger, MakePath(instance, "/user/hand/right/input/trigger/value")}});
    AttachActionSet(session, action_set);

    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = trigger;
    get_info.subactionPath = right_hand;
    ASSERT_EQ(xrGetActionStateFloat(session, &get_info, &state), XR_SUCCESS);
    EXPECT_EQ(state.isActive, XR_TRUE);
    EXPECT_NEAR(state.currentState, 0.8f, 0.0001f);
}

TEST_F(RuntimeTestBase, FloatActionConvertsBooleanBindingsToZeroOrOne) {
    driver_state.interaction_profiles = {"/interaction_profiles/oculus/touch_controller"};
    driver_state.bool_inputs["/user/hand/left|/input/x/click"] = XR_TRUE;

    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);
    const XrSystemId system_id = GetTestSystemId(instance);
    ASSERT_NE(system_id, XR_NULL_SYSTEM_ID);
    const XrSession session = CreateTestSession(instance, system_id);
    ASSERT_NE(session, XR_NULL_HANDLE);

    const XrPath left_hand = MakePath(instance, "/user/hand/left");
    const XrPath profile = MakePath(instance, "/interaction_profiles/oculus/touch_controller");

    const XrActionSet action_set = CreateTestActionSet(instance, "float_buttons");
    const XrAction action = CreateFloatAction(action_set, "x_click_float", &left_hand, 1);

    SuggestBindings(instance, profile, {{action, MakePath(instance, "/user/hand/left/input/x/click")}});
    AttachActionSet(session, action_set);

    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = left_hand;
    ASSERT_EQ(xrGetActionStateFloat(session, &get_info, &state), XR_SUCCESS);
    EXPECT_EQ(state.isActive, XR_TRUE);
    EXPECT_NEAR(state.currentState, 1.0f, 0.0001f);
}

TEST_F(RuntimeTestBase, BooleanActionOrsMultipleBoundInputsForSameQuery) {
    driver_state.interaction_profiles = {"/interaction_profiles/oculus/touch_controller"};
    driver_state.bool_inputs["/user/hand/left|/input/x/click"] = XR_FALSE;
    driver_state.bool_inputs["/user/hand/right|/input/a/click"] = XR_TRUE;

    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);
    const XrSystemId system_id = GetTestSystemId(instance);
    ASSERT_NE(system_id, XR_NULL_SYSTEM_ID);
    const XrSession session = CreateTestSession(instance, system_id);
    ASSERT_NE(session, XR_NULL_HANDLE);

    const XrPath profile = MakePath(instance, "/interaction_profiles/oculus/touch_controller");
    const XrActionSet action_set = CreateTestActionSet(instance, "boolean_or");
    const XrAction action = CreateBooleanAction(action_set, "confirm", nullptr, 0);

    SuggestBindings(instance, profile,
                    {{action, MakePath(instance, "/user/hand/left/input/x/click")},
                     {action, MakePath(instance, "/user/hand/right/input/a/click")}});
    AttachActionSet(session, action_set);

    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = XR_NULL_PATH;
    ASSERT_EQ(xrGetActionStateBoolean(session, &get_info, &state), XR_SUCCESS);
    EXPECT_EQ(state.isActive, XR_TRUE);
    EXPECT_EQ(state.currentState, XR_TRUE);
}

TEST_F(RuntimeTestBase, FloatActionUsesLargestAbsoluteValueAcrossBindings) {
    driver_state.interaction_profiles = {"/interaction_profiles/oculus/touch_controller"};
    driver_state.float_inputs["/user/hand/left|/input/trigger/value"] = 0.25f;
    driver_state.float_inputs["/user/hand/right|/input/trigger/value"] = -0.8f;

    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);
    const XrSystemId system_id = GetTestSystemId(instance);
    ASSERT_NE(system_id, XR_NULL_SYSTEM_ID);
    const XrSession session = CreateTestSession(instance, system_id);
    ASSERT_NE(session, XR_NULL_HANDLE);

    const XrPath profile = MakePath(instance, "/interaction_profiles/oculus/touch_controller");
    const XrActionSet action_set = CreateTestActionSet(instance, "float_max_abs");
    const XrAction action = CreateFloatAction(action_set, "trigger_any", nullptr, 0);

    SuggestBindings(instance, profile,
                    {{action, MakePath(instance, "/user/hand/left/input/trigger/value")},
                     {action, MakePath(instance, "/user/hand/right/input/trigger/value")}});
    AttachActionSet(session, action_set);

    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = XR_NULL_PATH;
    ASSERT_EQ(xrGetActionStateFloat(session, &get_info, &state), XR_SUCCESS);
    EXPECT_EQ(state.isActive, XR_TRUE);
    EXPECT_NEAR(state.currentState, -0.8f, 0.0001f);
}

TEST_F(RuntimeTestBase, Vector2ActionUsesLongestBoundInput) {
    driver_state.interaction_profiles = {"/interaction_profiles/oculus/touch_controller"};
    driver_state.vector2_inputs["/user/hand/left|/input/thumbstick"] = {0.25f, 0.1f};
    driver_state.vector2_inputs["/user/hand/right|/input/thumbstick"] = {0.6f, 0.6f};

    XrInstance instance = CreateBasicInstance();
    ASSERT_NE(instance, XR_NULL_HANDLE);
    const XrSystemId system_id = GetTestSystemId(instance);
    ASSERT_NE(system_id, XR_NULL_SYSTEM_ID);
    const XrSession session = CreateTestSession(instance, system_id);
    ASSERT_NE(session, XR_NULL_HANDLE);

    const XrPath profile = MakePath(instance, "/interaction_profiles/oculus/touch_controller");
    const XrActionSet action_set = CreateTestActionSet(instance, "vec2_longest");
    const XrAction action = CreateVector2Action(action_set, "thumb_any", nullptr, 0);

    SuggestBindings(instance, profile,
                    {{action, MakePath(instance, "/user/hand/left/input/thumbstick")},
                     {action, MakePath(instance, "/user/hand/right/input/thumbstick")}});
    AttachActionSet(session, action_set);

    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = XR_NULL_PATH;
    ASSERT_EQ(xrGetActionStateVector2f(session, &get_info, &state), XR_SUCCESS);
    EXPECT_EQ(state.isActive, XR_TRUE);
    EXPECT_NEAR(state.currentState.x, 0.6f, 0.0001f);
    EXPECT_NEAR(state.currentState.y, 0.6f, 0.0001f);
}
