//
// Created by cniew on 8/29/26.
//

#include "tx_queue.h"

#include <vector>
#include <rte_ethdev.h>

namespace dpdk {

tx_queue::tx_queue(uint16_t port_id, uint16_t queue_id)
    : handle_(new handle{port_id, queue_id}) {}

tx_queue::tx_queue(tx_queue &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

tx_queue &tx_queue::operator=(tx_queue &&other) noexcept {
    if (this != &other) {
        delete handle_;
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

tx_queue::~tx_queue() {
    delete handle_;
}

uint16_t tx_queue::send_burst(std::span<packet> pkts) const {
    // Reused across calls on this (single-threaded-by-contract) queue to
    // avoid a heap allocation on every send.
    thread_local std::vector<rte_mbuf *> bufs;
    if (bufs.size() < pkts.size()) {
        bufs.resize(pkts.size());
    }
    for (std::size_t i = 0; i < pkts.size(); ++i) {
        bufs[i] = pkts[i].native_handle();
    }

    const uint16_t n = rte_eth_tx_burst(handle_->port_id, handle_->queue_id,
                                         bufs.data(), static_cast<uint16_t>(pkts.size()));
    for (uint16_t i = 0; i < n; ++i) {
        pkts[i].release(); // ownership now belongs to the driver
    }
    return n;
}

uint16_t tx_queue::port_id() const noexcept { return handle_->port_id; }
uint16_t tx_queue::queue_id() const noexcept { return handle_->queue_id; }

} // namespace dpdk
