//
// Created by cniew on 8/29/26.
//

#ifndef RX_QUEUE_H
#define RX_QUEUE_H

#include <cstddef>
#include <cstdint>
#include "packet_burst.h"

namespace dpdk {

// Move-only handle to a single hardware RX queue (port_id, queue_id).
// port hands this out as a shared_ptr to the singleton associated with
// that queue id, so a worker thread can hold it and poll directly
// without going back through port.
class rx_queue {
public:
    rx_queue(rx_queue &&other) noexcept;
    rx_queue &operator=(rx_queue &&other) noexcept;
    rx_queue(const rx_queue &) = delete;
    rx_queue &operator=(const rx_queue &) = delete;
    ~rx_queue();

    // Pulls up to max_count packets off the ring (capped at
    // packet_burst::max_size) and returns them as a self-freeing batch —
    // no caller-managed buffer, no manual free after use.
    packet_burst receive_burst(std::size_t max_count = packet_burst::max_size) const;

    uint16_t port_id() const noexcept;
    uint16_t queue_id() const noexcept;

private:
    // Only port constructs a queue — it's the sole owner of the
    // (port_id, queue_id) pairs actually configured on the device.
    rx_queue(uint16_t port_id, uint16_t queue_id);

    struct handle {
        uint16_t port_id;
        uint16_t queue_id;
    };
    handle *handle_;

    friend class port;
};

} // namespace dpdk

#endif //RX_QUEUE_H
