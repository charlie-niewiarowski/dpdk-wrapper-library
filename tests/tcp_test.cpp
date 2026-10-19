// TCP state machine tests, driven by crafted segments over net_ring.
//
// The test plays the kernel peer: it injects frames on tx queue peer_q,
// the stack's receiver polls rx queue peer_q, and whatever the stack
// sends comes back on rx control_q (the receiver's SYN-ACK/ACK/RST) or
// rx data_q (the sender's data/FIN) for the test to parse -- see
// test_env.h. Everything runs on the test thread; the threading contract
// is exercised by the tap integration test instead.

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

#include <gtest/gtest.h>
#include <rte_ip.h>
#include <rte_tcp.h>

#include "arp.h"
#include "dpdk_error.h"
#include "ethernet_header.h"
#include "ipv4_header.h"
#include "tcp_header.h"
#include "tcp_stack.h"
#include "test_env.h"

namespace {

using dpdk::tcp_event_type;
using dpdk_test::control_q;
using dpdk_test::data_q;
using dpdk_test::peer_q;

constexpr uint32_t local_ip = RTE_IPV4(10, 77, 0, 2);
constexpr uint32_t peer_ip = RTE_IPV4(10, 77, 0, 1);
constexpr uint16_t listen_port = 9000;
constexpr uint16_t peer_port = 40000;
const rte_ether_addr peer_mac{{0x02, 0, 0, 0, 0, 0x01}};

constexpr uint8_t SYN = RTE_TCP_SYN_FLAG;
constexpr uint8_t ACK = RTE_TCP_ACK_FLAG;
constexpr uint8_t FIN = RTE_TCP_FIN_FLAG;
constexpr uint8_t RST = RTE_TCP_RST_FLAG;
constexpr uint8_t PSH = RTE_TCP_PSH_FLAG;

// rte_eal_init pins the calling thread to the main lcore, and every
// std::thread inherits that mask -- left alone, the "concurrent" threads
// below would all time-slice on one core and never truly race.
void unpin_this_thread() {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        CPU_SET(c, &all);
    }
    pthread_setaffinity_np(pthread_self(), sizeof(all), &all);
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

// A segment the stack sent, parsed back off the wire.
struct seen_segment {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t flags;
    uint16_t window;
    std::optional<uint16_t> mss_option;
    std::string payload;
    bool checksums_ok;
    rte_ether_addr eth_dst;
};

// A tcp_event with its payload copied out, so it outlives the burst.
struct seen_event {
    tcp_event_type type;
    dpdk::conn_id conn;
    std::string data;
};

bool checksums_ok(const rte_ipv4_hdr *ip, const rte_tcp_hdr *tcp) {
    if (rte_ipv4_cksum(ip) != 0) {
        return false;
    }
    const uint16_t l4_len =
        rte_be_to_cpu_16(ip->total_length) - static_cast<uint16_t>(rte_ipv4_hdr_len(ip));
    uint32_t sum = rte_ipv4_phdr_cksum(ip, 0);
    sum = __rte_raw_cksum(tcp, l4_len, sum);
    return __rte_raw_cksum_reduce(sum) == 0xffff;
}

class tcp : public ::testing::Test {
protected:
    dpdk_test::env &e = dpdk_test::get();

    void SetUp() override {
        dpdk_test::drain_all();
        e.proto.set_ipv4_address(local_ip);
    }

    // Built per test (not in the fixture constructor) so a test can pass
    // its own tcp_params first.
    void make_stack(dpdk::tcp_params params = {}) {
        stack.emplace(e.proto, params);
        stack->listen(listen_port);
        rx.emplace(stack->receiver(e.tx[control_q]));
        tx.emplace(stack->sender(e.tx[data_q]));
    }

