//
// Created by cniew on 9/3/26.
//

#ifndef TCP_HEADER_H
#define TCP_HEADER_H

#include <cstdint>
#include <rte_ip.h>
#include <rte_tcp.h>

#include "packet.h"

namespace dpdk {

// Appends a TCP header to pkt (no options: a fixed 20-byte header) and
// fills in everything except cksum, which depends on the packet's
// final size -- see finalize_tcp_header. flags is a bitwise-OR of
// RTE_TCP_*_FLAG. Ports/seq/ack/window are host byte order in, network
// byte order on the wire. Returns nullptr if the pool's buffers aren't
// big enough, same failure contract as packet::append.
rte_tcp_hdr *build_tcp_header(packet &pkt, uint16_t src_port, uint16_t dst_port, uint32_t seq,
                               uint32_t ack, uint8_t flags, uint16_t window) noexcept;

// Backfills cksum once every later append on pkt (the payload) is
// done. ip_hdr must already be finalized via finalize_ipv4_header --
// the checksum helper reads ip_hdr's total_length to work out the TCP
// payload length.
void finalize_tcp_header(const packet &pkt, const rte_ipv4_hdr *ip_hdr, rte_tcp_hdr *hdr) noexcept;

} // namespace dpdk

#endif //TCP_HEADER_H
