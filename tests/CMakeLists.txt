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
