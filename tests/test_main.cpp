#include "test_env.h"

#include <memory>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace dpdk_test {

namespace {

std::unique_ptr<dpdk::runtime> g_runtime;
std::unique_ptr<env> g_env;

class dpdk_environment : public ::testing::Environment {
public:
    void SetUp() override {
        g_runtime = std::make_unique<dpdk::runtime>(dpdk::runtime_params{
            .program_name = "dpdk_lib_tests",
            .legacy_mem_mb = 256u,
            // Unique per process: without it, two test binaries run in
            // parallel (ctest -j) contend for the same EAL runtime
            // directory and the second one fails to init. --in-memory
            // would avoid the files entirely, but EAL rejects it together
            // with --no-huge.
            .file_prefix = "dpdk_lib_test_" + std::to_string(::getpid()),
            .log_level = std::string("*:warning"),
            .no_huge = true,
            .no_pci = true,
            .vdevs = {"net_ring0", "net_ring1"},
        });
        dpdk::port &proto = g_runtime->add_port(0, n_protocol_queues, n_protocol_queues);
        dpdk::port &claims = g_runtime->add_port(1, n_claim_queues, n_claim_queues);
        g_env.reset(new env{*g_runtime, proto, claims, {}, {}});
        for (uint16_t q = 0; q < n_protocol_queues; ++q) {
            g_env->rx[q] = proto.claim_rx_queue(q);
            g_env->tx[q] = proto.claim_tx_queue(q);
        }
    }

    void TearDown() override {
        g_env.reset();
        g_runtime.reset();
    }
};

} // namespace

env &get() { return *g_env; }

void drain_all() {
    for (auto &q : g_env->rx) {
        while (!q->receive_burst().empty()) {
        }
    }
}

} // namespace dpdk_test

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new dpdk_test::dpdk_environment);
    return RUN_ALL_TESTS();
}
