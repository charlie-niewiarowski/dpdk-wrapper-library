//
// Created by cniew on 8/29/26.
//

#ifndef PORT_H
#define PORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <rte_ether.h>
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

    // Claims one specific queue by id, for when the id itself matters --
    // e.g. a gateway whose inbound thread must own tx queue 0 for control
    // segments while its outbound thread owns tx queue 1 for data. Throws
    // dpdk_error if queue_id isn't a configured queue (a setup bug, not a
    // runtime condition). Returns an empty shared_ptr if that queue was
    // already claimed by either form; the no-arg form skips ids claimed
    // here, so mixing the two never hands the same queue out twice. Same
    // single-setup-thread rule as above.
    std::shared_ptr<rx_queue> claim_rx_queue(uint16_t queue_id);
    std::shared_ptr<tx_queue> claim_tx_queue(uint16_t queue_id);

    packet_burst receive_burst(uint16_t queue_id,
                                std::size_t max_count = packet_burst::max_size) const;
    uint16_t send_burst(uint16_t queue_id, std::span<packet> pkts) const;

    uint16_t port_id() const noexcept;

    // The device's own MAC (rte_eth_macaddr_get). What ARP replies
    // advertise and what outgoing frames use as their source address.
    // Throws dpdk_error if the driver can't report one.
    rte_ether_addr mac_address() const;

    // The IPv4 address (host byte order) this port answers to: ARP
    // replies are only sent for it, and tcp_stack uses it as the local
    // address. Nothing in DPDK knows about L3 addressing, so this is pure
    // bookkeeping on the port -- unset until the caller sets it. Set it
    // during setup, before anything that reads it is constructed or any
    // worker thread starts.
    void set_ipv4_address(uint32_t addr) noexcept { ipv4_addr_ = addr; }
    std::optional<uint32_t> ipv4_address() const noexcept { return ipv4_addr_; }

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
    // One flag per queue, set by either claim form. vector<char> rather
    // than vector<bool> purely to keep element access a plain byte load.
    std::vector<char> rx_claimed_;
    std::vector<char> tx_claimed_;
    std::optional<uint32_t> ipv4_addr_;
    // False for a moved-from port, so its destructor doesn't stop/close a
    // device now owned by whatever it was moved into.
    bool owns_device_ = true;

    friend class runtime;
};

} // namespace dpdk

#endif //PORT_H
