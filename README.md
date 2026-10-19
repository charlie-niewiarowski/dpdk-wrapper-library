# dpdk-wrapper-library

A small C++20 library for writing [DPDK](https://www.dpdk.org/) applications without the C
boilerplate. It wraps the parts of DPDK a packet-processing app touches like EAL startup, ports,
queues, mbuf pools and mbufs in RAII types. On top of that it adds header builders for
UDP/IPv4, and a TCP endpoint that a regular kernel-socket client can connect to.

- **RAII everywhere.** No `rte_pktmbuf_free`, no teardown order to remember. Packets free
  themselves, and the runtime tears ports down before the EAL.
- **Thin on the hot path.** Receive and send are thin wrappers over `rte_eth_rx_burst` /
  `rte_eth_tx_burst`, with no locks and no allocation.
- **Thread-per-queue friendly.** Claim a queue handle once during setup, move it into a worker
  thread, and poll it directly.
- **TCP termination.** Accept connections from stock Linux clients and stream bytes both ways,
  using two threads and no locks.
- **Runs without hardware.** Works over DPDK's software devices (`net_ring`, `net_tap`), with
  no hugepages and no NIC, so you can develop and test on a laptop.

```cpp
#include "runtime.h"

int main(int argc, char **argv) {
    dpdk::runtime rt(dpdk::runtime_params{
        .program_name = argv[0],
        .extra_args = std::vector<std::string>(argv + 1, argv + argc), // EAL flags
    });
    dpdk::port &eth0 = rt.add_port(/*port_id=*/0, /*n_rx_queues=*/1, /*n_tx_queues=*/1);

    for (;;) {
        dpdk::packet_burst burst = eth0.receive_burst(/*queue_id=*/0);
        eth0.send_burst(/*queue_id=*/0, burst); // echo; unsent packets free themselves
    }
}
```

## Contents

- [Requirements](#requirements)
- [Adding it to your project](#adding-it-to-your-project)
- [Core concepts](#core-concepts)
- [Building packets](#building-packets)
- [Accepting TCP connections](#accepting-tcp-connections)
- [Talking to the kernel over tap](#talking-to-the-kernel-over-tap)
- [Running without hugepages or a NIC](#running-without-hugepages-or-a-nic)
- [Examples](#examples)
- [Building and testing the library](#building-and-testing-the-library)

## Requirements

- Linux
- DPDK 22.11, discoverable through `pkg-config libdpdk`
- A C++20 compiler (tested with GCC 12)
- CMake 3.31 or newer

## Adding it to your project

The library builds as a static CMake target, `dpdk_lib`. Linking it gives you the public headers
and DPDK's own include and link flags.

```cmake
include(FetchContent)
FetchContent_Declare(dpdk_wrapper
        GIT_REPOSITORY https://github.com/charlie-niewiarowski/dpdk-wrapper-library.git
        GIT_TAG main)
set(DPDK_LIB_BUILD_TESTS OFF CACHE BOOL "" FORCE) # skip the library's own test suite
FetchContent_MakeAvailable(dpdk_wrapper)

target_link_libraries(my_app PRIVATE dpdk_lib)
```

A checkout works the same way with `add_subdirectory(path/to/dpdk-wrapper-library)`.

Everything lives in namespace `dpdk`. Most programs only need `#include "runtime.h"`, which
brings in ports, queues and packets. Add `tcp_stack.h`, `tap_interface.h` or the header builders
as you need them.

## Core concepts

```
runtime                  the EAL; owns every port            (one per process)
└── port                 one ethdev: its queues and a pool
    ├── rx_queue ×N      shared_ptr handles, one per thread
    ├── tx_queue ×N
    └── packet_pool      shared_ptr; can be shared between ports
packet                   owns one rte_mbuf (move-only)
packet_burst             up to 32 packets from one receive
```

### `runtime`

`runtime` brings the EAL up in its constructor and tears it down in its destructor. EAL options
go in a `runtime_params` aggregate: name the fields you need, and anything without a field goes
in `extra_args`, passed through verbatim.

```cpp
dpdk::runtime rt(dpdk::runtime_params{
    .core_list = "0-3",
    .n_memory_channels = 4,
    .file_prefix = "my_app",
    .pci_allow = {"0000:3b:00.0"},
});
```

Only one `runtime` may exist per process; that's a DPDK restriction. Setup failures throw
`dpdk::dpdk_error`, which is a `std::runtime_error`.

### `port`

`rt.add_port(port_id, n_rx, n_tx)` configures and starts an ethdev and returns a reference to
it. The `runtime` owns the port, and the reference stays valid for the runtime's lifetime. By
default each port gets its own pool of 2 KB buffers on the port's NUMA socket. Pass a pool to
share one between ports:

```cpp
dpdk::port &eth0 = rt.add_port(0, 2, 2);
dpdk::port &eth1 = rt.add_port(1, 2, 2, eth0.get_pool()); // same pool
auto jumbo = rt.create_pool("JUMBO", /*n_mbufs=*/1024, /*elt_size=*/9216, rte_socket_id());
```

For a single-threaded app, `port::receive_burst(q)` and `port::send_burst(q, pkts)` are all you
need. A multi-threaded app should claim queue handles instead.

### `rx_queue` / `tx_queue`

A queue handle is a `shared_ptr` you claim once during setup and then move into the thread that
polls it. DPDK queues aren't thread-safe, so each one belongs to exactly one thread.

```cpp
std::vector<std::thread> workers;
while (auto rxq = eth0.claim_rx_queue()) {   // next unclaimed queue; null once all are taken
    auto txq = eth0.claim_tx_queue();
    workers.emplace_back([rxq, txq] {
        for (;;) {
            dpdk::packet_burst burst = rxq->receive_burst();
            txq->send_burst(burst);
        }
    });
}
```

`claim_rx_queue(id)` and `claim_tx_queue(id)` claim one specific queue. They return null if that
queue is already taken, and throw if the id doesn't exist. Mixing the two forms never hands out
the same queue twice. Claim from a single setup thread before the workers start.

> **Pin your threads.** DPDK pins the thread that creates the `runtime` to one CPU, and every
> `std::thread` started afterwards inherits that pinning. Set each worker's affinity to its own
> core, or they'll all share one.

### `packet` and `packet_burst`

`packet` is a move-only owner of one `rte_mbuf`. It's freed when it goes out of scope unless the
NIC took it. `data()` and `length()` give you the bytes, and an empty `packet` tests false.

`receive_burst()` returns a `packet_burst` holding up to 32 packets. You can iterate over it or
index into it, and it converts to `std::span<packet>`. `send_burst(span)` returns how many
packets the NIC accepted. The rest stay valid and owned in the span, so you can retry them or let
them drop.

```cpp
dpdk::packet_burst burst = rxq->receive_burst();
for (const dpdk::packet &pkt : burst) {
    parse(pkt.data(), pkt.length());
}
```

Packets that don't fit in one buffer, such as jumbo frames, arrive split across several linked
mbufs, and `data()` covers only the first one. Check `pkt.is_contiguous()` before parsing a
packet through `data()`.

## Building packets

Allocate a packet from a pool, append headers outermost first, append the payload, then finalize
the headers innermost last so lengths and checksums are filled in. Every builder takes host byte
order and writes network byte order. Each one returns `nullptr` if the buffer is full.

```cpp
#include "ethernet_header.h"
#include "ipv4_header.h"
#include "udp_header.h"

// Publish one datagram to multicast group 239.1.2.3:5000.
const uint32_t group = RTE_IPV4(239, 1, 2, 3);
dpdk::packet pkt = eth0.get_pool()->get();               // empty packet if the pool is exhausted
dpdk::build_ethernet_header(pkt, dpdk::ipv4_multicast_mac(group), eth0.mac_address(),
                            RTE_ETHER_TYPE_IPV4);
rte_ipv4_hdr *ip = dpdk::build_ipv4_header(pkt, RTE_IPV4(10, 0, 0, 2), group, IPPROTO_UDP);
rte_udp_hdr *udp = dpdk::build_udp_header(pkt, /*src_port=*/5000, /*dst_port=*/5000);
pkt.append(std::as_bytes(std::span(message)));
dpdk::finalize_ipv4_header(pkt, ip);
dpdk::finalize_udp_header(pkt, ip, udp);
txq->send_burst(std::span(&pkt, 1));
```

`ipv4_multicast_mac(group)` maps a multicast group to its Ethernet address (`01:00:5e` + the low
23 bits), so multicast needs no address resolution. Checksums are computed in software, and the
library never enables device checksum offloads.

`tcp_header.h` provides the same `build_` / `finalize_` pair for TCP headers. You only need it
to write raw TCP segments yourself; for actual connections, use `tcp_stack` below.

## Accepting TCP connections

`tcp_stack` (`tcp_stack.h`) is a passive-open TCP endpoint over IPv4. A normal kernel-socket
client can `connect()` to it, stream bytes both ways and close cleanly. It answers ARP for the
port's address, so clients on the same link find it on their own. It's designed for an
application with one **inbound** thread and one **outbound** thread, and the two share no
locks:

| | inbound thread | outbound thread |
|---|---|---|
| handle | `tcp_receiver` | `tcp_sender` |
| owns | the RX queue(s) and a *control* TX queue | a *data* TX queue |
| does | `poll(burst)` → connection events and received bytes | `send(conn, bytes)`, `close(conn)` |

The stack sends ACKs, handshake replies and resets on the control queue. Your data goes out on
the data queue. The two must be different queues.

```cpp
#include "tcp_stack.h"

dpdk::port &port = rt.add_port(0, 2, 2);
port.set_ipv4_address(RTE_IPV4(10, 77, 0, 2));     // the address the stack answers to

dpdk::tcp_stack stack(port);                       // must outlive both handles
stack.listen(9000);
dpdk::tcp_receiver rx = stack.receiver(port.claim_tx_queue(uint16_t{0}));
dpdk::tcp_sender   tx = stack.sender(port.claim_tx_queue(uint16_t{1}));
auto rxq = port.claim_rx_queue(uint16_t{0});

std::thread inbound([&] {
    for (;;) {
        dpdk::packet_burst burst = rxq->receive_burst();
        for (const dpdk::tcp_event &ev : rx.poll(burst)) {
            switch (ev.type) {
            case dpdk::tcp_event_type::accepted:    /* new ev.conn from ev.peer_addr */ break;
            case dpdk::tcp_event_type::data:        /* ev.data: received bytes        */ break;
            case dpdk::tcp_event_type::peer_closed: /* peer is done sending           */ break;
            case dpdk::tcp_event_type::closed:      /* both sides finished            */ break;
            case dpdk::tcp_event_type::reset:       /* peer aborted                   */ break;
            }
            // Pass ev.conn (and a copy of ev.data) to the outbound thread through
            // your own queue, e.g. a single-producer/single-consumer ring.
        }
    }
});

std::thread outbound([&] {
    // For each item from the inbound thread:
    //   std::size_t n = tx.send(conn, bytes); // sends a prefix; keep the rest and retry
    //   tx.close(conn);                       // exactly once per accepted connection
});
```

`ev.data` points directly into the received packet. Copy what you need before `burst` goes out
of scope.

### Sending

- `send(conn, bytes)` behaves like a non-blocking socket `send()`. It splits the bytes into
  segments, sends as much as the peer's receive window allows, and returns how many bytes went
  out.
- `send_space(conn)` tells you how much would fit right now, so you can check that a whole
  message fits before committing to it.
- `sendable(conn)` is false once the connection can never send again. Use it to tell "window
  full, retry later" apart from "give up".

### Rules

1. **Call `poll()` only from the inbound thread, and `send()`/`close()` only from the outbound
   thread.**
2. **Never let the inbound thread wait on the outbound thread.** The outbound thread can be
   stalled on a full peer window, and only the inbound thread can process the ACK that reopens
   it. If your inbound thread blocks on a full handoff queue, both threads stop. Buffer on the
   inbound side instead; [`examples/tcp_echo_server.h`](examples/tcp_echo_server.h) shows how.
3. **`close()` every accepted connection exactly once**, even after `closed` or `reset`, like
   closing a file descriptor. That's what frees its slot for a new connection.

### Configuration and diagnostics

`tcp_params` sets the connection table size (default 64), the MSS (default 1460, clamped to the
port's MTU), the advertised receive window, and whether inbound checksums are verified. A SYN
that arrives while the table is full is refused with RST. `rx.stats()` counts dropped and
refused segments by reason.

### Limitations

`tcp_stack` targets **lossless links**: tap, loopback-style setups, or one host talking to
itself. It doesn't retransmit, and it drops segments that arrive out of order and asks for them
again. Over a link that loses or reorders packets, a stream will stall. Also not supported:

- congestion control and Nagle
- window scaling, SACK and timestamps (Linux clients negotiate these off automatically)
- TIME_WAIT
- IP fragments, IPv4 options, and segments split across several mbufs
- active open, i.e. `connect()`

## Talking to the kernel over tap

DPDK's `net_tap` driver creates a Linux network interface whose other end is your DPDK port.
Give the interface an address, and ordinary sockets on the same host can reach your application.
This is the easiest way to test against real clients without a NIC.

```cpp
#include "tap_interface.h"

dpdk::runtime rt(dpdk::runtime_params{
    .legacy_mem_mb = 256u,
    .no_huge = true,
    .no_pci = true,
    .vdevs = {"net_tap0,iface=dpdktap0"},
});
dpdk::port &tap = rt.add_port(0, 2, 2);                // tap needs equal RX and TX queue counts
dpdk::steer_rx_to_queue(tap, 0);                       // see below
dpdk::configure_kernel_interface("dpdktap0", RTE_IPV4(10, 77, 0, 1), 24);
tap.set_ipv4_address(RTE_IPV4(10, 77, 0, 2));
// Now `nc 10.77.0.2 9000` from a shell reaches a tcp_stack listening on port 9000.
```

Things to know:

- **It needs root,** or `CAP_NET_ADMIN`, to create and configure the interface. The interface
  exists only while your process runs, which is why its address is set from inside the program.
- **Call `steer_rx_to_queue(tap, 0)`** whenever you use more than one TX queue, which
  `tcp_stack` always does. Without it the kernel shuffles a connection's packets between RX
  queues and they arrive out of order. TCP throughput then drops by orders of magnitude.
- **Blocking calls can fail with `EINTR`.** The tap driver signals the process with real-time
  signals (`SIGRTMIN+1` to `SIGRTMAX`) as packets move. That interrupts `connect()`, `recv()`
  with a timeout, and similar calls on any thread. Block that signal range on threads that make
  blocking syscalls, but not on your DPDK polling threads: if no thread can take the signals,
  the kernel falls back to `SIGIO`, which terminates the process.

## Running without hugepages or a NIC

For development and CI, DPDK can run entirely in ordinary memory with software devices:

```cpp
dpdk::runtime rt(dpdk::runtime_params{
    .legacy_mem_mb = 256u,
    .file_prefix = "my_test",    // keeps concurrent processes from colliding
    .no_huge = true,
    .no_pci = true,
    .vdevs = {"net_ring0"},      // TX queue i loops back into RX queue i
});
```

`net_ring` needs no privileges at all, which makes it handy for unit tests. `no_huge` can't be
combined with `in_memory`, so give concurrent processes distinct `file_prefix`es instead.

## Examples

| Program | Shows |
|---|---|
| [`examples/easy_usage.cpp`](examples/easy_usage.cpp) | One port, one queue, echo through `port` alone |
| [`examples/advanced_usage.cpp`](examples/advanced_usage.cpp) | One thread per queue, shared pools, custom pools |
| [`examples/tcp_echo.cpp`](examples/tcp_echo.cpp) | A TCP echo server over tap, with the two-thread structure |

```sh
sudo ./build/tcp_echo            # creates dpdktap0: kernel 10.77.0.1, DPDK 10.77.0.2:9000
nc 10.77.0.2 9000                # in another terminal; type and see it echoed
```

## Building and testing the library

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The unit tests need no root and no hardware, because they run over `net_ring`. The tap
integration test runs real kernel sockets against the TCP stack. It needs `CAP_NET_ADMIN` and
reports *skipped* without it:

```sh
sudo ./build/tests/tap_integration_test
```

Pass `-DDPDK_LIB_BUILD_TESTS=OFF` to skip building the tests; they download GoogleTest.
