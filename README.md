# dpdk-wrapper-library

A small C++20 wrapper over DPDK 22.11: RAII ownership of the EAL, ports, queues, mbuf pools and
packets, plus header builders, ARP, and a scoped TCP termination stack. That's enough for a
plain kernel-TCP client on the same machine to connect to a DPDK port.

Built with GCC 12 and CMake, and finds DPDK with `pkg_check_modules(libdpdk)`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build            # unprivileged suite; the tap test reports "skipped" without root
```

## Core API (unchanged)

| Type | Role |
|---|---|
| `runtime` | Brings the EAL up from a `runtime_params` aggregate (designated initializers) and tears it down. Owns every port. One per process. |
| `port` | One ethdev: its RX/TX queues and a shared `packet_pool`. `add_port(id, n_rx, n_tx)` returns `port&`. |
| `rx_queue` / `tx_queue` | Per-queue handles, handed out as `shared_ptr` for pinning to a thread. `receive_burst()` → `packet_burst`. `send_burst(span<packet>)` leaves unsent packets owned in the span. |
| `packet` | Move-only owner of one `rte_mbuf*`. |
| `build_*` / `finalize_*` | Ethernet / IPv4 / UDP / TCP header builders. Host order in, network order on the wire. Software checksums. |

## Additions

### Port helpers

```cpp
auto ctrl = port.claim_tx_queue(uint16_t{0});   // a specific queue, alongside claim_tx_queue()
rte_ether_addr mac = port.mac_address();        // rte_eth_macaddr_get
port.set_ipv4_address(RTE_IPV4(10, 77, 0, 2));  // the L3 identity ARP and TCP answer to
rte_ether_addr grp = dpdk::ipv4_multicast_mac(RTE_IPV4(239, 1, 2, 3)); // 01:00:5e:01:02:03
```

The two claim forms never hand out the same queue twice: the no-arg form skips ids already
claimed by id. Claiming an out-of-range id throws `dpdk_error`. Claiming an already-claimed id
returns null. `packet::is_contiguous()` reports whether a packet's bytes are all in its first
segment.

### ARP (`arp.h`)

- `neighbor_table` is a fixed-capacity IPv4 → MAC map. It doesn't allocate after construction
  and is single-threaded (it belongs to the RX thread).
- `handle_arp(pkt, local_mac, local_ip, table, pool, reply)` follows RFC 826's merge rule:
  known senders are refreshed, and a sender is added only when the request targets us. A
  request for `local_ip` gets a unicast reply built into `reply`. The function never
  transmits, so the caller picks the queue.
- `build_arp_request(...)` builds a who-has request, or a gratuitous ARP when target ==
  local.

`tcp_receiver` runs ARP itself and also learns neighbors from every IPv4 frame addressed to us.
The free functions are for UDP-only users, such as a multicast publisher.

### TCP termination (`tcp_stack.h`)

Passive-open IPv4 TCP that a stock Linux client can connect to and stream over, on a
**lossless** path (tap, loopback-like links). It's built around a gateway with exactly two
threads and no locks:

```cpp
port.set_ipv4_address(RTE_IPV4(10, 77, 0, 2));
dpdk::tcp_stack stack(port);                                    // fixed connection table (tcp_params)
stack.listen(9000);
dpdk::tcp_receiver rx = stack.receiver(port.claim_tx_queue(uint16_t{0})); // inbound: control TX
dpdk::tcp_sender   tx = stack.sender(port.claim_tx_queue(uint16_t{1}));   // outbound: data TX

// inbound thread
for (;;) {
    dpdk::packet_burst burst = rxq->receive_burst();
    for (const dpdk::tcp_event &ev : rx.poll(burst)) {
        // ev.type: accepted | data | peer_closed | closed | reset
        // ev.data: zero-copy view into burst -- consume or copy before burst dies
        // hand ev.conn (and bytes) to the outbound thread through your own SPSC queue
    }
}

