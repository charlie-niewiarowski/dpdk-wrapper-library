//
// Created by cniew on 8/29/26.
//

#ifndef TX_QUEUE_H
#define TX_QUEUE_H

#include <cstdint>
#include <span>
#include "packet.h"

namespace dpdk {

// Move-only handle to a single hardware TX queue (port_id, queue_id).
// port hands this out as a shared_ptr to the singleton associated with
// that queue id, so a worker thread can hold it and send directly
// without going back through port.
class tx_queue {
public:
    tx_queue(tx_queue &&other) noexcept;
    tx_queue &operator=(tx_queue &&other) noexcept;
    tx_queue(const tx_queue &) = delete;
    tx_queue &operator=(const tx_queue &) = delete;
    ~tx_queue();

    // Attempts to send every packet in pkts. Packets actually accepted
    // by the NIC are released (ownership passes to the driver); any
    // left over remain valid, still-owned packets in pkts for the
    // caller to retry or drop. Returns the number actually sent.
    uint16_t send_burst(std::span<packet> pkts) const;

    uint16_t port_id() const noexcept;
    uint16_t queue_id() const noexcept;

private:
    // Only port constructs a queue — it's the sole owner of the
    // (port_id, queue_id) pairs actually configured on the device.
    tx_queue(uint16_t port_id, uint16_t queue_id);

    struct handle {
        uint16_t port_id;
        uint16_t queue_id;
    };
    handle *handle_;

    friend class port;
};

} // namespace dpdk

#endif //TX_QUEUE_H
