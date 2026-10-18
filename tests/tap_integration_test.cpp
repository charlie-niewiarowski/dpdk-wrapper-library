// Integration test against the real Linux TCP stack over net_tap: kernel
// sockets in this process connect to the DPDK port and talk to the echo
// server from examples/tcp_echo_server.h (the gateway's two-thread shape).
//
// Needs root / CAP_NET_ADMIN to create and address the tap interface, so
// it's a separate binary from the unprivileged suite. Without the
// privilege it exits with code 77, which ctest reports as "skipped".
//
//   sudo ./build/tests/tap_integration_test
//   sudo ctest --test-dir build -L privileged
//
// Or without sudo, inside a throwaway user + network namespace (root in
// the namespace is enough to create tap devices, and the tmpfs gives the
// "root" EAL a writable /var/run):
//   unshare -rnm sh -c 'mount -t tmpfs tmpfs /var/run && ip link set lo up &&
//                       ./build/tests/tap_integration_test'
//
// TAP_TEST_NO_STEERING=1 skips steer_rx_to_queue, to observe the
// cross-queue reordering it exists to prevent.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <rte_ethdev.h>
#include <rte_ip.h>

#include "runtime.h"
#include "tap_interface.h"
#include "tcp_echo_server.h"

namespace {

constexpr int kSkipExitCode = 77; // ctest SKIP_RETURN_CODE
const char *const kIface = "dpdktaptest0";
constexpr uint32_t kKernelIp = RTE_IPV4(10, 78, 0, 1);
constexpr uint32_t kDpdkIp = RTE_IPV4(10, 78, 0, 2);
constexpr uint16_t kEchoPort = 9000;
constexpr uint16_t kQueues = 2;

std::unique_ptr<dpdk::runtime> g_runtime;
std::unique_ptr<echo::server> g_server;
std::atomic<bool> g_stop{false};
std::thread g_inbound, g_outbound;

// RAII kernel socket with a receive timeout, so a broken stack fails the
// test instead of hanging it.
struct client {
    int fd = -1;
    explicit client(uint16_t port = kEchoPort) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(kDpdkIp);
        connect_errno = ::connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0 ? 0 : errno;
        if (connect_errno == EINTR) {
            // The connect carries on in the background; wait for it.
            pollfd p{fd, POLLOUT, 0};
            while (::poll(&p, 1, 5000) < 0 && errno == EINTR) {
            }
            socklen_t len = sizeof(connect_errno);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_errno, &len);
        }
    }
    ~client() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    int connect_errno;

    bool send_all(const std::string &s) const {
        for (std::size_t off = 0; off < s.size();) {
            const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Reads until n bytes, EOF, or timeout.
    std::string recv_n(std::size_t n) const {
        std::string out;
        char buf[65536];
        while (out.size() < n) {
            const ssize_t r = ::recv(fd, buf, std::min(sizeof(buf), n - out.size()), 0);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            if (r <= 0) {
                break;
            }
            out.append(buf, static_cast<std::size_t>(r));
        }
        return out;
    }
};

// net_tap puts every queue fd in O_ASYNC mode, so the kernel raises a
// real-time signal on the process as packets move -- delivered to any
// thread that doesn't block it, where it turns blocking socket calls into
// EINTR (SA_RESTART doesn't cover connect() or calls with timeouts).
// Block the RT range on this (test/client) thread, and every thread it
// spawns inherits that. The DPDK polling threads must keep it unblocked:
// if nobody accepts the queued signals, the kernel falls back to SIGIO,
// whose default action kills the process.
void block_tap_signals() {
    sigset_t rt;
    sigemptyset(&rt);
    for (int sig = SIGRTMIN; sig <= SIGRTMAX; ++sig) {
        sigaddset(&rt, sig);
    }
    pthread_sigmask(SIG_BLOCK, &rt, nullptr);
}

std::string pattern(std::size_t n, unsigned seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (char &c : s) {
        x = x * 1103515245u + 12345u;
        c = static_cast<char>(x >> 24);
    }
    return s;
}

// Streams `data` through one connection, writing from a second thread so
// neither side's socket buffer can deadlock the exchange.
void echo_round_trip(const std::string &data) {
    client c;
    ASSERT_EQ(c.connect_errno, 0) << std::strerror(c.connect_errno);
    std::thread writer([&] {
        // Uneven chunk sizes, so segment boundaries don't line up with MSS.
        std::size_t off = 0;
        for (std::size_t i = 0; off < data.size(); ++i) {
            const std::size_t len = std::min<std::size_t>(data.size() - off, 1 + (i * 7919) % 9000);
            if (!c.send_all(data.substr(off, len))) {
                return;
            }
            off += len;
        }
    });
    const std::string got = c.recv_n(data.size());
    writer.join();
    ASSERT_EQ(got.size(), data.size());
    EXPECT_TRUE(got == data) << "echoed bytes differ";
}

TEST(tap, single_message_echo) {
    client c;
    ASSERT_EQ(c.connect_errno, 0) << std::strerror(c.connect_errno);
    ASSERT_TRUE(c.send_all("hello, exchange"));
    EXPECT_EQ(c.recv_n(15), "hello, exchange");
}

TEST(tap, large_stream_echo) {
    echo_round_trip(pattern(4 << 20, 1));
}

TEST(tap, half_close_echoes_then_eofs) {
    client c;
    ASSERT_EQ(c.connect_errno, 0);
    ASSERT_TRUE(c.send_all("last words"));
    ASSERT_EQ(::shutdown(c.fd, SHUT_WR), 0);
    EXPECT_EQ(c.recv_n(1000), "last words") << "echo, then EOF from our FIN";
    char b;
    EXPECT_EQ(::recv(c.fd, &b, 1, 0), 0) << "clean EOF, not a reset";
}

TEST(tap, unlistened_port_is_refused) {
    client c(kEchoPort + 1);
    EXPECT_EQ(c.connect_errno, ECONNREFUSED);
}

TEST(tap, client_reset_is_survived) {
    {
        client c;
        ASSERT_EQ(c.connect_errno, 0);
        ASSERT_TRUE(c.send_all("abc"));
        EXPECT_EQ(c.recv_n(3), "abc");
        linger l{1, 0}; // close() now sends RST instead of FIN
        setsockopt(c.fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    }
    // The server keeps working afterwards.
    echo_round_trip(pattern(10000, 2));
}

TEST(tap, concurrent_clients) {
    std::vector<std::thread> clients;
    for (unsigned i = 0; i < 8; ++i) {
        clients.emplace_back([i] { echo_round_trip(pattern(256 * 1024, 100 + i)); });
    }
    for (auto &t : clients) {
        t.join();
    }
}

TEST(tap, sequential_connections_recycle_slots) {
    // More connections than the server has slots (64 by default).
    for (unsigned i = 0; i < 100; ++i) {
        client c;
        ASSERT_EQ(c.connect_errno, 0) << "connection " << i;
        ASSERT_TRUE(c.send_all("x"));
        ASSERT_EQ(c.recv_n(1), "x");
        ::shutdown(c.fd, SHUT_WR);
        char b;
        ASSERT_EQ(::recv(c.fd, &b, 1, 0), 0) << "connection " << i;
    }
}

// Runs last (gtest keeps declaration order within a binary by default).
TEST(tap, zz_kernel_checksums_always_verified) {
    // Every frame the kernel produced passed software verification; and
    // since the kernel itself drops frames with bad checksums, every
    // test above passing shows ours are right too.
    EXPECT_EQ(g_server->stats().bad_checksum, 0u);
    EXPECT_EQ(g_server->stats().malformed, 0u);
    // The kernel's counters for the tap interface. /proc/net/dev is per
    // network namespace, unlike /sys/class/net. TX is kernel -> DPDK; TX
    // drops are frames tun couldn't queue because a ring was full (we
    // weren't draining fast enough), i.e. real loss on the "lossless" path.
    unsigned long kernel_tx_dropped = 0;
    std::ifstream dev("/proc/net/dev");
    for (std::string line; std::getline(dev, line);) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || line.find(kIface) == std::string::npos) {
            continue;
        }
        printf("/proc/net/dev: %s\n", line.c_str());
        std::istringstream fields(line.substr(colon + 1));
        unsigned long v[12] = {};
        for (unsigned long &x : v) {
            fields >> x;
        }
        // Fields: rx bytes packets errs drop fifo frame compressed
        // multicast, then tx bytes packets errs drop ...
        kernel_tx_dropped = v[11];
    }

    // With steer_rx_to_queue every kernel frame arrives on RX queue 0, so
    // absent loss nothing can arrive out of order. Without it (the
    // TAP_TEST_NO_STEERING diagnostic mode) tun's per-flow re-steering
    // reorders segments across queues, and the stream survives on
    // retransmission -- slowly -- so only report the count then. Real loss
    // also shows up as out-of-order and is recovered the same way.
    const unsigned long ooo = g_server->stats().out_of_order;
    printf("out-of-order segments dropped: %lu; kernel tap TX drops: %lu\n", ooo,
           kernel_tx_dropped);
    if (std::getenv("TAP_TEST_NO_STEERING") == nullptr && kernel_tx_dropped == 0) {
        EXPECT_EQ(ooo, 0u);
    }
}

} // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    try {
        g_runtime = std::make_unique<dpdk::runtime>(dpdk::runtime_params{
            .program_name = "tap_integration_test",
            .legacy_mem_mb = 256u,
            .file_prefix = "dpdk_lib_tap_test_" + std::to_string(::getpid()),
            .log_level = std::string("*:warning"),
            .no_huge = true,
            .no_pci = true,
            .vdevs = {std::string("net_tap0,iface=") + kIface},
        });
    } catch (const std::exception &e) {
        printf("SKIP: EAL init failed: %s\n", e.what());
        return kSkipExitCode;
    }
    if (rte_eth_dev_count_avail() == 0) {
        printf("SKIP: net_tap could not create %s (needs root / CAP_NET_ADMIN)\n", kIface);
        g_runtime.reset();
        return kSkipExitCode;
    }

    int rc = 1;
    try {
        dpdk::port &tap = g_runtime->add_port(0, kQueues, kQueues);
        if (std::getenv("TAP_TEST_NO_STEERING") == nullptr) {
            dpdk::steer_rx_to_queue(tap, 0);
        } else {
            printf("NOTE: running without steer_rx_to_queue\n");
        }
        dpdk::configure_kernel_interface(kIface, kKernelIp, 24);
        tap.set_ipv4_address(kDpdkIp);
        g_server = std::make_unique<echo::server>(tap, kQueues, kEchoPort);
        g_inbound = std::thread([] { g_server->run_inbound(g_stop); });
        g_outbound = std::thread([] { g_server->run_outbound(g_stop); });
        block_tap_signals(); // see below; the polling threads above keep them


        rc = RUN_ALL_TESTS();

        g_stop.store(true);
        g_inbound.join();
        g_outbound.join();
    } catch (const std::exception &e) {
        fprintf(stderr, "setup failed: %s\n", e.what());
        g_stop.store(true);
        if (g_inbound.joinable()) {
            g_inbound.join();
        }
        if (g_outbound.joinable()) {
            g_outbound.join();
        }
        rc = 1;
    }
    g_server.reset();
    g_runtime.reset();
    return rc;
}
