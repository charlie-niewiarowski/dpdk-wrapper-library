//
// Created by cniew on 8/29/26.
//

#include "tx_queue.h"

#include <rte_ethdev.h>

namespace dpdk {

uint16_t tx_queue::send_burst(std::span<packet> pkts) const {
    // Same layout-compatibility trick as rx_queue::receive_burst: pkts
    // is a contiguous span of packet, each exactly one rte_mbuf*, so
    // DPDK can read mbuf pointers straight out of the caller's own
    // array. No scratch buffer, no copy pass.
    static_assert(sizeof(packet) == sizeof(rte_mbuf *));

    const uint16_t n = rte_eth_tx_burst(port_id_, queue_id_,
                                         reinterpret_cast<rte_mbuf **>(pkts.data()),
                                         static_cast<uint16_t>(pkts.size()));
    for (uint16_t i = 0; i < n; ++i) {
        pkts[i].release(); // ownership now belongs to the driver
    }
    return n;
}

} // namespace dpdk
