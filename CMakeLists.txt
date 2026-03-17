cmake_minimum_required(VERSION 3.5)

project(ox-runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(WIN32 AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT TARGET spdlog::spdlog)
    include(FetchContent)

    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG v1.17.0
        GIT_SHALLOW true
    )
    FetchContent_MakeAvailable(spdlog)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_compile_definitions(NDEBUG)
endif()

if(APPLE)
    option(OX_OPENGL "Enable OpenGL graphics API support" ON)
    option(OX_VULKAN "Enable Vulkan graphics API support" OFF)
    option(OX_METAL "Enable Metal graphics API support on macOS" ON)
else()
    option(OX_OPENGL "Enable OpenGL graphics API support" ON)
    option(OX_VULKAN "Enable Vulkan graphics API support" ON)
    option(OX_METAL "Enable Metal graphics API support on macOS" OFF)
endif()

FetchContent_Declare(
    openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.54
)
FetchContent_MakeAvailable(openxr)

set(OPENXR_INCLUDE_DIR ${openxr_SOURCE_DIR}/include)

set(OX_RUNTIME_SOURCES
    src/runtime.cpp
    src/driver_connection.cpp
)

if(APPLE AND OX_METAL)
    set_source_files_properties(src/runtime.cpp PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )
endif()

function(configure_graphics target)
    if(OX_OPENGL)
        target_compile_definitions(${target} PRIVATE OX_OPENGL=1)
        find_package(OpenGL REQUIRED)
        target_link_libraries(${target} PRIVATE OpenGL::GL)
    endif()

    if(OX_VULKAN)
        target_compile_definitions(${target} PRIVATE OX_VULKAN=1)
        find_package(Vulkan REQUIRED)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
        target_include_directories(${target} PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()

    if(OX_METAL)
        target_compile_definitions(${target} PRIVATE OX_METAL=1)
    endif()

    if(APPLE AND OX_METAL)
        find_library(METAL_LIBRARY Metal REQUIRED)
        find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
        find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
        target_link_libraries(${target} PRIVATE ${METAL_LIBRARY} ${QUARTZCORE_LIBRARY} ${COREFOUNDATION_LIBRARY})
    endif()
endfunction()

add_library(ox_runtime SHARED ${OX_RUNTIME_SOURCES})

target_include_directories(ox_runtime PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(ox_runtime PRIVATE
    OX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    OX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    OX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

configure_graphics(ox_runtime)

target_link_libraries(ox_runtime PRIVATE spdlog::spdlog)

if(WIN32)
    target_compile_definitions(ox_runtime PRIVATE
        XR_USE_PLATFORM_WIN32
        XR_RUNTIME_EXPORTS
    )
    set(RUNTIME_LIBRARY_NAME "ox_runtime.dll")
    set(RUNTIME_LIBRARY_PATH "./ox_runtime.dll")
elseif(APPLE)
    target_compile_definitions(ox_runtime PRIVATE
        XR_USE_PLATFORM_MACOS
        GL_SILENCE_DEPRECATION
    )
    set_target_properties(ox_runtime PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )
    set(RUNTIME_LIBRARY_NAME "libox_runtime.dylib")
    set(RUNTIME_LIBRARY_PATH "./libox_runtime.dylib")
else()
    target_compile_definitions(ox_runtime PRIVATE
        XR_USE_PLATFORM_XLIB
    )
    set_target_properties(ox_runtime PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )
    set(RUNTIME_LIBRARY_NAME "libox_runtime.so")
    set(RUNTIME_LIBRARY_PATH "./libox_runtime.so")
endif()

set_target_properties(ox_runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/lib
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib
)

set(MANIFEST_OUTPUT "${CMAKE_BINARY_DIR}/bin/ox_runtime.json")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ox_runtime.json.in"
    "${MANIFEST_OUTPUT}"
    @ONLY
)

install(TARGETS ox_runtime
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

add_subdirectory(tests)