    // Injects one crafted segment from the peer and polls it through the
    // receiver, returning the events it produced.
    std::vector<seen_event> peer_sends(uint32_t seq, uint32_t ack, uint8_t flags,
                                       std::string_view payload = {}, uint16_t window = 65535,
                                       std::optional<uint16_t> mss = std::nullopt,
                                       uint16_t from_port = peer_port, uint16_t to_port = listen_port,
                                       bool corrupt = false) {
        dpdk::packet pkt[1] = {craft(seq, ack, flags, payload, window, mss, from_port, to_port)};
        if (corrupt) {
            pkt[0].data()[pkt[0].length() - 1] ^= 0xff;
        }
        EXPECT_EQ(e.tx[peer_q]->send_burst(pkt), 1);
        return poll();
    }

    std::vector<seen_event> poll() {
        dpdk::packet_burst burst = e.rx[peer_q]->receive_burst();
        std::vector<seen_event> out;
        for (const dpdk::tcp_event &ev : rx->poll(burst)) {
            out.push_back({ev.type, ev.conn,
                           std::string(reinterpret_cast<const char *>(ev.data.data()), ev.data.size())});
        }
        return out;
    }

    // Everything the stack sent on one of its queues since the last call.
    std::vector<seen_segment> sent_on(uint16_t q) {
        std::vector<seen_segment> out;
        for (;;) {
            dpdk::packet_burst burst = e.rx[q]->receive_burst();
            if (burst.empty()) {
                return out;
            }
            for (const dpdk::packet &pkt : burst) {
                out.push_back(parse(pkt));
            }
        }
    }

    // Runs the three-way handshake from the peer's side, checking the
    // SYN-ACK along the way, and returns the accepted connection.
    dpdk::conn_id handshake(uint16_t window = 65535, std::optional<uint16_t> mss = 1460) {
        EXPECT_TRUE(peer_sends(peer_isn, 0, SYN, {}, 65535, mss).empty());
        std::vector<seen_segment> syn_ack = sent_on(control_q);
        EXPECT_EQ(syn_ack.size(), 1u);
        if (syn_ack.empty()) {
            return {};
        }
        EXPECT_EQ(syn_ack[0].flags, SYN | ACK);
        EXPECT_EQ(syn_ack[0].ack, peer_isn + 1);
        iss = syn_ack[0].seq;

        std::vector<seen_event> ev = peer_sends(peer_isn + 1, iss + 1, ACK, {}, window);
        EXPECT_EQ(ev.size(), 1u);
        if (ev.empty()) {
            return {};
        }
        EXPECT_EQ(ev[0].type, tcp_event_type::accepted);
        EXPECT_TRUE(sent_on(control_q).empty()) << "the final ACK is never ACKed";
        return ev[0].conn;
    }

    dpdk::packet craft(uint32_t seq, uint32_t ack, uint8_t flags, std::string_view payload,
                       uint16_t window, std::optional<uint16_t> mss, uint16_t from_port,
                       uint16_t to_port) {
        dpdk::packet pkt = e.proto.get_pool()->get();
        const rte_ether_addr local_mac = e.proto.mac_address();
        dpdk::build_ethernet_header(pkt, local_mac, peer_mac, RTE_ETHER_TYPE_IPV4);
        rte_ipv4_hdr *ip = dpdk::build_ipv4_header(pkt, peer_ip, local_ip, IPPROTO_TCP);
        rte_tcp_hdr *th = dpdk::build_tcp_header(pkt, from_port, to_port, seq, ack, flags, window);
        if (mss) {
            // NOP, NOP, MSS: options needn't start aligned, so make sure
            // the parser walks over padding rather than assuming offset 0.
            uint8_t *opt = pkt.append(8);
            const uint8_t o[8] = {1, 1, 2, 4, static_cast<uint8_t>(*mss >> 8),
                                  static_cast<uint8_t>(*mss), 1, 1};
            std::memcpy(opt, o, sizeof(o));
            th->data_off = 7 << 4;
        }
        pkt.append(bytes(payload));
        dpdk::finalize_ipv4_header(pkt, ip);
        dpdk::finalize_tcp_header(pkt, ip, th);
        return pkt;
    }

