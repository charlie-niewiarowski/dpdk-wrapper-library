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
// the packet_pool those RX queues pull from. Queue count is a runtime
// constructor value (not a template parameter), matching how DPDK
// itself configures these at runtime.
//
// Only runtime constructs a port (see runtime::add_port) -- it's the
// object responsible for making sure every port is torn down before
// the EAL is. The pool is a shared_ptr so multiple ports can share one
// (e.g. two NICs on the same NUMA socket); get_pool() is how you'd
// obtain it from one port to hand to another.
//
// Manual per-queue access is available via claim_rx_queue/claim_tx_queue
// for callers that want to pin a queue to a specific thread/core and
// poll it directly -- the returned shared_ptr is safe to move into a
// worker thread. receive_burst/send_burst here are a convenience
// forwarding path for callers who don't need that.
class port {
public:
    port(port &&other) noexcept;
    port &operator=(port &&other) noexcept;
    port(const port &) = delete;
    port &operator=(const port &) = delete;
    ~port();

    std::shared_ptr<packet_pool> get_pool() const noexcept { return pool_; }

    // Hands back the next not-yet-claimed queue, or an empty shared_ptr
    // once every configured queue has been claimed. The natural fit for
    // RSS: each worker/core takes one queue and the specific numeric id
    // never has to be named at the call site. Meant to be called from a
    // single setup thread before workers are spawned, not concurrently
    // from multiple threads.
    std::shared_ptr<rx_queue> claim_rx_queue();
    std::shared_ptr<tx_queue> claim_tx_queue();

    packet_burst receive_burst(uint16_t queue_id,
                                std::size_t max_count = packet_burst::max_size) const;
    uint16_t send_burst(uint16_t queue_id, std::span<packet> pkts) const;

    uint16_t port_id() const noexcept;

private:
    port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
         std::shared_ptr<packet_pool> pool);

    uint16_t port_id_;
    std::shared_ptr<packet_pool> pool_;
    // rx_queue/tx_queue constructors are private (friend port), so these
    // are built with shared_ptr(new rx_queue(...)) rather than
    // make_shared, which needs constructor access from outside the
    // class it's allocating.
    std::vector<std::shared_ptr<rx_queue>> rx_queues_;
    std::vector<std::shared_ptr<tx_queue>> tx_queues_;
    std::size_t next_rx_claim_ = 0;
    std::size_t next_tx_claim_ = 0;
    // False for a moved-from port, so its destructor doesn't stop/close a
    // device now owned by whatever it was moved into.
    bool owns_device_ = true;

    friend class runtime;
};

} // namespace dpdk

#endif //PORT_H
