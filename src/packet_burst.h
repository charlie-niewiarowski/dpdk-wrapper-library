//
// Created by cniew on 8/29/26.
//

#ifndef PACKET_BURST_H
#define PACKET_BURST_H

#include <cstddef>
#include <new>
#include <span>
#include "packet.h"

namespace dpdk {

// Owns a bounded batch of packets, e.g. everything pulled off an
// rx_queue in one poll. Each packet frees itself individually (packet
// is RAII) so there's nothing extra to clean up here — this exists so
// callers don't have to declare and manage their own receive buffer or
// track how many of it are actually populated.
//
// Backed by raw storage rather than std::array<packet, max_size>: only
// the packets actually received (indices [0, size())) are ever
// constructed or destroyed. A fixed-size array would default-construct
// (and, on every destruction, visit) all max_size slots regardless of
// how many are real -- for the common case of a handful of packets per
// poll, that fixed cost dominates the actual rte_eth_rx_burst() work
// this exists to wrap. The tradeoff: a queue that's consistently busy
// near max_size now pays a real per-packet construction cost this
// design doesn't amortize away, which the old fixed-array approach did.
class packet_burst {
public:
    static constexpr std::size_t max_size = 32;

    packet_burst() noexcept = default;
    packet_burst(packet_burst &&other) noexcept;
    packet_burst &operator=(packet_burst &&other) noexcept;
    packet_burst(const packet_burst &) = delete;
    packet_burst &operator=(const packet_burst &) = delete;
    ~packet_burst();

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    packet &operator[](std::size_t i) noexcept { return *slot(i); }
    const packet &operator[](std::size_t i) const noexcept { return *slot(i); }

    packet *begin() noexcept { return slot(0); }
    packet *end() noexcept { return slot(count_); }
    const packet *begin() const noexcept { return slot(0); }
    const packet *end() const noexcept { return slot(count_); }

    operator std::span<packet>() noexcept { return {begin(), count_}; }

private:
    // Move-constructs pkt into the next slot and increments size().
    // Only rx_queue calls this, once per mbuf actually received,
    // immediately after rte_eth_rx_burst() returns.
    void push_back(packet &&pkt) noexcept;

    packet *slot(std::size_t i) noexcept {
        return std::launder(reinterpret_cast<packet *>(storage_ + i * sizeof(packet)));
    }
    const packet *slot(std::size_t i) const noexcept {
        return std::launder(reinterpret_cast<const packet *>(storage_ + i * sizeof(packet)));
    }

    alignas(packet) unsigned char storage_[max_size * sizeof(packet)];
    std::size_t count_ = 0;

    friend class rx_queue;
};

} // namespace dpdk

#endif //PACKET_BURST_H