    static seen_segment parse(const dpdk::packet &pkt) {
        const auto *eth = reinterpret_cast<const rte_ether_hdr *>(pkt.data());
        const auto *ip = reinterpret_cast<const rte_ipv4_hdr *>(eth + 1);
        const auto *th = reinterpret_cast<const rte_tcp_hdr *>(
            reinterpret_cast<const uint8_t *>(ip) + rte_ipv4_hdr_len(ip));
        const std::size_t doff = (th->data_off >> 4) * 4;
        const std::size_t l4_len = rte_be_to_cpu_16(ip->total_length) - rte_ipv4_hdr_len(ip);
        seen_segment s{rte_be_to_cpu_16(th->src_port), rte_be_to_cpu_16(th->dst_port),
                       rte_be_to_cpu_32(th->sent_seq), rte_be_to_cpu_32(th->recv_ack),
                       th->tcp_flags, rte_be_to_cpu_16(th->rx_win), std::nullopt,
                       std::string(reinterpret_cast<const char *>(th) + doff, l4_len - doff),
                       checksums_ok(ip, th), eth->dst_addr};
        const auto *opt = reinterpret_cast<const uint8_t *>(th + 1);
        if (doff >= 24 && opt[0] == 2 && opt[1] == 4) {
            s.mss_option = static_cast<uint16_t>((opt[2] << 8) | opt[3]);
        }
        return s;
    }

    const uint32_t peer_isn = 0xfffffff0u; // wraps soon: exercises modular seq math
    uint32_t iss = 0;
    std::optional<dpdk::tcp_stack> stack;
    std::optional<dpdk::tcp_receiver> rx;
    std::optional<dpdk::tcp_sender> tx;
};

TEST_F(tcp, syn_ack_is_well_formed) {
    make_stack();
    ASSERT_TRUE(peer_sends(peer_isn, 0, SYN, {}, 65535, 1460).empty());
    std::vector<seen_segment> out = sent_on(control_q);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].flags, SYN | ACK);
    EXPECT_EQ(out[0].ack, peer_isn + 1);
    EXPECT_EQ(out[0].src_port, listen_port);
    EXPECT_EQ(out[0].dst_port, peer_port);
    EXPECT_EQ(out[0].mss_option, stack->mss());
    EXPECT_TRUE(out[0].checksums_ok);
    EXPECT_TRUE(rte_is_same_ether_addr(&out[0].eth_dst, &peer_mac));
    EXPECT_TRUE(out[0].payload.empty());
}

TEST_F(tcp, duplicate_syn_resends_syn_ack) {
    make_stack();
    peer_sends(peer_isn, 0, SYN, {}, 65535, 1460);
    const uint32_t first_iss = sent_on(control_q).at(0).seq;
    peer_sends(peer_isn, 0, SYN, {}, 65535, 1460);
    std::vector<seen_segment> again = sent_on(control_q);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].flags, SYN | ACK);
    EXPECT_EQ(again[0].seq, first_iss) << "same connection, same ISS";
}

