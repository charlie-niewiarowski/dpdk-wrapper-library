//
// Created by cniew on 9/3/26.
//

#ifndef UDP_HEADER_H
#define UDP_HEADER_H

#include <cstdint>
#include <rte_ip.h>
#include <rte_udp.h>

#include "packet.h"

namespace dpdk {

// Appends a UDP header to pkt and fills in the ports. dgram_len and
// dgram_cksum depend on the packet's final size -- see
// finalize_udp_header. Ports are host byte order in, network byte
// order on the wire. Returns nullptr if the pool's buffers aren't big
// enough, same failure contract as packet::append.
rte_udp_hdr *build_udp_header(packet &pkt, uint16_t src_port, uint16_t dst_port) noexcept;

// Backfills dgram_len and dgram_cksum once every later append on pkt
// (the payload) is done. ip_hdr must already be finalized via
// finalize_ipv4_header -- the checksum helper reads ip_hdr's
// total_length to work out the UDP payload length.
void finalize_udp_header(const packet &pkt, const rte_ipv4_hdr *ip_hdr, rte_udp_hdr *hdr) noexcept;

} // namespace dpdk

#endif //UDP_HEADER_H
