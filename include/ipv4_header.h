//
// Created by cniew on 9/3/26.
//

#ifndef IPV4_HEADER_H
#define IPV4_HEADER_H

#include <cstdint>
#include <rte_ip.h>

#include "packet.h"

namespace dpdk {

// Appends an IPv4 header to pkt (version 4, no options: a fixed
// 20-byte header) and fills in everything except total_length and
// hdr_checksum, which depend on the packet's final size -- see
// finalize_ipv4_header. Addresses are host byte order in, network byte
// order on the wire. Returns nullptr if the pool's buffers aren't big
// enough, same failure contract as packet::append.
rte_ipv4_hdr *build_ipv4_header(packet &pkt, uint32_t src_addr, uint32_t dst_addr,
                                 uint8_t next_proto, uint8_t ttl = 64, uint8_t tos = 0) noexcept;

// Backfills total_length and hdr_checksum once every later append on
// pkt (further headers, then the payload) is done. Must run before
// finalize_udp_header/finalize_tcp_header on the same packet -- their
// checksum depends on total_length being set here first.
void finalize_ipv4_header(const packet &pkt, rte_ipv4_hdr *hdr) noexcept;

} // namespace dpdk

#endif //IPV4_HEADER_H