TEST_F(tcp, handshake_data_both_ways_then_passive_close) {
    make_stack();
    const dpdk::conn_id conn = handshake();
    uint32_t pseq = peer_isn + 1;

    // Peer -> us.
    std::vector<seen_event> ev = peer_sends(pseq, iss + 1, ACK | PSH, "hello");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, tcp_event_type::data);
    EXPECT_EQ(ev[0].conn, conn);
    EXPECT_EQ(ev[0].data, "hello");
    pseq += 5;
    std::vector<seen_segment> acks = sent_on(control_q);
    ASSERT_EQ(acks.size(), 1u);
    EXPECT_EQ(acks[0].flags, ACK);
    EXPECT_EQ(acks[0].ack, pseq);
    EXPECT_EQ(acks[0].seq, iss + 1);
    EXPECT_TRUE(acks[0].checksums_ok);

    // Us -> peer, piggybacking the ACK.
    EXPECT_EQ(tx->send(conn, bytes("world")), 5u);
    std::vector<seen_segment> data = sent_on(data_q);
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0].seq, iss + 1);
    EXPECT_EQ(data[0].ack, pseq);
    EXPECT_EQ(data[0].flags, ACK | PSH);
    EXPECT_EQ(data[0].payload, "world");
    EXPECT_TRUE(data[0].checksums_ok);

    // Peer ACKs our data and FINs: half-close.
    ev = peer_sends(pseq, iss + 6, ACK | FIN);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, tcp_event_type::peer_closed);
    acks = sent_on(control_q);
    ASSERT_EQ(acks.size(), 1u);
    EXPECT_EQ(acks[0].ack, pseq + 1) << "FIN occupies one sequence number";

    // Still allowed to send in CLOSE_WAIT.
    EXPECT_EQ(tx->send(conn, bytes("bye")), 3u);
    ASSERT_EQ(sent_on(data_q).size(), 1u);

    // Our FIN, then the peer's final ACK.
    tx->close(conn);
    std::vector<seen_segment> fin = sent_on(data_q);
    ASSERT_EQ(fin.size(), 1u);
    EXPECT_EQ(fin[0].flags, FIN | ACK);
    EXPECT_EQ(fin[0].seq, iss + 9);
    EXPECT_EQ(fin[0].ack, pseq + 1);

    ev = peer_sends(pseq + 1, iss + 10, ACK);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, tcp_event_type::closed);
    EXPECT_TRUE(sent_on(control_q).empty());

    // The tuple is gone: a straggler now gets a RST.
    peer_sends(pseq + 1, iss + 10, ACK);
    std::vector<seen_segment> rst = sent_on(control_q);
    ASSERT_EQ(rst.size(), 1u);
    EXPECT_EQ(rst[0].flags, RST);
    EXPECT_EQ(rst[0].seq, iss + 10);
}

TEST_F(tcp, active_close) {
    make_stack();
    const dpdk::conn_id conn = handshake();
    const uint32_t pseq = peer_isn + 1;

    tx->close(conn);
    std::vector<seen_segment> fin = sent_on(data_q);
    ASSERT_EQ(fin.size(), 1u);
    EXPECT_EQ(fin[0].flags, FIN | ACK);
    EXPECT_EQ(fin[0].seq, iss + 1);

    // FIN_WAIT_2: the peer ACKs our FIN but can still send.
    EXPECT_TRUE(peer_sends(pseq, iss + 2, ACK).empty());
    std::vector<seen_event> ev = peer_sends(pseq, iss + 2, ACK, "late");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].data, "late");
    sent_on(control_q);

    // Peer's FIN completes it, and is ACKed.
    ev = peer_sends(pseq + 4, iss + 2, ACK | FIN);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, tcp_event_type::closed);
    std::vector<seen_segment> ack = sent_on(control_q);
    ASSERT_EQ(ack.size(), 1u);
    EXPECT_EQ(ack[0].ack, pseq + 5);
    EXPECT_EQ(ack[0].seq, iss + 2);
}

TEST_F(tcp, simultaneous_close) {
    make_stack();
    const dpdk::conn_id conn = handshake();
    const uint32_t pseq = peer_isn + 1;

    tx->close(conn);
    sent_on(data_q);
    // Peer's FIN crosses ours (doesn't ACK it): CLOSING.
    EXPECT_TRUE(peer_sends(pseq, iss + 1, ACK | FIN).empty());
    EXPECT_EQ(sent_on(control_q).at(0).ack, pseq + 1);
    // Then it ACKs our FIN.
    std::vector<seen_event> ev = peer_sends(pseq + 1, iss + 2, ACK);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, tcp_event_type::closed);
}

TEST_F(tcp, final_ack_carrying_data_and_fin) {
    make_stack();
    peer_sends(peer_isn, 0, SYN, {}, 65535, 1460);
    iss = sent_on(control_q).at(0).seq;
    std::vector<seen_event> ev = peer_sends(peer_isn + 1, iss + 1, ACK | PSH | FIN, "x");
    ASSERT_EQ(ev.size(), 3u);
    EXPECT_EQ(ev[0].type, tcp_event_type::accepted);
    EXPECT_EQ(ev[1].type, tcp_event_type::data);
    EXPECT_EQ(ev[1].data, "x");
    EXPECT_EQ(ev[2].type, tcp_event_type::peer_closed);
    std::vector<seen_segment> ack = sent_on(control_q);
    ASSERT_EQ(ack.size(), 1u) << "one coalesced ACK covers data and FIN";
    EXPECT_EQ(ack[0].ack, peer_isn + 1 + 1 + 1);
}

