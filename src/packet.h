//
// Created by cniew on 8/29/26.
//

#ifndef PACKET_H
#define PACKET_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <rte_mbuf.h>

namespace dpdk {

// Move-only, RAII-owning wrapper around a single rte_mbuf.
class packet {
public:
    // A read-only view over already-laid-out wire bytes -- a UDP/TCP
    // header, an ITCH/OUCH message body, whatever. One concrete type
    // covers all of them since the caller just byte-casts a packed
    // struct or buffer into it; no template needed on append().
    using Payload = std::span<const std::byte>;

    packet() noexcept = default;

    packet(packet &&other) noexcept;
    packet &operator=(packet &&other) noexcept;
    packet(const packet &) = delete;
    packet &operator=(const packet &) = delete;
    ~packet();

    uint8_t *data() const noexcept;
    uint16_t length() const noexcept;

    // Grows the packet by len bytes and returns a pointer to the newly
    // added region for the caller to fill in -- the same role as
    // rte_pktmbuf_append, for building an outgoing packet's payload.
    // Returns nullptr if the pool's buffers aren't big enough for len
    // more bytes.
    uint8_t *append(uint16_t len) noexcept;

    // Grows the packet by payload.size() bytes and copies payload into
    // the newly added region -- for appending a UDP/TCP header or an
    // ITCH/OUCH message whose wire bytes are already laid out
    // contiguously. Returns nullptr (and leaves the packet unchanged)
    // if the pool's buffers aren't big enough for payload.size() more
    // bytes.
    uint8_t *append(Payload payload) noexcept;

    explicit operator bool() const noexcept { return pkt_ != nullptr; }

private:
    // Only packet_pool constructs a packet, by allocating a fresh mbuf.
    explicit packet(rte_mbuf *pkt) noexcept;

    // Relinquishes ownership of the underlying mbuf to the NIC after a
    // successful transmit; this packet becomes empty. Only tx_queue
    // calls this -- a user calling it would get a raw rte_mbuf* back,
    // which defeats the entire point of this type existing.
    rte_mbuf *release() noexcept;

    // Exactly one member, no vtable: packet is standard-layout and
    // layout-compatible with a raw rte_mbuf*. tx_queue::send_burst
    // relies on this to let DPDK read mbuf pointers directly out of a
    // caller's span<packet> (reinterpret_cast to rte_mbuf**) instead of
    // copying through a scratch buffer. Adding a second member here
    // would silently break that.
    rte_mbuf *pkt_ = nullptr;

    friend class packet_pool;
    friend class tx_queue;
};

} // namespace dpdk

#endif //PACKET_H
