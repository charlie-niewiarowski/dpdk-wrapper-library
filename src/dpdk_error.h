//
// Created by cniew on 8/29/26.
//

#ifndef DPDK_ERROR_H
#define DPDK_ERROR_H

#include <stdexcept>
#include <string>

namespace dpdk {

// Thrown when a DPDK C API call fails during setup (EAL init, port/queue
// configuration, pool creation, ...). A single type keeps failures from
// this library's setup path uniformly catchable.
class dpdk_error : public std::runtime_error {
public:
    explicit dpdk_error(const std::string &what) : std::runtime_error(what) {}
};

} // namespace dpdk

#endif //DPDK_ERROR_H
