//
// Created by cniew on 8/29/26.
//

#ifndef PACKET_BURST_H
#define PACKET_BURST_H

#include <array>
#include <cstddef>
#include <span>
#include "packet.h"

namespace dpdk {

// Owns a bounded batch of packets, e.g. everything pulled off an
// rx_queue in one poll. Each packet frees itself individually (packet
// is RAII) so there's nothing extra to clean up here — this exists so
// callers don't have to declare and manage their own receive buffer or
// track how many of it are actually populated.
class packet_burst {
public:
    static constexpr std::size_t max_size = 32;

    packet_burst() noexcept = default;
    packet_burst(packet_burst &&) noexcept = default;
    packet_burst &operator=(packet_burst &&) noexcept = default;
    packet_burst(const packet_burst &) = delete;
    packet_burst &operator=(const packet_burst &) = delete;
    ~packet_burst() = default;

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    packet &operator[](std::size_t i) noexcept { return packets_[i]; }
    const packet &operator[](std::size_t i) const noexcept { return packets_[i]; }

    packet *begin() noexcept { return packets_.data(); }
    packet *end() noexcept { return packets_.data() + count_; }
    const packet *begin() const noexcept { return packets_.data(); }
    const packet *end() const noexcept { return packets_.data() + count_; }

    operator std::span<packet>() noexcept { return {packets_.data(), count_}; }

private:
    std::array<packet, max_size> packets_{};
    std::size_t count_ = 0;

    friend class rx_queue;
};

} // namespace dpdk

#endif //PACKET_BURST_H
