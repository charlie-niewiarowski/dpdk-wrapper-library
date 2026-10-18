// TCP echo over a tap interface: a kernel socket client on this host
// connects to the DPDK port and gets its bytes echoed back.
//
//   sudo ./tcp_echo                 # creates dpdktap0, 10.77.0.1 <-> 10.77.0.2
//   nc 10.77.0.2 9000               # in another terminal; type, see it echoed
//
// Options: --iface NAME  --kernel-ip A.B.C.D  --ip A.B.C.D  --port N
// (the two addresses share a /24). Needs root: creating and addressing a
// tap interface requires CAP_NET_ADMIN. Ctrl-C to stop.
//
// The threading is the exchange gateway's: an inbound thread owning RX +
// control TX, an outbound thread owning data TX, an SPSC ring between
// them -- see tcp_echo_server.h.

#include <arpa/inet.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include <rte_ethdev.h>

#include "dpdk_error.h"
#include "runtime.h"
#include "tap_interface.h"
#include "tcp_echo_server.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

uint32_t parse_ip(const char *s) {
    in_addr a{};
    if (inet_pton(AF_INET, s, &a) != 1) {
        throw std::runtime_error(std::string("bad IPv4 address: ") + s);
    }
    return ntohl(a.s_addr);
}

} // namespace

int main(int argc, char **argv) {
    std::string iface = "dpdktap0";
    const char *kernel_ip = "10.77.0.1";
    const char *dpdk_ip = "10.77.0.2";
    uint16_t listen_port = 9000;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--iface") {
            iface = argv[i + 1];
        } else if (flag == "--kernel-ip") {
            kernel_ip = argv[i + 1];
        } else if (flag == "--ip") {
            dpdk_ip = argv[i + 1];
        } else if (flag == "--port") {
            listen_port = static_cast<uint16_t>(std::stoi(argv[i + 1]));
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    try {
        // No hugepages needed for a tap-only process; --file-prefix keeps
        // it from colliding with any other DPDK process on the host.
        dpdk::runtime runtime(dpdk::runtime_params{
            .program_name = argv[0],
            .legacy_mem_mb = 256u,
            .file_prefix = "tcp_echo",
            .no_huge = true,
            .no_pci = true,
            .vdevs = {"net_tap0,iface=" + iface},
        });

        if (rte_eth_dev_count_avail() == 0) {
            // The EAL comes up fine unprivileged; it's the tap probe
            // (TUNSETIFF creating the interface) that needs CAP_NET_ADMIN.
            fprintf(stderr, "Error: net_tap failed to create %s -- run as root "
                            "(needs CAP_NET_ADMIN)\n", iface.c_str());
            return 1;
        }

        // Two queue pairs: TX 0 = control (inbound thread), TX 1 = data
        // (outbound thread). Tap insists RX count == TX count.
        constexpr uint16_t kQueues = 2;
        dpdk::port &tap = runtime.add_port(/*port_id=*/0, kQueues, kQueues);
        // Pin everything the kernel sends to RX queue 0: otherwise tun
        // re-steers each flow to whichever queue we last wrote it on and
        // reorders segments in the switch (see tap_interface.h). Still
        // correct without it, just much slower under load.
        try {
            dpdk::steer_rx_to_queue(tap, 0);
        } catch (const dpdk::dpdk_error &e) {
            fprintf(stderr, "warning: %s; continuing without RX steering\n", e.what());
        }
        dpdk::configure_kernel_interface(iface, parse_ip(kernel_ip), 24);
        tap.set_ipv4_address(parse_ip(dpdk_ip));

        echo::server server(tap, kQueues, listen_port);
        printf("echoing on %s:%u via %s (kernel side %s) -- try: nc %s %u\n", dpdk_ip,
               listen_port, iface.c_str(), kernel_ip, dpdk_ip, listen_port);

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::thread inbound([&] { server.run_inbound(g_stop); });
        std::thread outbound([&] { server.run_outbound(g_stop); });
        inbound.join();
        outbound.join();

        const dpdk::tcp_stats &st = server.stats();
        printf("\naccepted %lu, finished %lu; dropped: %lu bad checksum, %lu out of order, "
               "%lu malformed; %lu RST sent\n",
               server.accepted.load(), server.finished.load(), st.bad_checksum, st.out_of_order,
               st.malformed, st.rst_sent);
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
