//
// Passive-open TCP termination over IPv4 for a lossless path (a tap
// interface or loopback-like link on one machine): enough TCP for a stock
// Linux kernel client to connect and stream bytes both ways. See
// "Scope" below for exactly what is and isn't implemented.
//

#ifndef TCP_STACK_H
#define TCP_STACK_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <rte_ether.h>

#include "packet.h"
#include "packet_burst.h"
#include "packet_pool.h"
#include "port.h"
#include "tx_queue.h"

namespace dpdk {

// ---------------------------------------------------------------------
// Threading model
//
// Built around a gateway with exactly two threads and no locks:
//
//   inbound thread  -- owns an rx_queue and a *control* tx_queue. Feeds
//                      every received burst through tcp_receiver::poll(),
//                      which answers ARP, runs the TCP state machine,
//                      sends SYN-ACK / pure ACK / RST on the control
//                      queue, and returns connection events + payload.
//   outbound thread -- owns a *data* tx_queue. Sends application bytes
//                      with tcp_sender::send() and ends connections with
//                      tcp_sender::close(). Owns sequence numbers.
//
// Every per-connection field has exactly one writer thread. Values the
// other thread needs cross through single-writer std::atomics with
// release/acquire ordering:
//
//   inbound writes  -> outbound reads: rcv_nxt (piggybacked as the ACK on
//                      data segments), {snd_una, peer window} packed into
//                      one 64-bit atomic so the pair is always consistent,
//                      and "terminated" (reset/closed: send() stops).
//   outbound writes -> inbound reads:  snd_nxt (to validate ACKs and as
//                      the seq of pure ACKs), and "FIN sent" / "released"
//                      flags (close() was called).
//
// A connection's addressing (4-tuple, peer MAC, MSS, ISS) is written by
// the inbound thread before the `accepted` event and never again; the
// gateway's own handoff of the conn_id to the outbound thread (an SPSC
// queue or similar) is what makes it visible there.
//
// Known, accepted race: pure ACKs (inbound, control queue) and data
// (outbound, data queue) for one connection can reorder on the wire. If
// the peer has already acknowledged data that overtook a pure ACK, Linux
// judges that ACK's seq old and drops it. Nothing is corrupted -- ACKs
// are cumulative and every data segment carries the latest rcv_nxt too
// -- at worst an idle peer retransmits once and is re-ACKed. snd_nxt is
// published *before* each segment reaches the NIC to keep that window as
// narrow as possible.
//
// ---------------------------------------------------------------------
// Scope
//
// Implemented: passive open; MSS option (parsed from the peer's SYN,
// sent in our SYN-ACK -- the one place this library emits TCP options);
// in-order delivery, including trimming a segment that partly overlaps
// bytes already received; cumulative ACKs coalesced to at most one per
// connection per poll(); segmentation to the peer's MSS; sending limited
// by the peer's advertised window; FIN in both directions; RST in (tears
// the connection down) and out (for segments to unknown connections, and
// SYNs when the table is full); re-sending a SYN-ACK for a duplicate
// SYN; software IPv4 + TCP checksum verification.
//
// Not implemented (on a lossless path none of these ever fire):
//   - Retransmission. Sent bytes are never kept, so send() needs no send
//     buffer. Extension point: an outbound-owned ring of unacked bytes
//     trimmed as snd_una (already published to the outbound thread)
//     advances, plus an RTO timer polled from the outbound loop. This is
//     the first thing to add for "full TCP".
//   - Out-of-order reassembly. A segment beyond rcv_nxt is dropped and
//     rcv_nxt re-ACKed, so the peer retransmits it in order. Extension
//     point: an inbound-owned list of held segments keyed by seq, drained
//     in process order when rcv_nxt reaches them. Second thing to add.
//   - Persist timer / zero-window probes, congestion control, window
//     scaling, SACK, timestamps, Nagle. The peer's SYN options other than
//     MSS are ignored and never offered back, so Linux disables them.
//   - TIME_WAIT: the active closer goes straight to closed once the
//     peer's FIN is ACKed.
//   - SYN cookies, IP fragments and multi-segment mbufs (dropped and
//     counted in tcp_stats), IPv4 options on transmit.
//
// Receive side: payload is handed to the application straight out of the
// mbuf and the stack keeps no receive buffer, so the advertised window is
// a constant (tcp_params::receive_window). An application that can't
// keep up must buffer for itself; there is no backpressure on the peer.
// ---------------------------------------------------------------------

// Identifies one connection. slot indexes the stack's fixed connection
// table; generation tells apart successive connections that reuse a
// slot, so a stale id is rejected rather than addressing a newer peer.
// Generations start at 1: a value-initialized conn_id is never valid.
struct conn_id {
    uint32_t slot = 0;
    uint32_t generation = 0;

