//
// Created by cniew on 8/29/26.
//

#ifndef PORT_H
#define PORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "packet.h"
#include "packet_burst.h"
#include "packet_pool.h"
#include "rx_queue.h"
#include "tx_queue.h"

namespace dpdk {

// Owns one physical NIC end to end: an rte_ethdev's RX/TX queues, and
// the packet_pool those RX queues pull from (sized for the device's own
// NUMA socket). Queue count and pool element size are runtime
// constructor values (not template parameters), matching how DPDK
// itself configures these at runtime.
//
// The pool is a shared_ptr so multiple ports can share one packet_pool
// (e.g. two NICs on the same NUMA socket) by passing the same pointer
// to each; get_pool() is how you'd obtain it from one port to hand to
// another. The constructor taking just elt_size builds and owns its
// own pool via make_shared instead.
//
// Manual per-queue access is available via get_rx_queue/get_tx_queue
// for callers that want to pin a queue to a specific thread/core and
// poll it directly. receive_burst/send_burst here are a convenience
// forwarding path for callers who don't need that.
class port {
public:
    static constexpr uint16_t default_elt_size = 2048;

    port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
         uint16_t elt_size = default_elt_size);
    port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
         std::shared_ptr<packet_pool> pool);

    port(port &&other) noexcept;
    port &operator=(port &&other) noexcept;
    port(const port &) = delete;
    port &operator=(const port &) = delete;
    ~port();

    std::shared_ptr<rx_queue> get_rx_queue(uint16_t queue_id) const;
    std::shared_ptr<tx_queue> get_tx_queue(uint16_t queue_id) const;
    std::shared_ptr<packet_pool> get_pool() const noexcept { return pool_; }

    packet_burst receive_burst(uint16_t queue_id,
                                std::size_t max_count = packet_burst::max_size) const;
    uint16_t send_burst(uint16_t queue_id, std::span<packet> pkts) const;

    uint16_t port_id() const noexcept;

private:
    uint16_t port_id_;
    std::shared_ptr<packet_pool> pool_;
    // rx_queue/tx_queue constructors are private (friend port), so these
    // are built with shared_ptr(new rx_queue(...)) rather than
    // make_shared, which needs constructor access from outside the
    // class it's allocating.
    std::vector<std::shared_ptr<rx_queue>> rx_queues_;
    std::vector<std::shared_ptr<tx_queue>> tx_queues_;
    // False for a moved-from port, so its destructor doesn't stop/close a
    // device now owned by whatever it was moved into.
    bool owns_device_ = true;
};

} // namespace dpdk

#endif //PORT_H
