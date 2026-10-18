//
// Created by cniew on 9/3/26.
//

#ifndef ETHERNET_HEADER_H
#define ETHERNET_HEADER_H

#include <cstdint>
#include <rte_ether.h>

#include "packet.h"

namespace dpdk {

// Appends an Ethernet header to pkt and fills in dst/src addresses and
// the EtherType (host byte order in, network byte order on the wire).
// Returns nullptr if the pool's buffers aren't big enough, same failure
// contract as packet::append.
//
// No finalize_ethernet_header exists -- nothing in this header depends
// on layers appended after it.
rte_ether_hdr *build_ethernet_header(packet &pkt, const rte_ether_addr &dst,
                                      const rte_ether_addr &src, uint16_t ether_type) noexcept;

// Maps an IPv4 multicast group (224.0.0.0/4, host byte order) to the
// Ethernet multicast MAC frames for it must be sent to: 01:00:5e followed
// by the group's low 23 bits (RFC 1112 section 6.4). No ARP is involved
// for multicast -- this mapping *is* the resolution -- which is why a
// publisher (e.g. ITCH over UDP multicast) can build frames without ever
// consulting a neighbor table. Five bits of the group are dropped, so 32
// groups share each MAC; receivers filter the rest by IP. Not validated:
// passing a non-multicast address just yields a meaningless MAC.
constexpr rte_ether_addr ipv4_multicast_mac(uint32_t group) noexcept {
    return rte_ether_addr{{0x01, 0x00, 0x5e,
                           static_cast<uint8_t>((group >> 16) & 0x7f),
                           static_cast<uint8_t>((group >> 8) & 0xff),
                           static_cast<uint8_t>(group & 0xff)}};
}

} // namespace dpdk

#endif //ETHERNET_HEADER_H