    friend bool operator==(conn_id, conn_id) = default;
};

enum class tcp_event_type {
    accepted,    // handshake complete; conn is usable with tcp_sender
    data,        // in-order payload bytes, in tcp_event::data
    peer_closed, // peer sent FIN (half-close); you may keep sending, then close()
    closed,      // both directions finished cleanly; no further events for conn
    reset,       // peer sent RST; no further events for conn
};

struct tcp_event {
    tcp_event_type type;
    conn_id conn;
    // data events only: a zero-copy view into the received mbuf, valid
    // until the packet_burst passed to poll() is destroyed.
    std::span<const std::byte> data;
    uint32_t peer_addr; // host byte order
    uint16_t peer_port;
    uint16_t local_port;
};

// The result of one poll(). Fixed capacity, no heap: a single segment
// yields at most three events (accepted + data + peer_closed when the
// handshake's final ACK carries data and a FIN), so three per packet in
// the largest possible burst always fits.
class tcp_events {
public:
    static constexpr std::size_t max_size = 3 * packet_burst::max_size;

    const tcp_event *begin() const noexcept { return events_; }
    const tcp_event *end() const noexcept { return events_ + count_; }
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    const tcp_event &operator[](std::size_t i) const noexcept { return events_[i]; }

private:
    void push_back(const tcp_event &ev) noexcept { events_[count_++] = ev; }

    tcp_event events_[max_size];
    std::size_t count_ = 0;

    friend class tcp_receiver;
};

struct tcp_params {
    // Connection slots, allocated once at construction. A SYN arriving
    // while every slot is in use is answered with RST.
    uint16_t max_connections = 64;
    // Advertised in our SYN-ACK and the upper bound on segment size;
    // clamped to the port's MTU minus 40 (IPv4 + TCP headers).
    uint16_t mss = 1460;
    // Advertised on every segment; constant (see "Receive side" above).
    // No window scaling, so 65535 is the most it can be.
    uint16_t receive_window = 65535;
    // Software-verify inbound IPv4 and TCP checksums and drop failures.
    bool verify_checksums = true;
};

// Inbound-thread counters. Plain integers: only the inbound thread
// writes them, and reading them from another thread is only meaningful
// once it has stopped.
struct tcp_stats {
    uint64_t bad_checksum = 0;
    uint64_t malformed = 0;      // truncated / bad lengths / bad header fields
    uint64_t ip_fragments = 0;   // not reassembled
    uint64_t multi_segment = 0;  // payload not contiguous in one mbuf
    uint64_t out_of_order = 0;   // segment beyond rcv_nxt, dropped and re-ACKed
    uint64_t no_free_slot = 0;   // SYN refused because the table was full
    uint64_t rst_received = 0;
    uint64_t rst_sent = 0;
    uint64_t control_tx_dropped = 0; // control segments the tx ring didn't take
};

class tcp_receiver;
class tcp_sender;
struct tcp_connection; // defined in src/; per-slot state

// Owns the connection table and the set of listening ports. A setup-time
// object: construct it, listen(), take the one receiver and the one
// sender, and move each into its thread. Must outlive both handles --
// they point back into it, the same lifetime rule port has with runtime.
class tcp_stack {
public:
    // Reads the port's MAC, IPv4 address and MTU, and allocates outgoing
    // segments from its pool. Throws dpdk_error if the port has no IPv4
    // address set (port::set_ipv4_address) or the parameters are invalid.
    explicit tcp_stack(port &p, tcp_params params = {});
    tcp_stack(const tcp_stack &) = delete;
    tcp_stack &operator=(const tcp_stack &) = delete;
    ~tcp_stack();

    // Accept connections on local_port. Setup-time only: call before the
    // receiver starts polling. Throws dpdk_error past 16 ports.
    void listen(uint16_t local_port);

