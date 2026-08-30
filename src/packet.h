//
// Created by cniew on 8/29/26.
//

#ifndef PACKET_H
#define PACKET_H

#include <cstdint>
#include <rte_mbuf.h>

namespace dpdk {

// Move-only, RAII-owning wrapper around a single rte_mbuf.
class packet {
public:
    packet() noexcept = default;

    packet(packet &&other) noexcept;
    packet &operator=(packet &&other) noexcept;
    packet(const packet &) = delete;
    packet &operator=(const packet &) = delete;
    ~packet();

    // Relinquishes ownership of the underlying mbuf (e.g. handing it off
    // to the NIC on a successful transmit). Caller becomes responsible
    // for its lifetime; this packet becomes empty.
    rte_mbuf *release() noexcept;

    rte_mbuf *native_handle() const noexcept { return pkt_; }
    uint8_t *data() const noexcept;
    uint16_t length() const noexcept;

    explicit operator bool() const noexcept { return pkt_ != nullptr; }

private:
    // Only memory_pool constructs a packet — either by allocating a
    // fresh mbuf itself, or by wrapping one handed back by a queue via
    // memory_pool::get_packet.
    explicit packet(rte_mbuf *pkt) noexcept;

    rte_mbuf *pkt_ = nullptr;

    friend class memory_pool;
};

} // namespace dpdk

#endif //PACKET_H
