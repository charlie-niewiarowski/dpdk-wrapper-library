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

} // namespace dpdk

#endif //ETHERNET_HEADER_H