    // Each may be called exactly once (a second call throws dpdk_error):
    // there is one inbound and one outbound side, and the type system is
    // how that's enforced. control_txq and data_txq must be different
    // queues -- tx queues aren't thread-safe.
    tcp_receiver receiver(std::shared_ptr<tx_queue> control_txq);
    tcp_sender sender(std::shared_ptr<tx_queue> data_txq);

    // The MSS actually in use (params.mss clamped to the port MTU).
    uint16_t mss() const noexcept { return mss_; }

private:
    struct listen_set {
        static constexpr std::size_t max_ports = 16;
        uint16_t ports[max_ports] = {};
        std::size_t count = 0;
        bool contains(uint16_t p) const noexcept;
    };

    std::shared_ptr<packet_pool> pool_;
    rte_ether_addr local_mac_;
    uint32_t local_ip_;
    uint16_t mss_;
    tcp_params params_;
    listen_set listening_;
    std::unique_ptr<tcp_connection[]> slots_;
    bool receiver_taken_ = false;
    bool sender_taken_ = false;

    friend class tcp_receiver;
    friend class tcp_sender;
};

// The inbound thread's handle. Move-only; never share it between threads.
class tcp_receiver {
public:
    tcp_receiver(tcp_receiver &&) noexcept;
    tcp_receiver &operator=(tcp_receiver &&) noexcept;
    tcp_receiver(const tcp_receiver &) = delete;
    tcp_receiver &operator=(const tcp_receiver &) = delete;
    ~tcp_receiver();

    // Runs every packet in burst through ARP / IPv4 / TCP, sends whatever
    // control segments that produces on the control queue (at most one
    // coalesced ACK per connection), and returns the resulting events in
    // arrival order. Packets that aren't ARP or IPv4 for the port's
    // address are skipped and left in burst for the caller. data events
    // point into burst: consume or copy them before burst goes away.
    tcp_events poll(packet_burst &burst) noexcept;

    const tcp_stats &stats() const noexcept;

private:
    struct impl;
    explicit tcp_receiver(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;

    friend class tcp_stack;
};

// The outbound thread's handle. Move-only; never share it between threads.
class tcp_sender {
public:
    tcp_sender(tcp_sender &&) noexcept = default;
    tcp_sender &operator=(tcp_sender &&) noexcept = default;
    tcp_sender(const tcp_sender &) = delete;
    tcp_sender &operator=(const tcp_sender &) = delete;
    ~tcp_sender() = default;

    // Segments bytes to the connection's MSS and sends as much as the
    // peer's window allows and the tx ring accepts. Returns how many
    // bytes went out -- always a prefix of bytes; the caller keeps the
    // rest and retries, exactly like a non-blocking ::send(). Returns 0
    // for a connection that can't send: unknown or stale id, reset or
    // closed by the peer's side, or already close()d.
    std::size_t send(conn_id conn, std::span<const std::byte> bytes) noexcept;

    // How many bytes send() could put on the wire right now (the peer's
    // window right edge minus snd_nxt), e.g. to check that a whole OUCH
    // message fits before committing to send it.
    std::size_t send_space(conn_id conn) const noexcept;

    // Whether send() can ever succeed on conn again: false once it's
    // unknown/stale, reset or closed, or close()d. Distinguishes "window
    // full, retry later" (sendable, send_space() == 0) from "give up and
    // drop the remainder" -- send() alone returns 0 for both.
    bool sendable(conn_id conn) const noexcept;

    // Must be called exactly once for every accepted connection, even
    // after it was reset or closed -- like close() on an fd, it's what
    // lets the slot be reused. On a connection still open in our
    // direction it sends our FIN first. After close() the id must not be
    // used with this sender again; a stale-generation check catches most
    // such mistakes, but it isn't a guarantee.
    void close(conn_id conn) noexcept;

private:
    tcp_sender(tcp_stack *stack, std::shared_ptr<tx_queue> txq) noexcept
        : stack_(stack), txq_(std::move(txq)) {}

    tcp_connection *lookup(conn_id conn) const noexcept;

    tcp_stack *stack_;
    std::shared_ptr<tx_queue> txq_;

    friend class tcp_stack;
};

} // namespace dpdk

#endif //TCP_STACK_H
