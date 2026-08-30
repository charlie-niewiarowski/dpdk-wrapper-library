// DPDK kernel-bypass networking walkthrough.
//
// This program exercises the core DPDK "hello world" API surface: EAL
// bring-up, mbuf pool creation, port/queue configuration, manually building
// an Ethernet/IPv4/UDP packet, transmitting it, and polling for RX.
//
// DPDK bypasses the kernel network stack entirely: a NIC is unbound from its
// kernel driver and bound to a userspace-poll-mode driver (UIO/VFIO), so
// packets move directly between the NIC and hugepage-backed memory that this
// process polls — no syscalls, no interrupts, no sk_buff allocation per
// packet.
//
// Build/run prerequisites (not needed to read the code, only to execute it):
//   - DPDK installed such that `pkg-config libdpdk` resolves.
//   - Hugepages reserved (e.g. `dpdk-hugepages.py --setup 1G`).
//   - At least one NIC bound to a DPDK-compatible driver (e.g. vfio-pci) via
//     `dpdk-devbind.py`, or run with `--vdev=net_null0` / `--vdev=net_tap0`
//     for a NIC-less smoke test.
//   - Root/CAP_SYS_ADMIN for hugepage + device access.

#include <cstdio>
#include <cstring>
#include <csignal>
#include <cinttypes>
#include <string_view>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_cycles.h>

namespace {

constexpr uint16_t kRxRingSize = 1024;
constexpr uint16_t kTxRingSize = 1024;
constexpr uint16_t kNumMbufs = 8192;
constexpr uint16_t kMbufCacheSize = 256;
constexpr uint16_t kBurstSize = 32;

volatile sig_atomic_t g_force_quit = 0;

void HandleSignal(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_force_quit = 1;
    }
}

// Every DPDK port (NIC) needs one RX queue and one TX queue configured
// before it can move packets. This mirrors what most DPDK example apps
// (skeleton, l2fwd) do in their port_init().
bool InitPort(uint16_t port_id, rte_mempool* mbuf_pool) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        fprintf(stderr, "Port %u is not valid\n", port_id);
        return false;
    }

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        fprintf(stderr, "Failed to get device info for port %u\n", port_id);
        return false;
    }

    // rte_eth_conf describes offloads/features to enable on the device
    // before queues are set up. Zero-init means "no offloads" — the safe
    // baseline every NIC supports.
    rte_eth_conf port_conf{};
    if (rte_eth_dev_configure(port_id, /*nb_rx_q=*/1, /*nb_tx_q=*/1, &port_conf) != 0) {
        fprintf(stderr, "rte_eth_dev_configure failed on port %u\n", port_id);
        return false;
    }

    uint16_t nb_rxd = kRxRingSize;
    uint16_t nb_txd = kTxRingSize;
    // NICs constrain descriptor counts to hardware-supported values; this
    // clamps our requested ring sizes to what the device actually allows.
    if (rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd) != 0) {
        fprintf(stderr, "rte_eth_dev_adjust_nb_rx_tx_desc failed on port %u\n", port_id);
        return false;
    }

    // One RX queue, backed by mbuf_pool: incoming packets get copied into
    // mbufs allocated from this pool.
    if (rte_eth_rx_queue_setup(port_id, /*rx_queue_id=*/0, nb_rxd,
                                rte_eth_dev_socket_id(port_id), nullptr, mbuf_pool) < 0) {
        fprintf(stderr, "rte_eth_rx_queue_setup failed on port %u\n", port_id);
        return false;
    }

    rte_eth_txconf txconf = dev_info.default_txconf;
    if (rte_eth_tx_queue_setup(port_id, /*tx_queue_id=*/0, nb_txd,
                                rte_eth_dev_socket_id(port_id), &txconf) < 0) {
        fprintf(stderr, "rte_eth_tx_queue_setup failed on port %u\n", port_id);
        return false;
    }

    if (rte_eth_dev_start(port_id) < 0) {
        fprintf(stderr, "rte_eth_dev_start failed on port %u\n", port_id);
        return false;
    }

    // Promiscuous mode: accept frames not addressed to our own MAC, useful
    // while learning since a vdev/testbed setup often won't have ARP wired
    // up. Not something you'd leave on in production without reason.
    rte_eth_promiscuous_enable(port_id);

    rte_ether_addr mac_addr{};
    rte_eth_macaddr_get(port_id, &mac_addr);
    printf("Port %u MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", port_id,
           mac_addr.addr_bytes[0], mac_addr.addr_bytes[1], mac_addr.addr_bytes[2],
           mac_addr.addr_bytes[3], mac_addr.addr_bytes[4], mac_addr.addr_bytes[5]);

    rte_eth_link link{};
    rte_eth_link_get_nowait(port_id, &link);
    printf("Port %u link: %s, %u Mbps\n", port_id,
           link.link_status ? "up" : "down", link.link_speed);

    return true;
}