TEST_F(tcp, acks_are_coalesced_per_poll) {
    make_stack();
    handshake();
    uint32_t pseq = peer_isn + 1;
    dpdk::packet pkts[3];
    for (auto &p : pkts) {
        p = craft(pseq, iss + 1, ACK, "abcd", 65535, std::nullopt, peer_port, listen_port);
        pseq += 4;
    }
    ASSERT_EQ(e.tx[peer_q]->send_burst(pkts), 3);
    std::vector<seen_event> ev = poll();
    ASSERT_EQ(ev.size(), 3u);
    std::vector<seen_segment> acks = sent_on(control_q);
    ASSERT_EQ(acks.size(), 1u);
    EXPECT_EQ(acks[0].ack, pseq);
}

TEST_F(tcp, reset_tears_down_and_send_stops) {
    make_stack();
    const dpdk::conn_id conn = handshake();
    const uint32_t pseq = peer_isn + 1;

    // Out of window: ignored.
    EXPECT_TRUE(peer_sends(pseq + 100000, 0, RST).empty());
    EXPECT_GT(tx->send_space(conn), 0u);

    std::vector<seen_event> ev = peer_sends(pseq, 0, RST);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, tcp_event_type::reset);
    EXPECT_EQ(ev[0].conn, conn);
    EXPECT_EQ(rx->stats().rst_received, 1u);
    EXPECT_TRUE(sent_on(control_q).empty()) << "never answer a RST";

    EXPECT_EQ(tx->send(conn, bytes("nope")), 0u);
    EXPECT_EQ(tx->send_space(conn), 0u);
    EXPECT_FALSE(tx->sendable(conn));
    tx->close(conn);
    EXPECT_TRUE(sent_on(data_q).empty()) << "no FIN into a reset connection";
}

TEST_F(tcp, rst_in_syn_received_recycles_silently) {
    make_stack(dpdk::tcp_params{.max_connections = 1});
    peer_sends(peer_isn, 0, SYN, {}, 65535, 1460);
    sent_on(control_q);
    EXPECT_TRUE(peer_sends(peer_isn + 1, 0, RST).empty());
    // The only slot is free again.
    peer_sends(peer_isn + 50, 0, SYN, {}, 65535, 1460, peer_port + 1);
    std::vector<seen_segment> out = sent_on(control_q);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].flags, SYN | ACK);
}

TEST_F(tcp, unknown_connections_get_rst) {
    make_stack();
    // An ACK for nothing: RST with seq taken from its ack.
    peer_sends(123, 456, ACK, "zz");
    std::vector<seen_segment> out = sent_on(control_q);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].flags, RST);
    EXPECT_EQ(out[0].seq, 456u);
    EXPECT_TRUE(out[0].checksums_ok);

    // A SYN to a port nobody listens on: RST|ACK acknowledging the SYN.
    peer_sends(1000, 0, SYN, {}, 65535, 1460, peer_port, 9999);
    out = sent_on(control_q);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].flags, RST | ACK);
    EXPECT_EQ(out[0].ack, 1001u);
    EXPECT_EQ(out[0].src_port, 9999);

    // A stray RST is never answered.
    peer_sends(1, 0, RST);
    EXPECT_TRUE(sent_on(control_q).empty());
    EXPECT_EQ(rx->stats().rst_sent, 2u);
}

TEST_F(tcp, full_table_refuses_with_rst) {
    make_stack(dpdk::tcp_params{.max_connections = 1});
    handshake();
    peer_sends(5000, 0, SYN, {}, 65535, 1460, peer_port + 1);
    std::vector<seen_segment> out = sent_on(control_q);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].flags, RST | ACK);
    EXPECT_EQ(rx->stats().no_free_slot, 1u);
}

