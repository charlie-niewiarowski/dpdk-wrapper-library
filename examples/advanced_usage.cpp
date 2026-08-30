// Advanced usage example: multiple RX/TX queues, pinned one-per-thread,
// each thread holding its own shared_ptr<rx_queue>/shared_ptr<tx_queue>
// obtained once up front and polled directly — no per-call lookup
// through port. Also shows sharing one port's pool with a second port,
// and bypassing pools entirely in favor of hand-managed packet_pool
// instances sized for two different traffic classes.

#include <cstdio>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <vector>

#include <rte_lcore.h>

#include "instance.h"
#include "packet_pool.h"

namespace {

std::atomic<bool> g_stop{false};

// One lcore's worth of work: hold this queue's handles directly and poll
// them in a tight loop, exactly like a DPDK lcore-per-queue worker would.
// No receive buffer to declare, no packets to manually free — packet_burst
// handles both.
void worker(std::shared_ptr<dpdk::rx_queue> rxq, std::shared_ptr<dpdk::tx_queue> txq) {
    while (!g_stop.load(std::memory_order_relaxed)) {
        dpdk::packet_burst burst = rxq->receive_burst();
        if (burst.empty()) {
            continue;
        }
        printf("[queue %u] received %zu packet(s)\n", rxq->queue_id(), burst.size());
        txq->send_burst(burst);
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        dpdk::instance runtime(argc, argv);

        constexpr uint16_t kNumQueues = 2;
        dpdk::port &eth0 = runtime.add_port(/*port_id=*/0, kNumQueues, kNumQueues);

        // Share eth0's pool with a second port on the same NUMA node
        // instead of each port building its own via the elt_size
        // constructor.
        dpdk::port &eth1 =
            runtime.add_port(/*port_id=*/1, kNumQueues, kNumQueues, eth0.get_pool());
        printf("eth0 and eth1 share the same pool: %s\n",
               eth0.get_pool() == eth1.get_pool() ? "yes" : "no");

        // Escape hatch: manage pools by hand instead of using a port's
        // own pool — e.g. a small pool for control-plane traffic and a
        // jumbo pool for bulk payloads, both on this NUMA node.
        dpdk::packet_pool small_pool("SMALL_POOL", /*n_mbufs=*/8192, /*elt_size=*/2048,
                                      rte_socket_id());
        dpdk::packet_pool jumbo_pool("JUMBO_POOL", /*n_mbufs=*/1024, /*elt_size=*/9216,
                                      rte_socket_id());
        dpdk::packet ctrl_pkt = small_pool.get();
        dpdk::packet bulk_pkt = jumbo_pool.get();
        printf("Allocated a %u-byte and a %u-byte scratch packet from hand-managed pools\n",
               ctrl_pkt.length(), bulk_pkt.length());

        // One thread per queue, each grabbing its own handle once up
        // front instead of going through port on every burst.
        std::vector<std::thread> workers;
        for (uint16_t q = 0; q < kNumQueues; ++q) {
            workers.emplace_back(worker, eth0.get_rx_queue(q), eth0.get_tx_queue(q));
        }

        // In production these would be pinned via rte_eal_remote_launch
        // / core affinity rather than left floating on whatever core the
        // OS scheduler picks.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        g_stop.store(true, std::memory_order_relaxed);
        for (auto &t : workers) {
            t.join();
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
