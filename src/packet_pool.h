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
    packet_pool(const char *name, unsigned n_mbufs, uint16_t elt_size,
                unsigned socket_id, unsigned cache_size = 256);

    packet_pool(packet_pool &&other) noexcept;
    packet_pool &operator=(packet_pool &&other) noexcept;
    packet_pool(const packet_pool &) = delete;
    packet_pool &operator=(const packet_pool &) = delete;
    ~packet_pool();

    packet get() const;
    rte_mempool *native_handle() const noexcept { return pool_; }
    unsigned socket_id() const noexcept;

private:
    rte_mempool *pool_ = nullptr;

    // Wraps an already-obtained mbuf (e.g. one handed back by
    // rte_eth_rx_burst) as a packet, without allocating. packet's
    // constructor is private to keep packet_pool the single point of
    // packet creation; the queue types reach it through here instead.
    static packet get_packet(rte_mbuf *mbuf) noexcept;

    friend class rx_queue;
    friend class tx_queue;
};

} // namespace dpdk

#endif //PACKET_POOL_H