TEST_F(tcp, segments_to_peer_mss) {
    make_stack();
    const dpdk::conn_id conn = handshake(65535, /*mss=*/100);
    const std::string msg(350, 'm');
    EXPECT_EQ(tx->send(conn, bytes(msg)), 350u);

    std::vector<seen_segment> segs = sent_on(data_q);
    ASSERT_EQ(segs.size(), 4u);
    uint32_t expect_seq = iss + 1;
    std::string reassembled;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        EXPECT_EQ(segs[i].seq, expect_seq);
        EXPECT_EQ(segs[i].payload.size(), i < 3 ? 100u : 50u);
        EXPECT_EQ((segs[i].flags & PSH) != 0, i == 3) << "PSH only on the last";
        EXPECT_TRUE(segs[i].checksums_ok);
        expect_seq += static_cast<uint32_t>(segs[i].payload.size());
        reassembled += segs[i].payload;
    }
    EXPECT_EQ(reassembled, msg);
}

TEST_F(tcp, missing_mss_option_defaults_to_536) {
    make_stack();
    const dpdk::conn_id conn = handshake(65535, std::nullopt);
    const std::string msg(600, 'q');
    EXPECT_EQ(tx->send(conn, bytes(msg)), 600u);
    std::vector<seen_segment> segs = sent_on(data_q);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].payload.size(), 536u);
}

TEST_F(tcp, our_mss_caps_a_larger_peer_mss) {
    make_stack(dpdk::tcp_params{.mss = 200});
    const dpdk::conn_id conn = handshake(65535, 1460);
    EXPECT_EQ(tx->send(conn, bytes(std::string(450, 'z'))), 450u);
    std::vector<seen_segment> segs = sent_on(data_q);
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_EQ(segs[0].payload.size(), 200u);
}

TEST_F(tcp, sending_is_limited_by_peer_window) {
    make_stack();
    const dpdk::conn_id conn = handshake(/*window=*/1000);
    uint32_t pseq = peer_isn + 1;
    const std::string msg(3000, 'w');

    EXPECT_EQ(tx->send_space(conn), 1000u);
    EXPECT_EQ(tx->send(conn, bytes(msg)), 1000u);
    EXPECT_EQ(tx->send_space(conn), 0u);
    EXPECT_EQ(tx->send(conn, bytes(msg).subspan(1000)), 0u);
    std::size_t on_wire = 0;
    for (const auto &s : sent_on(data_q)) {
        on_wire += s.payload.size();
    }
    EXPECT_EQ(on_wire, 1000u);

    // The peer ACKs half and keeps a 1000-byte window: 500 more fit.
    peer_sends(pseq, iss + 1 + 500, ACK, {}, 1000);
    EXPECT_EQ(tx->send_space(conn), 500u);

    // A pure window update (same ACK, bigger window) opens it further.
    peer_sends(pseq, iss + 1 + 500, ACK, {}, 4000);
    EXPECT_EQ(tx->send_space(conn), 3500u);
    EXPECT_EQ(tx->send(conn, bytes(msg).subspan(1000)), 2000u);
    std::vector<seen_segment> rest = sent_on(data_q);
    ASSERT_FALSE(rest.empty());
    EXPECT_EQ(rest.front().seq, iss + 1 + 1000);

    // A zero window stops sending entirely.
    peer_sends(pseq, iss + 1 + 3000, ACK, {}, 0);
    EXPECT_EQ(tx->send_space(conn), 0u);
    EXPECT_EQ(tx->send(conn, bytes("x")), 0u);
    EXPECT_TRUE(tx->sendable(conn)) << "window full is not dead";
}