// Hand-builds one UDP packet inside a freshly allocated mbuf so the
// Ethernet/IPv4/UDP header layout and checksum helpers are visible end to
// end, rather than hidden behind a library call.
rte_mbuf* BuildUdpPacket(rte_mempool* mbuf_pool, const rte_ether_addr& src_mac,
                          const rte_ether_addr& dst_mac, std::string_view payload) {
    rte_mbuf* pkt = rte_pktmbuf_alloc(mbuf_pool);
    if (pkt == nullptr) {
        fprintf(stderr, "rte_pktmbuf_alloc failed (pool exhausted?)\n");
        return nullptr;
    }

    const size_t hdr_len = sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr);
    const size_t total_len = hdr_len + payload.size();

    // rte_pktmbuf_append grows the mbuf's data area and returns a pointer to
    // the newly added region — the standard way to reserve space for
    // headers/payload you're about to fill in.
    auto* eth_hdr = reinterpret_cast<rte_ether_hdr*>(rte_pktmbuf_append(pkt, total_len));
    if (eth_hdr == nullptr) {
        fprintf(stderr, "rte_pktmbuf_append failed: mbuf too small for %zu bytes\n", total_len);
        rte_pktmbuf_free(pkt);
        return nullptr;
    }

    // --- Ethernet header ---
    eth_hdr->dst_addr = dst_mac;
    eth_hdr->src_addr = src_mac;
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // --- IPv4 header ---
    auto* ip_hdr = reinterpret_cast<rte_ipv4_hdr*>(eth_hdr + 1);
    std::memset(ip_hdr, 0, sizeof(*ip_hdr));
    ip_hdr->version_ihl = RTE_IPV4_VHL_DEF; // version 4, 20-byte header
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + payload.size());
    ip_hdr->packet_id = rte_cpu_to_be_16(0);
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->src_addr = rte_cpu_to_be_32(RTE_IPV4(10, 0, 0, 1));
    ip_hdr->dst_addr = rte_cpu_to_be_32(RTE_IPV4(10, 0, 0, 2));
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr); // software checksum; NICs can also offload this

    // --- UDP header + payload ---
    auto* udp_hdr = reinterpret_cast<rte_udp_hdr*>(ip_hdr + 1);
    udp_hdr->src_port = rte_cpu_to_be_16(9);
    udp_hdr->dst_port = rte_cpu_to_be_16(9); // discard port
    udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(rte_udp_hdr) + payload.size());
    udp_hdr->dgram_cksum = 0;
    auto* payload_ptr = reinterpret_cast<char*>(udp_hdr + 1);
    std::memcpy(payload_ptr, payload.data(), payload.size());
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);

    // mbuf metadata DPDK/the NIC uses for segmentation, offload decisions,
    // and multi-segment packet walking — set explicitly rather than relying
    // on defaults so the fields are visible here.
    pkt->l2_len = sizeof(rte_ether_hdr);
    pkt->l3_len = sizeof(rte_ipv4_hdr);
    pkt->pkt_len = static_cast<uint32_t>(total_len);
    pkt->data_len = static_cast<uint16_t>(total_len);
    pkt->nb_segs = 1;

    return pkt;
}

// rte_eth_tx_burst is non-blocking and may transmit fewer packets than
// requested (ring full); it does NOT free untransmitted mbufs, so the
// caller must free those itself. Successfully transmitted mbufs are freed
// by the driver once the NIC confirms send.
uint16_t SendBurst(uint16_t port_id, rte_mbuf** pkts, uint16_t nb_pkts) {
    uint16_t nb_sent = rte_eth_tx_burst(port_id, /*queue_id=*/0, pkts, nb_pkts);
    for (uint16_t i = nb_sent; i < nb_pkts; ++i) {
        rte_pktmbuf_free(pkts[i]);
    }
    return nb_sent;
}

