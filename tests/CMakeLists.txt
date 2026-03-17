cmake_minimum_required(VERSION 3.15)

# Fetch GoogleTest and let it inherit the parent project's CRT selection.
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# Keep GoogleTest on the same MSVC runtime library as the rest of the build.
if(WIN32 AND MSVC)
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(googletest)

# Include GoogleTest module for test discovery
include(GoogleTest)

# Add test subdirectories
add_subdirectory(runtime)

# Future test categories can be added here:
# add_subdirectory(ipc)
# add_subdirectory(driver)
# add_subdirectory(graphics)
