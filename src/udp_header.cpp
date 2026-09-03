//
// Created by cniew on 9/3/26.
//

#include "udp_header.h"

namespace dpdk {

rte_udp_hdr *build_udp_header(packet &pkt, uint16_t src_port, uint16_t dst_port) noexcept {
    auto *hdr = reinterpret_cast<rte_udp_hdr *>(pkt.append(sizeof(rte_udp_hdr)));
    if (hdr == nullptr) {
        return nullptr;
    }

    hdr->src_port = rte_cpu_to_be_16(src_port);
    hdr->dst_port = rte_cpu_to_be_16(dst_port);
    // dgram_len and dgram_cksum depend on the full packet size and are
    // filled in by finalize_udp_header once the payload is appended.

    return hdr;
}

void finalize_udp_header(const packet &pkt, const rte_ipv4_hdr *ip_hdr, rte_udp_hdr *hdr) noexcept {
    const auto *hdr_bytes = reinterpret_cast<const uint8_t *>(hdr);
    const uint16_t dgram_len = static_cast<uint16_t>(pkt.data() + pkt.length() - hdr_bytes);

    hdr->dgram_len = rte_cpu_to_be_16(dgram_len);
    hdr->dgram_cksum = 0;
    hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, hdr);
}

} // namespace dpdk
