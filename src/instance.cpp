//
// Created by cniew on 8/29/26.
//

#include "instance.h"

#include <algorithm>
#include <string>
#include <rte_eal.h>
#include "dpdk_error.h"

namespace dpdk {

instance::instance(int argc, char **argv) {
    const int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        throw dpdk_error(
            std::string("rte_eal_init failed: ") + rte_strerror(rte_errno) +
            ". Common causes: hugepages not reserved (try `dpdk-hugepages.py "
            "--setup 1G`), or no NIC bound to a DPDK-compatible driver (try "
            "`dpdk-devbind.py --bind=vfio-pci <pci-addr>`, or pass "
            "`--vdev=net_null0` for a NIC-less smoke test).");
    }
}

instance::instance(instance &&other) noexcept
    : owns_eal_(other.owns_eal_), ports_(std::move(other.ports_)) {
    other.owns_eal_ = false;
}

instance &instance::operator=(instance &&other) noexcept {
    if (this != &other) {
        if (owns_eal_) {
            rte_eal_cleanup();
        }
        owns_eal_ = other.owns_eal_;
        ports_ = std::move(other.ports_);
        other.owns_eal_ = false;
    }
    return *this;
}

instance::~instance() {
    if (owns_eal_) {
        rte_eal_cleanup();
    }
}

port &instance::add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                          uint16_t elt_size) {
    ports_.emplace_back(port_id, n_rx_queues, n_tx_queues, elt_size);
    return ports_.back();
}

port &instance::get_port(uint16_t port_id) {
    auto it = std::find_if(ports_.begin(), ports_.end(), [port_id](const port &p) {
        return p.port_id() == port_id;
    });
    if (it == ports_.end()) {
        throw dpdk_error("No port with id " + std::to_string(port_id) +
                          " has been added to this instance");
    }
    return *it;
}

} // namespace dpdk
