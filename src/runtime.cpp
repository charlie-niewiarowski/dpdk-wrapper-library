//
// Created by cniew on 8/29/26.
//

#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include "dpdk_error.h"

namespace dpdk {

namespace {

constexpr unsigned kDefaultNumMbufs = 8192;

// Guards against a second runtime being constructed while one is
// alive. Not a singleton accessor -- nothing external can read or
// reach this; it only ever answers "may a new runtime claim ownership."
std::atomic<bool> g_runtime_exists{false};

} // namespace

runtime::runtime(int argc, char **argv) {
    bool expected = false;
    if (!g_runtime_exists.compare_exchange_strong(expected, true)) {
        throw dpdk_error("Only one dpdk::runtime may exist per process");
    }

    const int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        g_runtime_exists.store(false);
        throw dpdk_error(std::string("rte_eal_init failed: ") + rte_strerror(rte_errno));
    }
}

runtime::runtime(runtime &&other) noexcept
    : owns_eal_(other.owns_eal_), ports_(std::move(other.ports_)) {
    other.owns_eal_ = false;
}

runtime &runtime::operator=(runtime &&other) noexcept {
    if (this != &other) {
        if (owns_eal_) {
            // Ports (and the packet_pool each one owns) must be torn down
            // before the EAL is; rte_eal_cleanup() must be the last DPDK
            // call made.
            ports_.clear();
            rte_eal_cleanup();
            g_runtime_exists.store(false);
        }
        owns_eal_ = other.owns_eal_;
        ports_ = std::move(other.ports_);
        other.owns_eal_ = false;
    }
    return *this;
}

runtime::~runtime() {
    if (owns_eal_) {
        // Same ordering requirement as above: ports must go first.
        ports_.clear();
        rte_eal_cleanup();
        g_runtime_exists.store(false);
    }
}

std::shared_ptr<packet_pool> runtime::create_pool(const char *name, unsigned n_mbufs,
                                                   uint16_t elt_size, unsigned socket_id,
                                                   unsigned cache_size) {
    // Direct construction, not make_shared: packet_pool's constructor is
    // private (friend runtime), and make_shared's internal placement-new
    // happens inside <memory>'s own code, which isn't covered by that
    // friendship.
    return std::shared_ptr<packet_pool>(
        new packet_pool(name, n_mbufs, elt_size, socket_id, cache_size));
}

std::shared_ptr<packet_pool> runtime::make_default_pool(uint16_t port_id, uint16_t elt_size) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        throw dpdk_error("Port " + std::to_string(port_id) + " is not a valid DPDK port");
    }
    const std::string name = "PORT_" + std::to_string(port_id) + "_POOL";
    const int socket_id = rte_eth_dev_socket_id(port_id);
    return create_pool(name.c_str(), kDefaultNumMbufs, elt_size,
                        static_cast<unsigned>(socket_id));
}

port &runtime::add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                         uint16_t elt_size) {
    return add_port(port_id, n_rx_queues, n_tx_queues, make_default_pool(port_id, elt_size));
}

port &runtime::add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                         std::shared_ptr<packet_pool> pool) {
    // port's constructor is private (friend runtime); deque::emplace_back
    // would construct in-place from inside the standard library's own
    // code, which isn't covered by that friendship either. Construct
    // directly here (a friend call, since it's textually inside a
    // runtime member function), then move the finished port in -- that
    // only needs port's public move constructor.
    port p(port_id, n_rx_queues, n_tx_queues, std::move(pool));
    ports_.push_back(std::move(p));
    return ports_.back();
}

port &runtime::get_port(uint16_t port_id) {
    auto it = std::find_if(ports_.begin(), ports_.end(), [port_id](const port &p) {
        return p.port_id() == port_id;
    });
    if (it == ports_.end()) {
        throw dpdk_error("No port with id " + std::to_string(port_id) +
                          " has been added to this runtime");
    }
    return *it;
}

} // namespace dpdk
