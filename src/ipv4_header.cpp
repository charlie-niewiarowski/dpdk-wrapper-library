//
// Created by cniew on 9/3/26.
//

#include "ipv4_header.h"

#include <cstring>

namespace dpdk {

rte_ipv4_hdr *build_ipv4_header(packet &pkt, uint32_t src_addr, uint32_t dst_addr,
                                 uint8_t next_proto, uint8_t ttl, uint8_t tos) noexcept {
    auto *hdr = reinterpret_cast<rte_ipv4_hdr *>(pkt.append(sizeof(rte_ipv4_hdr)));
    if (hdr == nullptr) {
        return nullptr;
    }

    std::memset(hdr, 0, sizeof(*hdr));
    hdr->version_ihl = RTE_IPV4_VHL_DEF; // version 4, 20-byte header, no options
    hdr->type_of_service = tos;
    hdr->time_to_live = ttl;
    hdr->next_proto_id = next_proto;
    hdr->src_addr = rte_cpu_to_be_32(src_addr);
    hdr->dst_addr = rte_cpu_to_be_32(dst_addr);
    // total_length and hdr_checksum depend on the full packet size and
    // are filled in by finalize_ipv4_header once the payload is appended.

    pkt.set_l3_len(sizeof(rte_ipv4_hdr));

    return hdr;
}

void finalize_ipv4_header(const packet &pkt, rte_ipv4_hdr *hdr) noexcept {
    const auto *hdr_bytes = reinterpret_cast<const uint8_t *>(hdr);
    const uint16_t total_length = static_cast<uint16_t>(pkt.data() + pkt.length() - hdr_bytes);

    hdr->total_length = rte_cpu_to_be_16(total_length);
    hdr->hdr_checksum = 0;
    hdr->hdr_checksum = rte_ipv4_cksum(hdr);
}

} // namespace dpdk
