// Easy-use example: one runtime (the DPDK runtime), one port with a
// single RX/TX queue, everything driven through port's convenience
// surface. No manual queue handles, no manual pool management, no
// manually declared receive buffer, no manual freeing of packets — this
// is the "just get packets moving" path.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "runtime.h"

int main(int argc, char **argv) {
    try {
        // runtime owns EAL bring-up/teardown itself now. Forward the
        // process's own argv straight through as EAL args (typical for a
        // standalone binary where the whole command line is DPDK's) via
        // extra_args -- program_name still comes from argv[0] separately
        // since EAL/getopt never treats it as an option.
        dpdk::runtime runtime(dpdk::runtime_params{
            .program_name = argv[0],
            .extra_args = std::vector<std::string>(argv + 1, argv + argc),
        });

        // One port: its own 2KB pool (default), sized for its own NUMA
        // socket, with a single RX queue and a single TX queue.
        dpdk::port &eth0 =
            runtime.add_port(/*port_id=*/0, /*n_rx_queues=*/1, /*n_tx_queues=*/1);

        printf("Polling port %u...\n", eth0.port_id());
        for (;;) {
            dpdk::packet_burst burst = eth0.receive_burst(/*queue_id=*/0);
            if (burst.empty()) {
                continue;
            }

            for (const dpdk::packet &pkt : burst) {
                printf("Got packet: %u bytes\n", pkt.length());
            }

            // Echo the batch straight back out. Whatever the port doesn't
            // accept stays owned in burst and is freed when it goes out
            // of scope at the end of this iteration.
            eth0.send_burst(/*queue_id=*/0, burst);
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
