cmake_minimum_required(VERSION 3.27)

project(ox-runtime LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

if(WIN32 AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT WIN32)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(NOT TARGET spdlog::spdlog)
    include(FetchContent)

    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG v1.17.0
        GIT_SHALLOW true
    )
    if(NOT WIN32)
        set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(spdlog)
endif()

if(NOT TARGET dylib)
    include(FetchContent)

    FetchContent_Declare(
        dylib
        GIT_REPOSITORY https://github.com/martin-olivier/dylib
        GIT_TAG v3.0.1
        GIT_SHALLOW true
    )
    FetchContent_MakeAvailable(dylib)
endif()

if(NOT TARGET whereami)
    include(FetchContent)

    FetchContent_Declare(
        whereami
        GIT_REPOSITORY https://github.com/gpakosz/whereami.git
        GIT_TAG master
        GIT_SHALLOW true
    )
    FetchContent_MakeAvailable(whereami)

    add_library(whereami STATIC ${whereami_SOURCE_DIR}/src/whereami.c)
    if(NOT WIN32)
        set_target_properties(whereami PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    target_include_directories(whereami PUBLIC ${whereami_SOURCE_DIR}/src)
    if(CMAKE_DL_LIBS)
        target_link_libraries(whereami PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(include)

if(APPLE)
    option(OX_OPENGL "Enable OpenGL graphics API support" ON)
    option(OX_VULKAN "Enable Vulkan graphics API support" OFF)
    option(OX_METAL "Enable Metal graphics API support on macOS" ON)
else()
    option(OX_OPENGL "Enable OpenGL graphics API support" ON)
    option(OX_VULKAN "Enable Vulkan graphics API support" ON)
    option(OX_METAL "Enable Metal graphics API support on macOS" OFF)
endif()

set(OX_RUNTIME_SOURCES
    src/runtime.cpp
)

if(APPLE AND OX_METAL)
    set_source_files_properties(src/runtime.cpp PROPERTIES
        LANGUAGE OBJCXX
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
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

configure_graphics(ox_runtime)

target_link_libraries(ox_runtime PRIVATE ox_driver_api spdlog::spdlog dylib whereami)

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

# Hide non-exported C++ symbols by default so only explicit API exports leave the shared library.
set_target_properties(ox_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/bin/ox_runtime.json"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/ox_runtime.json.in"
)

install(TARGETS ox_runtime
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

add_subdirectory(tests)
