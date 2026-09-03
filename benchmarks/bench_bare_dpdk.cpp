// Bare-DPDK baseline: raw rte_eth_tx_burst/rte_eth_rx_burst round-trip
// latency through a net_ring loopback vdev, with no library abstraction
// at all. Companion to bench_library.cpp -- same workload, same vdev,
// same batching, so the two are directly comparable.
//
// Run against a self-looping virtual NIC so this measures pure
// software/API overhead, not physical link latency:
//   ./bench_bare_dpdk --no-huge -m 512 --vdev=net_ring0

#include <cstdio>
#include <cstring>
#include <vector>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "bench_common.h"

namespace {

constexpr uint16_t kPortId = 0;
constexpr unsigned kNumMbufs = 8192;
constexpr uint16_t kEltSize = 2048;
constexpr uint16_t kRingSize = 1024;

bool setup_port(rte_mempool *pool) {
    rte_eth_conf conf{};
    if (rte_eth_dev_configure(kPortId, 1, 1, &conf) != 0) {
        return false;
    }
    uint16_t nb_rxd = kRingSize;
    uint16_t nb_txd = kRingSize;
    if (rte_eth_dev_adjust_nb_rx_tx_desc(kPortId, &nb_rxd, &nb_txd) != 0) {
        return false;
    }
    rte_eth_dev_info info{};
    if (rte_eth_dev_info_get(kPortId, &info) != 0) {
        return false;
    }
    if (rte_eth_rx_queue_setup(kPortId, 0, nb_rxd, rte_eth_dev_socket_id(kPortId), nullptr,
                                pool) < 0) {
        return false;
    }
    if (rte_eth_tx_queue_setup(kPortId, 0, nb_txd, rte_eth_dev_socket_id(kPortId),
                                &info.default_txconf) < 0) {
        return false;
    }
    return rte_eth_dev_start(kPortId) == 0;
}

// One send+receive round trip. Not timed individually -- see
// bench_common.h for why iterations are batched.
inline void round_trip(rte_mempool *pool) {
    rte_mbuf *m = rte_pktmbuf_alloc(pool);
    char *data = reinterpret_cast<char *>(rte_pktmbuf_append(m, bench::kPayloadLen));
    std::memset(data, 0, bench::kPayloadLen);

    rte_eth_tx_burst(kPortId, 0, &m, 1);

    rte_mbuf *rx_bufs[8];
    uint16_t n = 0;
    while (n == 0) {
        n = rte_eth_rx_burst(kPortId, 0, rx_bufs, 8);
    }
    for (uint16_t j = 0; j < n; ++j) {
        rte_pktmbuf_free(rx_bufs[j]);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (rte_eal_init(argc, argv) < 0) {
        fprintf(stderr, "rte_eal_init failed: %s\n", rte_strerror(rte_errno));
        return 1;
    }

    rte_mempool *pool = rte_pktmbuf_pool_create("BARE_DPDK_POOL", kNumMbufs, 256,
                                                 /*priv_size=*/0, kEltSize, rte_socket_id());
    if (pool == nullptr) {
        fprintf(stderr, "rte_pktmbuf_pool_create failed: %s\n", rte_strerror(rte_errno));
        return 1;
    }

    if (!setup_port(pool)) {
        fprintf(stderr, "port setup failed\n");
        return 1;
    }

    const uint64_t hz = rte_get_tsc_hz();

    for (int b = 0; b < bench::kWarmupBatches; ++b) {
        for (int i = 0; i < bench::kBatchSize; ++i) {
            round_trip(pool);
        }
    }

    std::vector<double> per_batch_ns;
    per_batch_ns.reserve(bench::kNumBatches);
    for (int b = 0; b < bench::kNumBatches; ++b) {
        const uint64_t t0 = rte_rdtsc();
        for (int i = 0; i < bench::kBatchSize; ++i) {
            round_trip(pool);
        }
        const uint64_t t1 = rte_rdtsc();
        const double batch_ns = static_cast<double>(t1 - t0) * 1e9 / static_cast<double>(hz);
        per_batch_ns.push_back(batch_ns / bench::kBatchSize);
    }

    bench::print_stats("bare_dpdk", bench::compute_stats(std::move(per_batch_ns)));

    rte_eth_dev_stop(kPortId);
    rte_eth_dev_close(kPortId);
    rte_eal_cleanup();
    return 0;
}
