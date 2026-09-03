//
// Created by cniew on 8/29/26.
//

#include "rx_queue.h"

#include <rte_ethdev.h>
#include "packet_pool.h"

namespace dpdk {

packet_burst rx_queue::receive_burst(std::size_t max_count) const {
    if (max_count > packet_burst::max_size) {
        max_count = packet_burst::max_size;
    }

    // Plain, uninitialized stack array -- exactly what bare
    // rte_eth_rx_burst(..., rte_mbuf **rx_pkts, ...) itself expects, no
    // construction cost. burst only pays for the packets actually
    // received (see packet_burst.h), so it's built by appending exactly
    // n times rather than pre-sized to max_size.
    rte_mbuf *raw[packet_burst::max_size];
    const uint16_t n = rte_eth_rx_burst(port_id_, queue_id_, raw,
                                         static_cast<uint16_t>(max_count));

    packet_burst burst;
    for (uint16_t i = 0; i < n; ++i) {
        burst.push_back(packet_pool::get_packet(raw[i]));
    }
    return burst;
}

} // namespace dpdk
