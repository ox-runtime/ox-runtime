set(RUNTIME_TEST_SOURCES
    test_runtime.cpp
    ../../src/runtime.cpp
    ../../src/driver_connection.cpp
    common.h
)

if(APPLE AND OX_METAL)
    list(APPEND RUNTIME_TEST_SOURCES test_metal.mm)
    set_source_files_properties(../../src/runtime.cpp PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )
endif()

add_executable(runtime_tests ${RUNTIME_TEST_SOURCES})

if(WIN32 AND MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_property(TARGET runtime_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}"
    )
endif()

target_include_directories(runtime_tests PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(runtime_tests PRIVATE
    OX_BUILD_STATIC
    OX_RUNTIME_TESTS
)

target_link_libraries(runtime_tests PRIVATE
    GTest::gtest_main
    GTest::gmock
    spdlog::spdlog
)

configure_graphics(runtime_tests)

gtest_discover_tests(runtime_tests)

set_target_properties(runtime_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests/runtime
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/tests/runtime
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/tests/runtime
)