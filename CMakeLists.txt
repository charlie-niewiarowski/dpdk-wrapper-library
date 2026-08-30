cmake_minimum_required(VERSION 3.31)
project(kernel_bypass_networking_library)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

add_library(dpdk_lib STATIC
        src/packet.cpp
        src/packet.h
        src/rx_queue.cpp
        src/rx_queue.h
        src/tx_queue.cpp
        src/tx_queue.h
        src/packet_pool.cpp
        src/packet_pool.h
        src/packet_burst.cpp
        src/packet_burst.h
        src/dpdk_error.h
        src/port.cpp
        src/port.h
        src/instance.cpp
        src/instance.h)
target_include_directories(dpdk_lib PUBLIC src)
target_link_libraries(dpdk_lib PUBLIC PkgConfig::DPDK)

add_executable(kernel_bypass_networking_library main.cpp)
target_link_libraries(kernel_bypass_networking_library PRIVATE PkgConfig::DPDK)

add_executable(easy_usage examples/easy_usage.cpp)
target_link_libraries(easy_usage PRIVATE dpdk_lib)

add_executable(advanced_usage examples/advanced_usage.cpp)
target_link_libraries(advanced_usage PRIVATE dpdk_lib)
