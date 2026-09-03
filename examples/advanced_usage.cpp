// Advanced usage example: multiple RX/TX queues, pinned one-per-thread,
// each thread holding its own shared_ptr<rx_queue>/shared_ptr<tx_queue>
// obtained once up front and polled directly — no per-call lookup
// through port. Also shows sharing one port's pool with a second port,
// and getting hand-managed packet_pool instances (sized for two
// different traffic classes) via runtime::create_pool, since
// packet_pool's constructor is private.

#include <cstdio>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#include <rte_lcore.h>

#include "runtime.h"

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
        dpdk::runtime runtime(argc, argv);

        constexpr uint16_t kNumQueues = 2;
        dpdk::port &eth0 = runtime.add_port(/*port_id=*/0, kNumQueues, kNumQueues);

        // Share eth0's pool with a second port on the same NUMA node
        // instead of each port building its own via the elt_size
        // constructor.
        dpdk::port &eth1 =
            runtime.add_port(/*port_id=*/1, kNumQueues, kNumQueues, eth0.get_pool());
        printf("eth0 and eth1 share the same pool: %s\n",
               eth0.get_pool() == eth1.get_pool() ? "yes" : "no");

        // Escape hatch: get custom pools from runtime instead of using a
        // port's own default one — e.g. a small pool for control-plane
        // traffic and a jumbo pool for bulk payloads, both on this NUMA
        // node. packet_pool's constructor is private, so
        // runtime.create_pool() is how you reach it.
        std::shared_ptr<dpdk::packet_pool> small_pool = runtime.create_pool(
            "SMALL_POOL", /*n_mbufs=*/8192, /*elt_size=*/2048, rte_socket_id());
        std::shared_ptr<dpdk::packet_pool> jumbo_pool = runtime.create_pool(
            "JUMBO_POOL", /*n_mbufs=*/1024, /*elt_size=*/9216, rte_socket_id());
        dpdk::packet ctrl_pkt = small_pool->get();
        dpdk::packet bulk_pkt = jumbo_pool->get();
        printf("Allocated a %u-byte and a %u-byte scratch packet from hand-managed pools\n",
               ctrl_pkt.length(), bulk_pkt.length());

        // One thread per queue: each just claims the next unclaimed
        // queue rather than naming a numeric id -- the RSS pattern is
        // "one core takes one queue," and which specific number a given
        // core ends up with never matters.
        std::vector<std::thread> workers;
        while (auto rxq = eth0.claim_rx_queue()) {
            auto txq = eth0.claim_tx_queue();
            workers.emplace_back(worker, std::move(rxq), std::move(txq));
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
