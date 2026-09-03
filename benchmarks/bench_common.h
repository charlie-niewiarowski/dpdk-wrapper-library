// Shared timing/statistics helpers for the send/receive latency
// benchmarks. Not part of the dpdk:: library -- these are benchmark
// harness utilities only.
//
// Two back-to-back rte_rdtsc() calls with nothing between them already
// measure ~10ns (~37 cycles at typical desktop clock speeds) on this
// hardware -- that's RDTSC's own execution latency, not something a
// benchmark can avoid. Timing every single iteration individually would
// make that noise floor a large fraction of a signal this small, so
// instead each "sample" here is the average over a whole batch of
// iterations timed as one bracket: the fixed ~10ns cost is paid once
// per batch and divided across every iteration in it, not paid once
// per iteration.
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace bench {

constexpr int kWarmupBatches = 20;
constexpr int kNumBatches = 2000;
constexpr int kBatchSize = 100; // round trips per timed bracket
constexpr uint16_t kPayloadLen = 64;

struct stats {
    double min_ns;
    double p50_ns;
    double p99_ns;
    double mean_ns;
    double max_ns;
};

// per_batch_ns: one already-averaged (batch_total_ns / kBatchSize)
// value per batch.
inline stats compute_stats(std::vector<double> per_batch_ns) {
    std::sort(per_batch_ns.begin(), per_batch_ns.end());

    const std::size_t n = per_batch_ns.size();
    double sum = 0.0;
    for (const double v : per_batch_ns) {
        sum += v;
    }

    stats s{};
    s.min_ns = per_batch_ns.front();
    s.max_ns = per_batch_ns.back();
    s.p50_ns = per_batch_ns[n / 2];
    s.p99_ns = per_batch_ns[static_cast<std::size_t>(static_cast<double>(n) * 0.99)];
    s.mean_ns = sum / static_cast<double>(n);
    return s;
}

// Prints as machine-parseable "label_metric_ns=value" lines so the
// driver script can grep/parse them without a JSON dependency.
inline void print_stats(const char *label, const stats &s) {
    printf("%s_min_ns=%.2f\n", label, s.min_ns);
    printf("%s_p50_ns=%.2f\n", label, s.p50_ns);
    printf("%s_p99_ns=%.2f\n", label, s.p99_ns);
    printf("%s_mean_ns=%.2f\n", label, s.mean_ns);
    printf("%s_max_ns=%.2f\n", label, s.max_ns);
}

} // namespace bench

#endif //BENCH_COMMON_H
