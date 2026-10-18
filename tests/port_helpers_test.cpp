#include <vector>

#include <gtest/gtest.h>

#include <rte_ethdev.h>
#include <rte_ip.h>

#include "dpdk_error.h"
#include "ethernet_header.h"
#include "test_env.h"

namespace {

TEST(ipv4_multicast_mac, maps_low_23_bits) {
    // 239.1.2.3 -> 01:00:5e:01:02:03
    constexpr rte_ether_addr mac = dpdk::ipv4_multicast_mac(RTE_IPV4(239, 1, 2, 3));
    const rte_ether_addr expected{{0x01, 0x00, 0x5e, 0x01, 0x02, 0x03}};
    EXPECT_TRUE(rte_is_same_ether_addr(&mac, &expected));

    // The 24th bit is dropped: 224.129.2.3 and 224.1.2.3 share a MAC.
    const rte_ether_addr hi = dpdk::ipv4_multicast_mac(RTE_IPV4(224, 129, 2, 3));
    const rte_ether_addr lo = dpdk::ipv4_multicast_mac(RTE_IPV4(224, 1, 2, 3));
    EXPECT_TRUE(rte_is_same_ether_addr(&hi, &lo));
    EXPECT_TRUE(rte_is_multicast_ether_addr(&hi));
}

TEST(port, mac_address_matches_the_device) {
    // net_ring reports an all-zero MAC, so "looks like a valid unicast
    // MAC" isn't checkable here -- compare against the device directly.
    const rte_ether_addr mac = dpdk_test::get().proto.mac_address();
    rte_ether_addr raw{};
    ASSERT_EQ(rte_eth_macaddr_get(0, &raw), 0);
    EXPECT_TRUE(rte_is_same_ether_addr(&mac, &raw));
}

TEST(port, ipv4_address_is_unset_until_set) {
    dpdk::port &p = dpdk_test::get().claims;
    EXPECT_FALSE(p.ipv4_address().has_value());
    p.set_ipv4_address(RTE_IPV4(10, 0, 0, 2));
    EXPECT_EQ(p.ipv4_address(), RTE_IPV4(10, 0, 0, 2));
}

// Port 1's queues are consumed for good here, so this is one test
// exercising the whole mixed-claim sequence in order.
TEST(port, claim_by_id_and_next_unclaimed_never_overlap) {
    dpdk::port &p = dpdk_test::get().claims;

    auto rx2 = p.claim_rx_queue(uint16_t{2});
    ASSERT_TRUE(rx2);
    EXPECT_EQ(rx2->queue_id(), 2);
    EXPECT_FALSE(p.claim_rx_queue(uint16_t{2})) << "same id handed out twice";

    // Next-unclaimed hands out 0, 1, skips 2, then 3, then nothing.
    std::vector<uint16_t> ids;
    while (auto q = p.claim_rx_queue()) {
        ids.push_back(q->queue_id());
    }
    EXPECT_EQ(ids, (std::vector<uint16_t>{0, 1, 3}));
    EXPECT_FALSE(p.claim_rx_queue(uint16_t{0})) << "claimed by the no-arg form";

    EXPECT_THROW(p.claim_tx_queue(uint16_t{dpdk_test::n_claim_queues}), dpdk::dpdk_error);
    auto tx0 = p.claim_tx_queue();
    ASSERT_TRUE(tx0);
    EXPECT_EQ(tx0->queue_id(), 0);
    EXPECT_FALSE(p.claim_tx_queue(uint16_t{0}));
    EXPECT_TRUE(p.claim_tx_queue(uint16_t{3}));
}

} // namespace
