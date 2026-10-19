//
// Helpers for the net_tap path: a DPDK port backed by a Linux tap
// interface, so a kernel socket on the same host can talk to the port.
//
// Setup, in order:
//   1. runtime_params{.vdevs = {"net_tap0,iface=dpdktap0"}, ...} -- the
//      PMD creates `dpdktap0` while the EAL probes, and deletes it when
//      the process exits (so its address can't be set in advance).
//   2. runtime.add_port(port_id, n, n) -- tap requires equal RX and TX
//      queue counts; starting the port brings the interface up.
//   3. steer_rx_to_queue(port, 0) when using more than one queue pair.
//   4. configure_kernel_interface("dpdktap0", <kernel-side IP>, 24)
//   5. port.set_ipv4_address(<DPDK-side IP, same subnet>)
// The kernel then routes that subnet via dpdktap0, ARPs for the DPDK
// side's address (answered by tcp_receiver), and a plain
// `nc <DPDK-side IP> <port>` reaches the port. Creating the tap device
// and configuring it both need root (CAP_NET_ADMIN).
//
// Things about tap worth knowing:
//   - Every queue pair is its own multiqueue tun fd, and the kernel
//     steers each flow's packets to the queue that flow was last
//     *written* on -- switching immediately, without draining the old
//     queue first. With the control/data TX split tcp_stack uses, a
//     connection's segments then arrive spread over both RX queues and,
//     at each switch, out of order; tcp_stack drops those and the kernel
//     has to retransmit, which measured ~10x slower on a bulk transfer.
//     Call steer_rx_to_queue(port, 0) after add_port: it pins every frame
//     the kernel sends to RX queue 0, and the reordering disappears.
//   - The PMD sets O_ASYNC on its queue fds, so the kernel raises a
//     real-time signal (in SIGRTMIN+1..SIGRTMAX) on the process as
//     packets move, delivered to any thread not blocking it. SA_RESTART
//     doesn't cover connect() or socket calls with timeouts, so blocking
//     syscalls on other threads can fail with EINTR. Block that range on
//     threads that make blocking calls, but leave it unblocked on the
//     DPDK polling threads: if no thread accepts the queued signals, the
//     kernel falls back to SIGIO, whose default action kills the process.
//   - The PMD opens tun without IFF_VNET_HDR, so the kernel completes
//     every checksum before handing a frame over, and verifies every
//     checksum on frames we write. Its advertised "TX checksum offload"
//     is a software computation inside the PMD, no faster than the
//     library's own -- the library always checksums in software and
//     leaves the port's offload configuration alone.
//

#ifndef TAP_INTERFACE_H
#define TAP_INTERFACE_H

#include <cstdint>
#include <string>

#include "port.h"

namespace dpdk {

// Assigns addr/prefix_len (host byte order) to the kernel network
// interface ifname and sets it up, via the classic SIOCSIF* ioctls.
// Throws dpdk_error (with errno's text) on failure -- typically EPERM
// without CAP_NET_ADMIN, or ENODEV if ifname doesn't exist (yet).
void configure_kernel_interface(const std::string &ifname, uint32_t addr, uint8_t prefix_len);

// Installs an rte_flow rule delivering every received frame to RX queue
// queue_id (pattern: any Ethernet frame; action: QUEUE). Generic rte_flow,
// so it works on any PMD that supports that rule -- on net_tap it becomes
// a TC skbedit queue_mapping filter, which is what stops tun's per-flow
// re-steering from reordering a connection across queues (see above).
// The rule lives as long as the port; there's nothing to undo. Throws
// dpdk_error with the driver's reason if the rule is rejected.
void steer_rx_to_queue(const port &p, uint16_t queue_id);

} // namespace dpdk

#endif //TAP_INTERFACE_H