// outbound thread
std::size_t n = tx.send(conn, bytes);   // a prefix; keep the rest and retry, like non-blocking send()
tx.send_space(conn);                    // what fits right now
tx.sendable(conn);                      // false once the conn can never send again
tx.close(conn);                         // exactly once per accepted conn -- also after reset/closed
```

**Ownership.**
- The inbound thread owns the state machine, `rcv_nxt`, and `snd_una` plus the peer window,
  which it learns from ACKs.
- The outbound thread owns `snd_nxt` and the FIN.
- A value one thread needs from the other is a single-writer atomic: `rcv_nxt`, a packed
  `{snd_una, window}`, `snd_nxt`, and the FIN-sent/released flags.
- A connection's addressing is written before `accepted` and never changes afterwards.
- A slot is reused only after the inbound side has seen the connection end *and* the outbound
  side has `close()`d it. Generation-tagged `conn_id`s reject stale ids.

`tcp_stack.h` and `src/tcp_connection.h` document each field.

**Rules for the application.**
1. Call `poll()` and `send()`/`close()` only from their own threads. The control and data tx
   queues must differ.
2. **The inbound thread must never block waiting on the outbound thread.** Outbound can be
   stuck on a full peer window that only an ACK processed by inbound can reopen. The stack
   advertises a constant receive window and has no backpressure, so buffer bursts on the
   inbound side instead of waiting. The echo example shows the overflow pattern; a blocking
   version reproducibly deadlocks.
3. `close()` every accepted connection exactly once, like an fd.

**Known, accepted race.** Pure ACKs (control queue) and data (data queue) can reorder on the
wire. Linux then discards a pure ACK whose seq it judges old. ACKs are cumulative and every
data segment carries `rcv_nxt` too, so nothing is corrupted; at worst an idle peer retransmits
once.

| Implemented | Not implemented (extension points documented in `tcp_stack.h`) |
|---|---|
| Passive open; MSS option parsed from the SYN and sent in the SYN-ACK (the only TCP option emitted) | **Retransmission**: first addition. An outbound-owned unacked-bytes ring, trimmed by the already-published `snd_una`, plus a timer |
| In-order delivery, trimming segments that overlap already-received bytes | **Out-of-order reassembly**: second addition. Today out-of-order segments are dropped and `rcv_nxt` is re-ACKed |
| Cumulative ACKs, coalesced to ≤ 1 per connection per `poll()` | Persist timer / zero-window probes, congestion control, Nagle |
| Segmentation to min(peer MSS, our MSS); sending limited by the peer window (RFC 793 WL1/WL2 updates) | Window scaling, SACK, timestamps (ignored in the SYN, never offered back, so Linux disables them) |
| FIN both ways, including simultaneous close; RST in; RST for unknown tuples and for SYNs when the table is full; duplicate-SYN handling | TIME_WAIT (the active closer goes straight to closed), SYN cookies |
| Software IPv4 + TCP checksum verification (`tcp_stats::bad_checksum`) | IP fragments, multi-segment mbufs, IPv4 options (all dropped and counted) |

### The tap path (`tap_interface.h`)

A `net_tap` port gives the kernel a `dpdktap0` interface, so a kernel socket on the same host can
reach the DPDK port:

```cpp
dpdk::runtime rt(dpdk::runtime_params{
    .legacy_mem_mb = 256u, .no_huge = true, .no_pci = true,
    .vdevs = {"net_tap0,iface=dpdktap0"},
});
dpdk::port &tap = rt.add_port(0, 2, 2);                 // tap requires n_rx == n_tx
dpdk::steer_rx_to_queue(tap, 0);                        // see below -- don't skip this
dpdk::configure_kernel_interface("dpdktap0", RTE_IPV4(10, 77, 0, 1), 24);
tap.set_ipv4_address(RTE_IPV4(10, 77, 0, 2));
```

Gotchas, each measured on DPDK 22.11.11 against Linux 6.12:

- **Root (CAP_NET_ADMIN)** is needed to create and address the interface. The PMD creates the
  interface during EAL probe and removes it at exit, so its address is set from inside the
  process with `configure_kernel_interface`.
- **RX reordering without `steer_rx_to_queue`.** Each tap queue pair is its own multiqueue
  tun fd, and tun steers a flow to the queue it was last *written* on. It switches immediately,
  without draining the old queue first. With tcp_stack's control/data TX split, segments then
  arrive across both RX queues and out of order at each switch. A 4 MB echo took ~5–14 s with
  hundreds of drops and retransmissions. `steer_rx_to_queue(tap, 0)` installs an `rte_flow`
  match-all → QUEUE 0 rule, which tap implements as a TC `skbedit queue_mapping` filter. With
  it, the same echo takes ~50 ms with zero reordering. The example still polls every RX queue,
  so it stays correct even when the rule is rejected.
- **Real-time signals.** The PMD sets `O_ASYNC` on its fds, so the kernel raises a signal in
  `SIGRTMIN+1..SIGRTMAX` on the process as packets move. `connect()` and socket calls with
  timeouts are not restarted by `SA_RESTART`, so they fail with `EINTR` on any thread that
  doesn't block that range. Block it on threads that make blocking syscalls, but **not** on the
  DPDK polling threads: if no thread accepts the queued signals, the kernel falls back to
  `SIGIO`, which kills the process.
- **Shared MAC.** The PMD gives the kernel-side netdev the same MAC as the DPDK port, so the
  kernel's neighbor entry for the DPDK address shows the interface's own MAC. Linux accepts
  that and it works, but it can be confusing while debugging ARP.
- **Checksums.** The PMD opens tun without `IFF_VNET_HDR`. The kernel completes every checksum
  before handing frames over and verifies every checksum on frames we write. The PMD's
  advertised TX checksum "offload" is a software computation in the PMD itself, so the library
  always checksums in software and leaves port offloads untouched. (`net_ring` advertises no
  checksum offloads at all.)

### Other EAL behaviour worth knowing

- `rte_eal_init` pins the calling thread to the main lcore, and every `std::thread` created
  afterwards inherits that one-CPU mask. Pin your inbound and outbound threads to their own
  cores explicitly, or they'll time-slice on one CPU.
- `no_huge` and `in_memory` can't be combined (EAL: "--legacy-mem is not compatible with
  --in-memory"). Give concurrent processes distinct `file_prefix`es instead.

## Example: TCP echo over tap

```sh
sudo ./build/tcp_echo            # dpdktap0, kernel 10.77.0.1 <-> DPDK 10.77.0.2:9000
nc 10.77.0.2 9000                # in another terminal
```

`examples/tcp_echo_server.h` is the gateway's shape: the inbound thread owns every RX queue
plus control TX, the outbound thread owns data TX, and an SPSC ring with an inbound-side
overflow queue connects them. Options: `--iface`, `--kernel-ip`, `--ip`, `--port`.

## Tests

- **`dpdk_lib_tests`** (label `unprivileged`, GoogleTest via FetchContent) needs no root, no
  hugepages and no NIC. A software-only EAL runs over `net_ring` vdevs: one shared runtime per
  binary, with a per-process `file_prefix`. `net_ring` loops tx queue *i* back into rx queue
  *i*, so the tests play the kernel peer with crafted segments. Covered:
  - ARP parsing and replies
  - the handshake, data both ways, and passive, active and simultaneous close
  - RST in and out
  - MSS segmentation and defaults
  - window-limited sending
  - out-of-order drop and overlap trimming
  - bad checksums
  - full table
  - stale `conn_id`s
  - a two-thread concurrent stream
- **`tap_integration_test`** (label `privileged`) runs real kernel sockets against the echo
  server over `net_tap`:
  - echo of a single message, a large stream, and concurrent clients
  - half-close → EOF
  - ECONNREFUSED on a port nobody listens on
  - surviving a client RST
  - recycling slots across 100 sequential connections
  - zero checksum failures and zero reordering

  Without CAP_NET_ADMIN it exits 77, which ctest reports as skipped. To run it:

  ```sh
  sudo ./build/tests/tap_integration_test
  # or without sudo, in a throwaway user+network namespace:
  unshare -rnm sh -c 'mount -t tmpfs tmpfs /var/run && ip link set lo up &&
                      ./build/tests/tap_integration_test'
  ```
