//
// Created by cniew on 8/29/26.
//

#ifndef RUNTIME_H
#define RUNTIME_H

#include <cstdint>
#include <deque>
#include <memory>
#include "packet_pool.h"
#include "port.h"

namespace dpdk {

// Represents the DPDK runtime itself: brings up the EAL (hugepages,
// lcores, PCI/vdev probing) on construction and tears it down on
// destruction. Owns every port (and, transitively, every port's
// packet_pool) configured through it.
//
// Exactly one runtime may exist per process at a time -- constructing a
// second one while one is alive throws, since rte_eal_init/cleanup are
// inherently process-global. That's enforced internally; it is not a
// singleton accessor, and there is no way to reach a runtime except by
// constructing one yourself or being handed a reference to one.
class runtime {
public:
    static constexpr uint16_t default_elt_size = 2048;

    runtime(int argc, char **argv);

    runtime(runtime &&) noexcept;
    runtime &operator=(runtime &&) noexcept;
    runtime(const runtime &) = delete;
    runtime &operator=(const runtime &) = delete;
    ~runtime();

    port &add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                    uint16_t elt_size = default_elt_size);
    port &add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                    std::shared_ptr<packet_pool> pool);
    port &get_port(uint16_t port_id);

    // Escape hatch for a custom pool (a different size class, or one
    // meant to be shared across several ports) without constructing
    // packet_pool directly -- its constructor is private to runtime.
    std::shared_ptr<packet_pool> create_pool(const char *name, unsigned n_mbufs,
                                              uint16_t elt_size, unsigned socket_id,
                                              unsigned cache_size = 256);

private:
    std::shared_ptr<packet_pool> make_default_pool(uint16_t port_id, uint16_t elt_size);

    bool owns_eal_ = true;
    // deque, not vector: add_port returns a reference into this
    // container, and unlike vector, deque never invalidates references
    // to existing elements when growing.
    std::deque<port> ports_;
};

} // namespace dpdk

#endif //RUNTIME_H
