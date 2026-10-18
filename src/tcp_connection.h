//
// Per-connection state shared by tcp_receiver (inbound thread) and
// tcp_sender (outbound thread). Private to the library: see tcp_stack.h
// for the threading model this layout implements.
//

#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include <atomic>
#include <cstdint>
#include <rte_ether.h>

namespace dpdk {

// The inbound thread's view of the connection. The outbound thread
// never reads this directly -- it sees `terminated` and the published
// sequence values instead -- so it can be a plain enum.
enum class tcp_state : uint8_t {
    free,         // slot unused (on the free list)
    syn_received, // SYN-ACK sent, waiting for the final ACK
    established,
    close_wait,   // peer FINed; waiting for our close()
    last_ack,     // peer FINed, we FINed; waiting for our FIN's ACK
    fin_wait_1,   // we FINed first; waiting for its ACK (or a FIN)
    fin_wait_2,   // our FIN ACKed; waiting for the peer's FIN
    closing,      // both FINed simultaneously; waiting for our FIN's ACK
    closed,       // terminal (closed or reset); waiting to be finalized
};

// Bits of tcp_connection::out_flags.
inline constexpr uint8_t out_fin_sent = 1; // our FIN is on the wire; snd_nxt is final
inline constexpr uint8_t out_released = 2; // close() called; outbound is done with the slot

// Sequence-space comparisons (RFC 793 modular arithmetic).
inline bool seq_lt(uint32_t a, uint32_t b) noexcept { return static_cast<int32_t>(a - b) < 0; }
inline bool seq_leq(uint32_t a, uint32_t b) noexcept { return static_cast<int32_t>(a - b) <= 0; }
inline bool seq_gt(uint32_t a, uint32_t b) noexcept { return seq_lt(b, a); }
inline bool seq_geq(uint32_t a, uint32_t b) noexcept { return seq_leq(b, a); }

inline uint64_t pack_window(uint32_t una, uint32_t wnd) noexcept {
    return (static_cast<uint64_t>(una) << 32) | wnd;
}
inline uint32_t window_una(uint64_t w) noexcept { return static_cast<uint32_t>(w >> 32); }
inline uint32_t window_size(uint64_t w) noexcept { return static_cast<uint32_t>(w); }

struct tcp_connection {
    // --- Addressing: written by inbound while the slot is not visible to
    // outbound (between taking it off the free list and the `accepted`
    // event), immutable afterwards. Outbound reads it only for ids it
    // learned from that event, which is what orders the two.
    uint32_t peer_ip = 0;   // host order
    uint16_t peer_port = 0;
    uint16_t local_port = 0;
    rte_ether_addr peer_mac{};
    uint16_t send_mss = 0;  // min(peer's MSS option, our MSS)
    uint32_t iss = 0;

    // --- Inbound-only (never read by outbound).
    tcp_state state = tcp_state::free;
    uint32_t irs = 0;
    uint32_t rcv_nxt_local = 0; // mirror of rcv_nxt, read without atomics on the hot path
    uint32_t snd_una = 0;       // mirror of the una half of send_window
    uint32_t snd_wnd = 0;
    uint32_t snd_wl1 = 0;       // seq/ack of the segment that last updated snd_wnd (RFC 793)
    uint32_t snd_wl2 = 0;
    bool ack_pending = false;   // queued for this poll()'s coalesced ACK
    bool finalize_pending = false;
    // Extension point (out-of-order reassembly): held segments keyed by
    // seq would live here, inbound-owned, drained when rcv_nxt reaches them.

    // --- Written by inbound, read by outbound. Own cache line so the
    // outbound thread's stores below don't bounce it.
    alignas(64) std::atomic<uint32_t> generation{0};
    std::atomic<uint32_t> rcv_nxt{0};
    std::atomic<uint64_t> send_window{0}; // pack_window(snd_una, peer window)
    std::atomic<uint8_t> terminated{0};   // 1 once closed or reset: send() stops

    // --- Written by outbound, read by inbound. (Inbound also resets them
    // when it re-initializes a slot, which only happens after outbound has
    // set out_released and promised never to touch the slot again.)
    alignas(64) std::atomic<uint32_t> snd_nxt{0}; // stored *before* the segment is transmitted
    std::atomic<uint8_t> out_flags{0};
    // Extension point (retransmission): the outbound-owned ring of
    // unacked bytes would live here, trimmed as send_window's una advances.
};

} // namespace dpdk

#endif //TCP_CONNECTION_H
