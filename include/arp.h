//
// ARP for IPv4 over Ethernet (RFC 826): answering requests for a port's
// own address, and remembering which MAC each neighbor uses.
//

#ifndef ARP_H
#define ARP_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <rte_ether.h>

#include "packet.h"
#include "packet_pool.h"

namespace dpdk {

// Fixed-capacity IPv4 -> MAC map. Open addressing with linear probing
// over a power-of-two slot array sized to at least twice capacity at
// construction, so lookups stay short and nothing allocates after
// setup. Single-threaded by design: it belongs to whichever thread polls
// RX (that's where every learning opportunity arrives) -- if another
// thread needs a peer's MAC, hand it a copy rather than sharing the table.
//
// No ageing or eviction: on the single-host setups this targets a peer's
// MAC never changes, and once capacity distinct addresses are known, new
// ones are simply not learned (learn() returns false). Ageing would slot
// in as a timestamp per entry checked in lookup().
class neighbor_table {
public:
    explicit neighbor_table(std::size_t capacity = 256);

    // Inserts or overwrites ip's MAC. Returns false (and learns nothing)
    // if ip is 0.0.0.0 -- the "unconfigured sender" address ARP probes
    // use, never a real neighbor -- or if the table is full.
    bool learn(uint32_t ip, const rte_ether_addr &mac) noexcept;

    // Overwrites ip's MAC only if ip is already known. This is RFC 826's
    // "merge" step: any ARP packet refreshes an existing entry, but only
    // traffic actually addressed to us is allowed to add new ones.
    bool update_if_present(uint32_t ip, const rte_ether_addr &mac) noexcept;

    // Pointer into the table, or nullptr if ip is unknown. Stays valid
    // until the next learn() of the same ip (which rewrites it in place).
    const rte_ether_addr *lookup(uint32_t ip) const noexcept;

    std::size_t size() const noexcept { return size_; }

private:
    struct entry {
        uint32_t ip = 0; // 0 marks an empty slot
        rte_ether_addr mac{};
    };

    entry *find_slot(uint32_t ip) noexcept;
    const entry *find_slot(uint32_t ip) const noexcept;

    std::vector<entry> slots_;
    std::size_t mask_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

enum class arp_result {
    not_arp, // not an ARP frame; pkt is untouched and still the caller's to handle
    ignored, // ARP, but malformed, not IPv4-over-Ethernet, or nothing to learn
    learned, // sender learned/refreshed; no reply needed (or none could be built)
    replied, // a request for local_ip: sender learned and `reply` filled in
};

// Runs one received frame through ARP. Any ARP packet refreshes its
// sender's existing neighbor entry; one targeting local_ip also adds it.
// A request for local_ip additionally gets a reply built into `reply`
// (allocated from pool), for the caller to send on whatever tx queue it
// owns -- this function never transmits, so it imposes no threading of
// its own. If the pool is exhausted the result is still `learned` and
// `reply` stays empty; the peer just re-asks.
//
// local_ip is host byte order. The reply is the bare 42-byte frame;
// padding to the 60-byte Ethernet minimum is left to the NIC, which every
// DPDK PMD (and the tap/ring vdevs) handles.
arp_result handle_arp(const packet &pkt, const rte_ether_addr &local_mac, uint32_t local_ip,
                      neighbor_table &neighbors, const packet_pool &pool,
                      packet &reply) noexcept;

// Builds a broadcast who-has request for target_ip. With target_ip ==
// local_ip it doubles as a gratuitous ARP, announcing local_mac to the
// segment. Returns an empty packet if the pool is exhausted.
packet build_arp_request(const packet_pool &pool, const rte_ether_addr &local_mac,
                         uint32_t local_ip, uint32_t target_ip) noexcept;

} // namespace dpdk

#endif //ARP_H