TEST_F(tcp, out_of_order_is_dropped_and_rcv_nxt_reacked) {
    make_stack();
    const dpdk::conn_id conn = handshake();
    const uint32_t pseq = peer_isn + 1;

    EXPECT_TRUE(peer_sends(pseq + 10, iss + 1, ACK, "later").empty());
    EXPECT_EQ(rx->stats().out_of_order, 1u);
    std::vector<seen_segment> ack = sent_on(control_q);
    ASSERT_EQ(ack.size(), 1u);
    EXPECT_EQ(ack[0].ack, pseq);

    // In-order now.
    std::vector<seen_event> ev = peer_sends(pseq, iss + 1, ACK, "abcde");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].data, "abcde");
    sent_on(control_q);

    // A retransmission overlapping 2 delivered bytes delivers only the new 3.
    ev = peer_sends(pseq + 3, iss + 1, ACK, "deFGH");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].conn, conn);
    EXPECT_EQ(ev[0].data, "FGH");
    EXPECT_EQ(sent_on(control_q).at(0).ack, pseq + 8);

    // A pure duplicate delivers nothing but is re-ACKed.
    EXPECT_TRUE(peer_sends(pseq, iss + 1, ACK, "abcde").empty());
    EXPECT_EQ(sent_on(control_q).at(0).ack, pseq + 8);
}

TEST_F(tcp, bad_checksum_is_dropped) {
    make_stack();
    handshake();
    EXPECT_TRUE(peer_sends(peer_isn + 1, iss + 1, ACK, "oops", 65535, std::nullopt, peer_port,
                           listen_port, /*corrupt=*/true)
                    .empty());
    EXPECT_EQ(rx->stats().bad_checksum, 1u);
    EXPECT_TRUE(sent_on(control_q).empty());
}

TEST_F(tcp, ack_of_unsent_data_is_dropped) {
    make_stack();
    handshake();
    EXPECT_TRUE(peer_sends(peer_isn + 1, iss + 500, ACK, "data").empty());
    std::vector<seen_segment> ack = sent_on(control_q);
    ASSERT_EQ(ack.size(), 1u);
    EXPECT_EQ(ack[0].ack, peer_isn + 1) << "payload not accepted";
}

TEST_F(tcp, slot_reuse_rejects_stale_ids) {
    make_stack(dpdk::tcp_params{.max_connections = 1});
    const dpdk::conn_id first = handshake();
    peer_sends(peer_isn + 1, 0, RST);
    tx->close(first);

    // The close() is noticed on the next poll, recycling the slot.
    const dpdk::conn_id second = handshake();
    EXPECT_EQ(second.slot, first.slot);
    EXPECT_NE(second.generation, first.generation);
    EXPECT_EQ(tx->send(first, bytes("stale")), 0u);
    tx->close(first); // no effect on the new connection
    EXPECT_TRUE(sent_on(data_q).empty());
    EXPECT_EQ(tx->send(second, bytes("fresh")), 5u);
    EXPECT_EQ(tx->send(dpdk::conn_id{}, bytes("x")), 0u) << "value-initialized id";
    EXPECT_EQ(tx->send(dpdk::conn_id{7, 1}, bytes("x")), 0u) << "out-of-range slot";
}

TEST_F(tcp, answers_arp) {
    make_stack();
    dpdk::packet req[1] = {dpdk::build_arp_request(*e.proto.get_pool(), peer_mac, peer_ip, local_ip)};
    ASSERT_EQ(e.tx[peer_q]->send_burst(req), 1);
    EXPECT_TRUE(poll().empty());
    dpdk::packet_burst out = e.rx[control_q]->receive_burst();
    ASSERT_EQ(out.size(), 1u);
    const auto *eth = reinterpret_cast<const rte_ether_hdr *>(out[0].data());
    EXPECT_EQ(eth->ether_type, rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP));
    EXPECT_TRUE(rte_is_same_ether_addr(&eth->dst_addr, &peer_mac)) << "reply goes to the asker";
}

TEST_F(tcp, stack_setup_errors) {
    EXPECT_THROW(dpdk::tcp_stack(e.proto, dpdk::tcp_params{.max_connections = 0}).mss(),
                 dpdk::dpdk_error);
    dpdk::tcp_stack s(e.proto);
    EXPECT_THROW(s.receiver(nullptr), dpdk::dpdk_error);
    auto r = s.receiver(e.tx[control_q]);
    EXPECT_THROW(s.receiver(e.tx[control_q]), dpdk::dpdk_error);
    auto t = s.sender(e.tx[data_q]);
    EXPECT_THROW(s.sender(e.tx[data_q]), dpdk::dpdk_error);
    for (uint16_t p = 1; p <= 16; ++p) {
        s.listen(p);
    }
    s.listen(1); // already listening: no-op, not an error
    EXPECT_THROW(s.listen(17), dpdk::dpdk_error);
}

