include(FetchContent)
FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(dpdk_lib_tests
        test_main.cpp
        test_env.h
        port_helpers_test.cpp
        arp_test.cpp
        tcp_test.cpp)
target_link_libraries(dpdk_lib_tests PRIVATE dpdk_lib GTest::gtest)

# One test entry for the whole binary rather than gtest_discover_tests:
# every discovered case would be its own process, each paying for a full
# EAL bring-up.
add_test(NAME dpdk_lib_tests COMMAND dpdk_lib_tests)

# Kernel-socket integration test over net_tap. Needs root (CAP_NET_ADMIN)
# to create the tap interface; exits 77 without it, which ctest reports
# as skipped rather than failed. Select it alone with `ctest -L privileged`.
add_executable(tap_integration_test
        tap_integration_test.cpp
        ${PROJECT_SOURCE_DIR}/examples/tcp_echo_server.h)
target_include_directories(tap_integration_test PRIVATE ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(tap_integration_test PRIVATE dpdk_lib GTest::gtest)
add_test(NAME tap_integration_test COMMAND tap_integration_test)
set_tests_properties(tap_integration_test PROPERTIES
        LABELS privileged
        SKIP_RETURN_CODE 77)
set_tests_properties(dpdk_lib_tests PROPERTIES LABELS unprivileged)