// Polls the RX queue once per loop iteration (the defining pattern of
// poll-mode drivers — no interrupts, just a tight loop calling
// rte_eth_rx_burst). Every returned mbuf must eventually be freed by us;
// nothing does that automatically for RX.
void PollAndPrint(uint16_t port_id, unsigned max_iterations) {
    rte_mbuf* bufs[kBurstSize];

    for (unsigned iter = 0; iter < max_iterations && !g_force_quit; ++iter) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, /*queue_id=*/0, bufs, kBurstSize);
        for (uint16_t i = 0; i < nb_rx; ++i) {
            rte_mbuf* m = bufs[i];
            auto* eth_hdr = rte_pktmbuf_mtod(m, rte_ether_hdr*);
            printf("RX: %u bytes, ether_type=0x%04x\n", m->pkt_len,
                   rte_be_to_cpu_16(eth_hdr->ether_type));
            rte_pktmbuf_free(m);
        }
        if (nb_rx == 0) {
            rte_delay_us_block(1000); // avoid pegging the core at 100% during this demo
        }
    }

    rte_eth_stats stats{};
    rte_eth_stats_get(port_id, &stats);
    printf("Port %u stats: ipackets=%" PRIu64 " opackets=%" PRIu64
           " ierrors=%" PRIu64 " oerrors=%" PRIu64 "\n",
           port_id, stats.ipackets, stats.opackets, stats.ierrors, stats.oerrors);
}

} // namespace

int RunDpdkDemo(int argc, char** argv) {
    // rte_eal_init parses and consumes DPDK's own arguments (-l, --vdev,
    // -m, etc.) and returns how many argv slots it ate; whatever remains is
    // application-specific. Everything else in DPDK requires the EAL to be
    // initialized first — hugepage memory, lcores, and PCI/vdev probing all
    // happen here.
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "rte_eal_init failed: %s\n", rte_strerror(rte_errno));
    }
    argc -= ret;
    argv += ret;

    printf("EAL initialized. lcores=%u, master lcore=%u\n",
           rte_lcore_count(), rte_get_main_lcore());

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        rte_exit(EXIT_FAILURE,
                  "No DPDK-bound ports found. Bind a NIC with dpdk-devbind.py "
                  "or start with --vdev=net_null0 for a NIC-less smoke test.\n");
    }
    printf("Found %u available port(s)\n", nb_ports);

    // rte_pktmbuf_pool_create builds a hugepage-backed pool of mbufs — the
    // fixed-size buffers every RX'd or TX'd packet lives in. Sized per NIC
    // port in real apps; one shared pool is enough for this demo.
    rte_mempool* mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL", kNumMbufs, kMbufCacheSize, /*priv_size=*/0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == nullptr) {
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create failed: %s\n", rte_strerror(rte_errno));
    }

    const uint16_t port_id = rte_eth_find_next_owned_by(0, RTE_ETH_DEV_NO_OWNER);
    if (!InitPort(port_id, mbuf_pool)) {
        rte_exit(EXIT_FAILURE, "Failed to initialize port %u\n", port_id);
    }

    rte_ether_addr src_mac{};
    rte_eth_macaddr_get(port_id, &src_mac);
    rte_ether_addr dst_mac{};
    std::memset(&dst_mac, 0xff, sizeof(dst_mac)); // broadcast, so this runs without a real peer

    rte_mbuf* tx_batch[1];
    tx_batch[0] = BuildUdpPacket(mbuf_pool, src_mac, dst_mac, "hello from dpdk");
    if (tx_batch[0] == nullptr) {
        rte_exit(EXIT_FAILURE, "Failed to build test packet\n");
    }

    uint16_t nb_sent = SendBurst(port_id, tx_batch, 1);
    printf("Sent %u packet(s)\n", nb_sent);

    // Poll-mode RX: pull whatever arrived over ~2 seconds' worth of
    // iterations, then report device-level counters.
    PollAndPrint(port_id, /*max_iterations=*/2000);

    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    // Releases hugepage memory, PCI/vdev resources, etc. Should be the last
    // DPDK call made.
    rte_eal_cleanup();
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    return RunDpdkDemo(argc, argv);
}