// The real deployment shape: an inbound thread polling the receiver and
// an outbound thread calling send(), concurrently, with the test thread
// as the peer streaming data back and ACKing with a small window so the
// sender keeps stalling on window updates that arrive via the inbound
// thread. Checks both byte streams arrive intact and in order.
TEST_F(tcp, concurrent_inbound_and_outbound_threads) {
    make_stack();
    constexpr uint16_t peer_window = 4096;
    const dpdk::conn_id conn = handshake(peer_window, /*mss=*/700);
    constexpr std::size_t n = 300000;
    std::string to_peer(n, '\0'), to_stack(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        to_peer[i] = static_cast<char>('a' + i % 23);
        to_stack[i] = static_cast<char>('A' + i % 19);
    }

    std::atomic<bool> stop{false};
    std::string got_by_stack;
    std::thread inbound([&] {
        unpin_this_thread();
        while (!stop.load(std::memory_order_relaxed)) {
            dpdk::packet_burst burst = e.rx[peer_q]->receive_burst();
            for (const dpdk::tcp_event &ev : rx->poll(burst)) {
                if (ev.type == tcp_event_type::data) {
                    got_by_stack.append(reinterpret_cast<const char *>(ev.data.data()),
                                        ev.data.size());
                }
            }
        }
    });
    std::thread outbound([&] {
        unpin_this_thread();
        std::size_t off = 0;
        while (off < n && !stop.load(std::memory_order_relaxed)) {
            off += tx->send(conn, bytes(to_peer).subspan(off));
        }
    });

    // Peer loop.
    std::string got_by_peer;
    std::size_t peer_sent = 0, peer_acked = 0;
    const uint32_t pbase = peer_isn + 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool in_order = true;
    while ((got_by_peer.size() < n || peer_acked < n) && std::chrono::steady_clock::now() < deadline) {
        bool got_data = false;
        for (const seen_segment &s : sent_on(data_q)) {
            in_order &= s.seq == iss + 1 + got_by_peer.size();
            got_by_peer += s.payload;
            peer_acked = std::max<std::size_t>(peer_acked, s.ack - pbase);
            got_data = true;
        }
        for (const seen_segment &s : sent_on(control_q)) {
            peer_acked = std::max<std::size_t>(peer_acked, s.ack - pbase);
        }
        // Stream to the stack, bounded by its constant 64K window, each
        // segment ACKing everything received so far.
        const uint32_t ack = iss + 1 + static_cast<uint32_t>(got_by_peer.size());
        if (peer_sent < n && peer_sent - peer_acked < 32768) {
            const std::size_t len = std::min<std::size_t>(1000, n - peer_sent);
            dpdk::packet p[1] = {craft(pbase + static_cast<uint32_t>(peer_sent), ack, ACK,
                                       std::string_view(to_stack).substr(peer_sent, len),
                                       peer_window, std::nullopt, peer_port, listen_port)};
            if (e.tx[peer_q]->send_burst(p) == 1) {
                peer_sent += len;
            }
        } else if (got_data) {
            dpdk::packet p[1] = {craft(pbase + static_cast<uint32_t>(peer_sent), ack, ACK, {},
                                       peer_window, std::nullopt, peer_port, listen_port)};
            e.tx[peer_q]->send_burst(p);
        }
    }
    stop.store(true);
    outbound.join();
    inbound.join();

    EXPECT_TRUE(in_order);
    EXPECT_EQ(got_by_peer.size(), n);
    EXPECT_TRUE(got_by_peer == to_peer) << "outbound stream corrupted";
    EXPECT_EQ(got_by_stack.size(), n);
    EXPECT_TRUE(got_by_stack == to_stack) << "inbound stream corrupted";
    EXPECT_EQ(rx->stats().out_of_order, 0u);
}

} // namespace
