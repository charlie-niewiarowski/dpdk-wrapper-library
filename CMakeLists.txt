cmake_minimum_required(VERSION 3.31)
project(kernel_bypass_networking_library)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)

# LTO/IPO lets the compiler inline across translation-unit boundaries --
# e.g. rx_queue.cpp calling into packet.cpp/packet_pool.cpp -- which it
# otherwise can't do even at -O3. Without this, small cross-TU wrapper
# functions stay real calls instead of disappearing.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(NOT ipo_supported)
    message(STATUS "IPO/LTO not supported by this toolchain: ${ipo_error}")
endif()

function(enable_ipo_if_supported target)
    if(ipo_supported)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

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
        src/runtime.cpp
        src/runtime.h)
target_include_directories(dpdk_lib PUBLIC src)
target_link_libraries(dpdk_lib PUBLIC PkgConfig::DPDK)
enable_ipo_if_supported(dpdk_lib)

add_executable(kernel_bypass_networking_library main.cpp)
target_link_libraries(kernel_bypass_networking_library PRIVATE PkgConfig::DPDK)

add_executable(easy_usage examples/easy_usage.cpp)
target_link_libraries(easy_usage PRIVATE dpdk_lib)
enable_ipo_if_supported(easy_usage)

add_executable(advanced_usage examples/advanced_usage.cpp)
target_link_libraries(advanced_usage PRIVATE dpdk_lib)
enable_ipo_if_supported(advanced_usage)

add_executable(bench_bare_dpdk benchmarks/bench_bare_dpdk.cpp)
target_link_libraries(bench_bare_dpdk PRIVATE PkgConfig::DPDK)
enable_ipo_if_supported(bench_bare_dpdk)

add_executable(bench_library benchmarks/bench_library.cpp)
target_link_libraries(bench_library PRIVATE dpdk_lib)
enable_ipo_if_supported(bench_library)
