//
// ARP for IPv4 over Ethernet; see arp.h.
//

#include "arp.h"

#include <bit>
#include <cstring>
#include <rte_arp.h>
#include <rte_byteorder.h>

#include "ethernet_header.h"

namespace dpdk {

namespace {

// Fibonacci hashing: spreads consecutive addresses in one subnet (the
// common case here) across the table instead of clustering them.
std::size_t hash_ip(uint32_t ip) noexcept {
    return static_cast<std::size_t>(ip * 2654435769u);
}

// Fills every field of an IPv4-over-Ethernet ARP body. Shared by the
// request and reply builders, which differ only in opcode and addresses.
void fill_arp(rte_arp_hdr *arp, uint16_t opcode, const rte_ether_addr &sha, uint32_t sip,
              const rte_ether_addr &tha, uint32_t tip) noexcept {
    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp->arp_plen = sizeof(uint32_t);
    arp->arp_opcode = rte_cpu_to_be_16(opcode);
    arp->arp_data.arp_sha = sha;
    arp->arp_data.arp_sip = rte_cpu_to_be_32(sip);
    arp->arp_data.arp_tha = tha;
    arp->arp_data.arp_tip = rte_cpu_to_be_32(tip);
}

packet build_arp(const packet_pool &pool, const rte_ether_addr &eth_dst, uint16_t opcode,
                 const rte_ether_addr &sha, uint32_t sip, const rte_ether_addr &tha,
                 uint32_t tip) noexcept {
    packet pkt = pool.get();
    if (!pkt) {
        return pkt;
    }
    if (build_ethernet_header(pkt, eth_dst, sha, RTE_ETHER_TYPE_ARP) == nullptr) {
        return packet{};
    }
    auto *arp = reinterpret_cast<rte_arp_hdr *>(pkt.append(sizeof(rte_arp_hdr)));
    if (arp == nullptr) {
        return packet{};
    }
    fill_arp(arp, opcode, sha, sip, tha, tip);
    return pkt;
}

} // namespace

neighbor_table::neighbor_table(std::size_t capacity)
    : slots_(std::bit_ceil(capacity * 2 < 2 ? 2 : capacity * 2)),
      mask_(slots_.size() - 1),
      capacity_(capacity) {}

neighbor_table::entry *neighbor_table::find_slot(uint32_t ip) noexcept {
    // Terminates: size_ <= capacity_ < slots_.size(), so at least one
    // slot is always empty.
    for (std::size_t i = hash_ip(ip) & mask_;; i = (i + 1) & mask_) {
        if (slots_[i].ip == ip || slots_[i].ip == 0) {
            return &slots_[i];
        }
    }
}

const neighbor_table::entry *neighbor_table::find_slot(uint32_t ip) const noexcept {
    return const_cast<neighbor_table *>(this)->find_slot(ip);
}

bool neighbor_table::learn(uint32_t ip, const rte_ether_addr &mac) noexcept {
    if (ip == 0) {
        return false;
    }
    entry *e = find_slot(ip);
    if (e->ip == 0) {
        if (size_ >= capacity_) {
            return false;
        }
        e->ip = ip;
        ++size_;
    }
    e->mac = mac;
    return true;
}

bool neighbor_table::update_if_present(uint32_t ip, const rte_ether_addr &mac) noexcept {
    if (ip == 0) {
        return false;
    }
    entry *e = find_slot(ip);
    if (e->ip != ip) {
        return false;
    }
    e->mac = mac;
    return true;
}

const rte_ether_addr *neighbor_table::lookup(uint32_t ip) const noexcept {
    if (ip == 0) {
        return nullptr;
    }
    const entry *e = find_slot(ip);
    return e->ip == ip ? &e->mac : nullptr;
}

arp_result handle_arp(const packet &pkt, const rte_ether_addr &local_mac, uint32_t local_ip,
                      neighbor_table &neighbors, const packet_pool &pool,
                      packet &reply) noexcept {
    if (pkt.length() < sizeof(rte_ether_hdr)) {
        return arp_result::not_arp;
    }
    const auto *eth = reinterpret_cast<const rte_ether_hdr *>(pkt.data());
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
        return arp_result::not_arp;
    }
    if (pkt.length() < sizeof(rte_ether_hdr) + sizeof(rte_arp_hdr)) {
        return arp_result::ignored;
    }

    const auto *arp = reinterpret_cast<const rte_arp_hdr *>(eth + 1);
    if (arp->arp_hardware != rte_cpu_to_be_16(RTE_ARP_HRD_ETHER) ||
        arp->arp_protocol != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
        arp->arp_hlen != RTE_ETHER_ADDR_LEN || arp->arp_plen != sizeof(uint32_t)) {
        return arp_result::ignored;
    }

    const uint16_t opcode = rte_be_to_cpu_16(arp->arp_opcode);
    const uint32_t sip = rte_be_to_cpu_32(arp->arp_data.arp_sip);
    const uint32_t tip = rte_be_to_cpu_32(arp->arp_data.arp_tip);
    const rte_ether_addr sha = arp->arp_data.arp_sha;
    const bool for_us = tip == local_ip;

    // RFC 826 merge: refresh if known; add only if we're the target.
    const bool learned = for_us ? neighbors.learn(sip, sha) : neighbors.update_if_present(sip, sha);

    if (for_us && opcode == RTE_ARP_OP_REQUEST) {
        // Unicast straight back to the asker. arp_tha in a request is
        // meaningless (usually zero), so it's rebuilt from the sender.
        reply = build_arp(pool, sha, RTE_ARP_OP_REPLY, local_mac, local_ip, sha, sip);
        return reply ? arp_result::replied : arp_result::learned;
    }
    return learned ? arp_result::learned : arp_result::ignored;
}

packet build_arp_request(const packet_pool &pool, const rte_ether_addr &local_mac,
                         uint32_t local_ip, uint32_t target_ip) noexcept {
    rte_ether_addr broadcast;
    std::memset(&broadcast, 0xff, sizeof(broadcast));
    return build_arp(pool, broadcast, RTE_ARP_OP_REQUEST, local_mac, local_ip,
                     rte_ether_addr{}, target_ip);
}

} // namespace dpdk
