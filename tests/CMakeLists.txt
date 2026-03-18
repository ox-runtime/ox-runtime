cmake_minimum_required(VERSION 3.27)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Include GoogleTest module for test discovery
include(GoogleTest)

# Add test subdirectories
add_subdirectory(runtime)
