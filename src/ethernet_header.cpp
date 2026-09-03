//
// Created by cniew on 9/3/26.
//

#include "ethernet_header.h"

namespace dpdk {

rte_ether_hdr *build_ethernet_header(packet &pkt, const rte_ether_addr &dst,
                                      const rte_ether_addr &src, uint16_t ether_type) noexcept {
    auto *hdr = reinterpret_cast<rte_ether_hdr *>(pkt.append(sizeof(rte_ether_hdr)));
    if (hdr == nullptr) {
        return nullptr;
    }

    hdr->dst_addr = dst;
    hdr->src_addr = src;
    hdr->ether_type = rte_cpu_to_be_16(ether_type);

    // No offload use yet, but this is the same bookkeeping main.cpp sets
    // by hand -- keeping it here means an offload path later only needs
    // to add ol_flags, not rediscover where l2_len belongs.
    pkt.set_l2_len(sizeof(rte_ether_hdr));

    return hdr;
}

} // namespace dpdk
