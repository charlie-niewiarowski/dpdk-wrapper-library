//
// Created by cniew on 8/29/26.
//

#include "rx_queue.h"

#include <vector>
#include <rte_ethdev.h>
#include "memory_pool.h"

namespace dpdk {

rx_queue::rx_queue(uint16_t port_id, uint16_t queue_id)
    : handle_(new handle{port_id, queue_id}) {}

rx_queue::rx_queue(rx_queue &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

rx_queue &rx_queue::operator=(rx_queue &&other) noexcept {
    if (this != &other) {
        delete handle_;
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

rx_queue::~rx_queue() {
    delete handle_;
}

packet_burst rx_queue::receive_burst(std::size_t max_count) const {
    if (max_count > packet_burst::max_size) {
        max_count = packet_burst::max_size;
    }

    // Reused across calls on this (single-threaded-by-contract) queue to
    // avoid a heap allocation on every poll.
    thread_local std::vector<rte_mbuf *> bufs;
    if (bufs.size() < max_count) {
        bufs.resize(max_count);
    }

    packet_burst burst;
    burst.count_ = rte_eth_rx_burst(handle_->port_id, handle_->queue_id,
                                     bufs.data(), static_cast<uint16_t>(max_count));
    for (std::size_t i = 0; i < burst.count_; ++i) {
        burst.packets_[i] = memory_pool::get_packet(bufs[i]);
    }
    return burst;
}

uint16_t rx_queue::port_id() const noexcept { return handle_->port_id; }
uint16_t rx_queue::queue_id() const noexcept { return handle_->queue_id; }

} // namespace dpdk
