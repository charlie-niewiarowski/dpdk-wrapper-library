#include <cstring>

#include <gtest/gtest.h>
#include <rte_arp.h>
#include <rte_ip.h>

#include "arp.h"
#include "test_env.h"

namespace {

constexpr uint32_t local_ip = RTE_IPV4(10, 77, 0, 2);
constexpr uint32_t peer_ip = RTE_IPV4(10, 77, 0, 1);
const rte_ether_addr local_mac{{0x02, 0, 0, 0, 0, 0x02}};
const rte_ether_addr peer_mac{{0x02, 0, 0, 0, 0, 0x01}};

const rte_arp_hdr *arp_of(const dpdk::packet &pkt) {
    return reinterpret_cast<const rte_arp_hdr *>(pkt.data() + sizeof(rte_ether_hdr));
}

class arp : public ::testing::Test {
protected:
    const dpdk::packet_pool &pool = *dpdk_test::get().proto.get_pool();
    dpdk::neighbor_table table;
    dpdk::packet reply;
};

TEST_F(arp, request_for_us_is_answered_and_sender_learned) {
    dpdk::packet req = dpdk::build_arp_request(pool, peer_mac, peer_ip, local_ip);
    ASSERT_TRUE(req);

    EXPECT_EQ(dpdk::handle_arp(req, local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::replied);
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply.length(), sizeof(rte_ether_hdr) + sizeof(rte_arp_hdr));

    const auto *eth = reinterpret_cast<const rte_ether_hdr *>(reply.data());
    EXPECT_TRUE(rte_is_same_ether_addr(&eth->dst_addr, &peer_mac)) << "reply is unicast";
    EXPECT_TRUE(rte_is_same_ether_addr(&eth->src_addr, &local_mac));
    EXPECT_EQ(eth->ether_type, rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP));

    const rte_arp_hdr *a = arp_of(reply);
    EXPECT_EQ(rte_be_to_cpu_16(a->arp_opcode), RTE_ARP_OP_REPLY);
    EXPECT_TRUE(rte_is_same_ether_addr(&a->arp_data.arp_sha, &local_mac));
    EXPECT_EQ(rte_be_to_cpu_32(a->arp_data.arp_sip), local_ip);
    EXPECT_TRUE(rte_is_same_ether_addr(&a->arp_data.arp_tha, &peer_mac));
    EXPECT_EQ(rte_be_to_cpu_32(a->arp_data.arp_tip), peer_ip);

    const rte_ether_addr *learned = table.lookup(peer_ip);
    ASSERT_NE(learned, nullptr);
    EXPECT_TRUE(rte_is_same_ether_addr(learned, &peer_mac));
}

TEST_F(arp, request_for_another_host_is_not_answered_or_learned) {
    dpdk::packet req = dpdk::build_arp_request(pool, peer_mac, peer_ip, RTE_IPV4(10, 77, 0, 99));
    EXPECT_EQ(dpdk::handle_arp(req, local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::ignored);
    EXPECT_FALSE(reply);
    EXPECT_EQ(table.lookup(peer_ip), nullptr);
}

TEST_F(arp, traffic_for_others_refreshes_a_known_neighbor) {
    table.learn(peer_ip, rte_ether_addr{{0x02, 0, 0, 0, 0, 0xee}});
    // Gratuitous ARP from the peer announcing a new MAC.
    dpdk::packet garp = dpdk::build_arp_request(pool, peer_mac, peer_ip, peer_ip);
    EXPECT_EQ(dpdk::handle_arp(garp, local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::learned);
    EXPECT_FALSE(reply);
    EXPECT_TRUE(rte_is_same_ether_addr(table.lookup(peer_ip), &peer_mac));
}

TEST_F(arp, non_arp_and_malformed_frames) {
    dpdk::packet ip_frame = pool.get();
    auto *eth = reinterpret_cast<rte_ether_hdr *>(ip_frame.append(sizeof(rte_ether_hdr) + 20));
    std::memset(eth, 0, sizeof(rte_ether_hdr) + 20);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    EXPECT_EQ(dpdk::handle_arp(ip_frame, local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::not_arp);

    // Right EtherType, truncated body.
    dpdk::packet truncated = pool.get();
    eth = reinterpret_cast<rte_ether_hdr *>(truncated.append(sizeof(rte_ether_hdr) + 8));
    std::memset(eth, 0, sizeof(rte_ether_hdr) + 8);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);
    EXPECT_EQ(dpdk::handle_arp(truncated, local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::ignored);

    // A request for us but for a non-IPv4 protocol address.
    dpdk::packet req = dpdk::build_arp_request(pool, peer_mac, peer_ip, local_ip);
    const_cast<rte_arp_hdr *>(arp_of(req))->arp_protocol = rte_cpu_to_be_16(0x86dd);
    EXPECT_EQ(dpdk::handle_arp(req, local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::ignored);
    EXPECT_FALSE(reply);
}

TEST_F(arp, reply_survives_a_trip_through_the_wire) {
    // Same exchange, but over net_ring: request in on the peer queue,
    // reply out on the control queue, read back as the peer would.
    auto &e = dpdk_test::get();
    dpdk_test::drain_all();
    dpdk::packet req[1] = {dpdk::build_arp_request(pool, peer_mac, peer_ip, local_ip)};
    ASSERT_EQ(e.tx[dpdk_test::peer_q]->send_burst(req), 1);

    dpdk::packet_burst in = e.rx[dpdk_test::peer_q]->receive_burst();
    ASSERT_EQ(in.size(), 1u);
    ASSERT_EQ(dpdk::handle_arp(in[0], local_mac, local_ip, table, pool, reply),
              dpdk::arp_result::replied);
    dpdk::packet out[1] = {std::move(reply)};
    ASSERT_EQ(e.tx[dpdk_test::control_q]->send_burst(out), 1);

    dpdk::packet_burst seen = e.rx[dpdk_test::control_q]->receive_burst();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(rte_be_to_cpu_16(arp_of(seen[0])->arp_opcode), RTE_ARP_OP_REPLY);
}

TEST(neighbor_table, capacity_and_zero_address) {
    dpdk::neighbor_table t(2);
    const rte_ether_addr m{{0x02, 0, 0, 0, 0, 1}};
    EXPECT_FALSE(t.learn(0, m));
    EXPECT_TRUE(t.learn(1, m));
    EXPECT_TRUE(t.learn(2, m));
    EXPECT_FALSE(t.learn(3, m)) << "full";
    EXPECT_TRUE(t.learn(2, m)) << "overwrite of a known ip still works when full";
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t.lookup(3), nullptr);
    EXPECT_FALSE(t.update_if_present(3, m));

    // Many addresses in one subnet: every one comes back.
    dpdk::neighbor_table big(200);
    for (uint32_t i = 1; i <= 200; ++i) {
        ASSERT_TRUE(big.learn(RTE_IPV4(10, 0, 0, 0) + i, rte_ether_addr{{0x02, 0, 0, 0, 0,
                                                                          static_cast<uint8_t>(i)}}));
    }
    for (uint32_t i = 1; i <= 200; ++i) {
        const rte_ether_addr *mac = big.lookup(RTE_IPV4(10, 0, 0, 0) + i);
        ASSERT_NE(mac, nullptr);
        EXPECT_EQ(mac->addr_bytes[5], static_cast<uint8_t>(i));
    }
}

} // namespace
