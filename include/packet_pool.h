//
// Created by cniew on 8/29/26.
//

#ifndef PACKET_POOL_H
#define PACKET_POOL_H

#include <cstdint>
#include <rte_mempool.h>
#include "packet.h"

namespace dpdk {

// Thin RAII wrapper around a single rte_mempool of packet mbufs.
// Exposed directly for advanced users who want manual control over
// pool count, element size, or NUMA placement.
//
// Must be destroyed before the instance (EAL) it depends on --
// rte_eal_cleanup() releases the hugepage memory this pool is backed
// by, and must be the last DPDK call made. instance guarantees this
// ordering for a pool owned by a port; a standalone packet_pool (as
// used here) is the caller's own scoping responsibility.
class packet_pool {
public:
    packet_pool(packet_pool &&other) noexcept;
    packet_pool &operator=(packet_pool &&other) noexcept;
    packet_pool(const packet_pool &) = delete;
    packet_pool &operator=(const packet_pool &) = delete;
    ~packet_pool();

    packet get() const;
    unsigned socket_id() const noexcept;

private:
    // Only runtime constructs a packet_pool (directly for a port's
    // default pool, or via runtime::create_pool() for a caller-managed
    // one), since runtime is the object responsible for making sure
    // every pool is freed before the EAL is torn down.
    packet_pool(const char *name, unsigned n_mbufs, uint16_t elt_size,
                unsigned socket_id, unsigned cache_size = 256);

    // Raw rte_mempool* access for the one internal collaborator that
    // needs it (port, to hand it to rte_eth_rx_queue_setup). Not public:
    // a user needing the raw mempool means this abstraction failed them.
    rte_mempool *native_handle() const noexcept { return pool_; }

    rte_mempool *pool_ = nullptr;

    // Wraps an already-obtained mbuf as a packet, without allocating.
    // packet's constructor is private to keep packet_pool the single
    // point of packet creation; used internally by get(), and by
    // rx_queue to wrap mbufs handed back by rte_eth_rx_burst.
    static packet get_packet(rte_mbuf *mbuf) noexcept;

    friend class rx_queue;
    friend class port;
    friend class runtime;
};

} // namespace dpdk

#endif //PACKET_POOL_H
