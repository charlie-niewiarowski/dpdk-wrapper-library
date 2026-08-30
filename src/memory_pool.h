//
// Created by cniew on 8/29/26.
//

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <cstdint>
#include <rte_mempool.h>
#include "packet.h"

namespace dpdk {

// Thin RAII wrapper around a single rte_mempool of packet mbufs.
// Exposed directly for advanced users who want manual control over
// pool count, element size, or NUMA placement instead of going
// through packet_pool.
class memory_pool {
public:
    memory_pool(const char *name, unsigned n_mbufs, uint16_t elt_size,
                unsigned socket_id, unsigned cache_size = 256);

    memory_pool(memory_pool &&other) noexcept;
    memory_pool &operator=(memory_pool &&other) noexcept;
    memory_pool(const memory_pool &) = delete;
    memory_pool &operator=(const memory_pool &) = delete;
    ~memory_pool();

    packet get() const;
    rte_mempool *native_handle() const noexcept { return pool_; }
    unsigned socket_id() const noexcept;

private:
    rte_mempool *pool_ = nullptr;

    // Wraps an already-obtained mbuf (e.g. one handed back by
    // rte_eth_rx_burst) as a packet, without allocating. packet's
    // constructor is private to keep memory_pool the single point of
    // packet creation; the queue types reach it through here instead.
    static packet get_packet(rte_mbuf *mbuf) noexcept;

    friend class rx_queue;
    friend class tx_queue;
};

} // namespace dpdk

#endif //MEMORY_POOL_H
