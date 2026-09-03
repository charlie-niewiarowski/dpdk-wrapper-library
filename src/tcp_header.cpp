//
// Created by cniew on 9/3/26.
//

#include "tcp_header.h"

#include <cstring>

namespace dpdk {

rte_tcp_hdr *build_tcp_header(packet &pkt, uint16_t src_port, uint16_t dst_port, uint32_t seq,
                               uint32_t ack, uint8_t flags, uint16_t window) noexcept {
    auto *hdr = reinterpret_cast<rte_tcp_hdr *>(pkt.append(sizeof(rte_tcp_hdr)));
    if (hdr == nullptr) {
        return nullptr;
    }

    std::memset(hdr, 0, sizeof(*hdr));
    hdr->src_port = rte_cpu_to_be_16(src_port);
    hdr->dst_port = rte_cpu_to_be_16(dst_port);
    hdr->sent_seq = rte_cpu_to_be_32(seq);
    hdr->recv_ack = rte_cpu_to_be_32(ack);
    hdr->data_off = 5 << 4; // 20-byte header, no options
    hdr->tcp_flags = flags;
    hdr->rx_win = rte_cpu_to_be_16(window);
    // cksum depends on the full packet size and is filled in by
    // finalize_tcp_header once the payload is appended.

    return hdr;
}

void finalize_tcp_header(const packet &pkt, const rte_ipv4_hdr *ip_hdr, rte_tcp_hdr *hdr) noexcept {
    hdr->cksum = 0;
    hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, hdr);
}

} // namespace dpdk
