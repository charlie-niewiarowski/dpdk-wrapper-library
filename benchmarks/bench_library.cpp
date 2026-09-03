// Library baseline: the same round-trip latency workload as
// bench_bare_dpdk.cpp, through the same net_ring loopback vdev, driven
// entirely through the dpdk:: library, using the same batched timing
// (see bench_common.h for why). Measures both supported access
// patterns:
//   - library_direct: claim_rx_queue()/claim_tx_queue() called once,
//     then the returned queue handles used directly -- the
//     zero-extra-indirection hot path.
//   - library_port_forwarding: port::receive_burst/send_burst, the
//     convenience path that looks the queue up by id on every call.
//
// Run against a self-looping virtual NIC so this measures pure
// software/API overhead, not physical link latency:
//   ./bench_library --no-huge -m 512 --vdev=net_ring0

#include <cstdio>
#include <cstring>
#include <array>
#include <vector>

#include <rte_cycles.h>

#include "bench_common.h"
#include "runtime.h"

namespace {

inline void round_trip_direct(dpdk::packet_pool &pool, dpdk::rx_queue &rxq,
                               dpdk::tx_queue &txq) {
    dpdk::packet pkt = pool.get();
    std::memset(pkt.append(bench::kPayloadLen), 0, bench::kPayloadLen);
    std::array<dpdk::packet, 1> tx_batch{std::move(pkt)};
    txq.send_burst(tx_batch);

    dpdk::packet_burst burst = rxq.receive_burst();
    while (burst.empty()) {
        burst = rxq.receive_burst();
    }
}

inline void round_trip_forwarding(dpdk::packet_pool &pool, dpdk::port &eth0) {
    dpdk::packet pkt = pool.get();
    std::memset(pkt.append(bench::kPayloadLen), 0, bench::kPayloadLen);
    std::array<dpdk::packet, 1> tx_batch{std::move(pkt)};
    eth0.send_burst(/*queue_id=*/0, tx_batch);

    dpdk::packet_burst burst = eth0.receive_burst(/*queue_id=*/0);
    while (burst.empty()) {
        burst = eth0.receive_burst(/*queue_id=*/0);
    }
}

template <typename RoundTripFn>
std::vector<double> run_batched(uint64_t hz, RoundTripFn &&round_trip) {
    for (int b = 0; b < bench::kWarmupBatches; ++b) {
        for (int i = 0; i < bench::kBatchSize; ++i) {
            round_trip();
        }
    }

    std::vector<double> per_batch_ns;
    per_batch_ns.reserve(bench::kNumBatches);
    for (int b = 0; b < bench::kNumBatches; ++b) {
        const uint64_t t0 = rte_rdtsc();
        for (int i = 0; i < bench::kBatchSize; ++i) {
            round_trip();
        }
        const uint64_t t1 = rte_rdtsc();
        const double batch_ns = static_cast<double>(t1 - t0) * 1e9 / static_cast<double>(hz);
        per_batch_ns.push_back(batch_ns / bench::kBatchSize);
    }
    return per_batch_ns;
}

} // namespace

int main(int argc, char **argv) {
    try {
        dpdk::runtime runtime(argc, argv);
        dpdk::port &eth0 =
            runtime.add_port(/*port_id=*/0, /*n_rx_queues=*/1, /*n_tx_queues=*/1);

        const uint64_t hz = rte_get_tsc_hz();

        {
            auto rxq = eth0.claim_rx_queue();
            auto txq = eth0.claim_tx_queue();
            auto pool = eth0.get_pool();
            auto samples = run_batched(hz, [&] { round_trip_direct(*pool, *rxq, *txq); });
            bench::print_stats("library_direct", bench::compute_stats(std::move(samples)));
        }
        {
            auto pool = eth0.get_pool();
            auto samples = run_batched(hz, [&] { round_trip_forwarding(*pool, eth0); });
            bench::print_stats("library_port_forwarding",
                                bench::compute_stats(std::move(samples)));
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
