// Shared fixture state for the unprivileged test suite.
//
// The EAL can be initialized only once per process, so every test in a
// binary shares one runtime, brought up by a gtest Environment before the
// first test and torn down after the last. It runs software-only: no
// hugepages, no PCI, and two net_ring vdevs as NICs, so the suite needs no
// root and no hardware.
//
// net_ring loops tx queue i straight back into rx queue i of the same
// port. Protocol tests use that to play both ends of the wire on port 0:
// the test injects "peer" frames on tx queue peer_q and the code under
// test polls rx peer_q; whatever the code under test sends on its own tx
// queue q comes back on rx q for the test to inspect. Port 1 exists only
// for the queue-claiming tests, which permanently consume its queues.

#ifndef DPDK_TEST_ENV_H
#define DPDK_TEST_ENV_H

#include <cstdint>
#include <memory>

#include "runtime.h"

namespace dpdk_test {

// Queue roles on port 0 (see header comment).
constexpr uint16_t peer_q = 0;    // test -> code under test
constexpr uint16_t control_q = 1; // code under test's inbound-thread tx
constexpr uint16_t data_q = 2;    // code under test's outbound-thread tx
constexpr uint16_t n_protocol_queues = 3;

constexpr uint16_t n_claim_queues = 4;

struct env {
    dpdk::runtime &rt;
    dpdk::port &proto;  // port 0
    dpdk::port &claims; // port 1
    // Claimed once here and shared: every protocol test needs the same
    // queues, and a claim can't be undone.
    std::shared_ptr<dpdk::rx_queue> rx[n_protocol_queues];
    std::shared_ptr<dpdk::tx_queue> tx[n_protocol_queues];
};

env &get();

// Discards anything still sitting in port 0's rx rings so one test's
// leftovers can't leak into the next.
void drain_all();

} // namespace dpdk_test

#endif // DPDK_TEST_ENV_H
