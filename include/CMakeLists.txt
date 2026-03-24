cmake_minimum_required(VERSION 3.27)

project(ox-driver-api LANGUAGES C)

include(FetchContent)

if(NOT TARGET openxr_loader AND NOT TARGET OpenXR::openxr_loader)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_LOADER OFF CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.42
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(openxr_sdk)
endif()

if(NOT TARGET ox_driver_api)
    add_library(ox_driver_api INTERFACE)
    add_library(ox::driver_api ALIAS ox_driver_api)
endif()

if(DEFINED openxr_sdk_SOURCE_DIR)
    set(OX_DRIVER_API_OPENXR_INCLUDE_DIR "${openxr_sdk_SOURCE_DIR}/include")
elseif(DEFINED openxr_SOURCE_DIR)
    set(OX_DRIVER_API_OPENXR_INCLUDE_DIR "${openxr_SOURCE_DIR}/include")
else()
    message(FATAL_ERROR "Could not determine OpenXR SDK include directory for ox_driver_api")
endif()

target_include_directories(ox_driver_api INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OX_DRIVER_API_OPENXR_INCLUDE_DIR}
)