//
// TCP termination; see tcp_stack.h for the threading model and scope,
// and tcp_connection.h for per-field ownership.
//

#include "tcp_stack.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <netinet/in.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_random.h>
#include <rte_tcp.h>

#include "arp.h"
#include "dpdk_error.h"
#include "ethernet_header.h"
#include "ipv4_header.h"
#include "tcp_connection.h"
#include "tcp_header.h"

namespace dpdk {

namespace {

// RFC 1122 4.2.2.6: the MSS to assume when a SYN carries no MSS option.
constexpr uint16_t kDefaultPeerMss = 536;
constexpr uint8_t kTcpOptEnd = 0;
constexpr uint8_t kTcpOptNop = 1;
constexpr uint8_t kTcpOptMss = 2;
constexpr uint8_t kTcpOptMssLen = 4;

// close() can't leave its FIN unsent -- nothing would ever retransmit it
// -- so it retries a full tx ring (or empty pool) this many times before
// giving up. A ring that stays full this long means the device is stuck.
constexpr unsigned kCloseRetries = 1u << 20;

struct segment_addr {
    rte_ether_addr src_mac;
    rte_ether_addr dst_mac;
    uint32_t src_ip; // host order
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
};

// Builds one complete Ethernet/IPv4/TCP segment with checksums filled
// in. A nonzero mss_option appends the MSS option -- only ever passed for
// SYN-ACKs, the single exception to the header builders' "no options"
// layout. Returns an empty packet if the pool is exhausted.
packet build_segment(const packet_pool &pool, const segment_addr &a, uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t window, std::span<const std::byte> payload,
                     uint16_t mss_option = 0) noexcept {
    packet pkt = pool.get();
    if (!pkt) {
        return pkt;
    }
    if (build_ethernet_header(pkt, a.dst_mac, a.src_mac, RTE_ETHER_TYPE_IPV4) == nullptr) {
        return packet{};
    }
    rte_ipv4_hdr *ip = build_ipv4_header(pkt, a.src_ip, a.dst_ip, IPPROTO_TCP);
    if (ip == nullptr) {
        return packet{};
    }
    rte_tcp_hdr *tcp = build_tcp_header(pkt, a.src_port, a.dst_port, seq, ack, flags, window);
    if (tcp == nullptr) {
        return packet{};
    }
    if (mss_option != 0) {
        uint8_t *opt = pkt.append(kTcpOptMssLen);
        if (opt == nullptr) {
            return packet{};
        }
        opt[0] = kTcpOptMss;
        opt[1] = kTcpOptMssLen;
        opt[2] = static_cast<uint8_t>(mss_option >> 8);
        opt[3] = static_cast<uint8_t>(mss_option);
        tcp->data_off = ((sizeof(rte_tcp_hdr) + kTcpOptMssLen) / 4) << 4;
    }
    if (!payload.empty() && pkt.append(payload) == nullptr) {
        return packet{};
    }
    finalize_ipv4_header(pkt, ip);
    finalize_tcp_header(pkt, ip, tcp);
    return pkt;
}

// Verifies a TCP checksum the same way a receiver's hardware would: the
// one's-complement sum over pseudo-header + segment, checksum field
// included, comes out all-ones. (rte_ipv4_udptcp_cksum_verify does this
// too, but is still experimental API in 22.11.)
bool tcp_checksum_ok(const rte_ipv4_hdr *ip, const void *l4, uint16_t l4_len) noexcept {
    uint32_t sum = rte_ipv4_phdr_cksum(ip, 0);
    sum = __rte_raw_cksum(l4, l4_len, sum);
    return __rte_raw_cksum_reduce(sum) == 0xffff;
}

// The MSS option's value, or kDefaultPeerMss if absent or malformed.
uint16_t parse_mss_option(const uint8_t *opt, std::size_t len) noexcept {
    std::size_t i = 0;
    while (i < len) {
        const uint8_t kind = opt[i];
        if (kind == kTcpOptEnd) {
            break;
        }
        if (kind == kTcpOptNop) {
            ++i;
            continue;
        }
        if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) {
            break; // malformed: stop rather than read past the header
        }
        if (kind == kTcpOptMss && opt[i + 1] == kTcpOptMssLen) {
            const uint16_t mss = static_cast<uint16_t>((opt[i + 2] << 8) | opt[i + 3]);
            return mss != 0 ? mss : kDefaultPeerMss;
        }
        i += opt[i + 1];
    }
    return kDefaultPeerMss;
}

// Bytes the peer's window still has room for, given a published
// {snd_una, window} pair and the current snd_nxt.
std::size_t window_space(uint64_t window, uint32_t snd_nxt) noexcept {
    const uint32_t right_edge = window_una(window) + window_size(window);
    const int32_t space = static_cast<int32_t>(right_edge - snd_nxt);
    return space > 0 ? static_cast<std::size_t>(space) : 0;
}

// A received segment, parsed once and passed around by reference.
struct segment {
    const rte_ether_hdr *eth;
    uint32_t src_ip; // host order
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    uint16_t window;
    const std::byte *payload;
    uint32_t len;
    const uint8_t *options;
    std::size_t options_len;

    bool has(uint8_t f) const noexcept { return (flags & f) != 0; }
};

// Connection lookup by (peer ip, peer port, local port), owned by the
// inbound thread. Open addressing with linear probing and backward-shift
// deletion (so no tombstones accumulate as connections come and go),
// sized at construction to keep the load factor at or under one half.
// Key 0 marks an empty slot; a peer address of 0.0.0.0 is rejected
// before lookup, so no real key is ever 0.
class flow_table {
public:
    static constexpr uint32_t npos = UINT32_MAX;

    explicit flow_table(std::size_t capacity)
        : entries_(std::bit_ceil(std::max<std::size_t>(capacity * 2, 2))),
          mask_(entries_.size() - 1) {}

    static uint64_t key(uint32_t peer_ip, uint16_t peer_port, uint16_t local_port) noexcept {
        return (static_cast<uint64_t>(peer_ip) << 32) | (static_cast<uint64_t>(peer_port) << 16) |
               local_port;
    }

    uint32_t find(uint64_t k) const noexcept {
        for (std::size_t i = home(k);; i = (i + 1) & mask_) {
            if (entries_[i].key == k) {
                return entries_[i].value;
            }
            if (entries_[i].key == 0) {
                return npos;
            }
        }
    }

    // Callers never insert more than max_connections keys, so a free
    // entry always exists.
    void insert(uint64_t k, uint32_t value) noexcept {
        std::size_t i = home(k);
        while (entries_[i].key != 0 && entries_[i].key != k) {
            i = (i + 1) & mask_;
        }
        entries_[i] = {k, value};
    }

    void erase(uint64_t k) noexcept {
        std::size_t i = home(k);
        while (entries_[i].key != k) {
            if (entries_[i].key == 0) {
                return;
            }
            i = (i + 1) & mask_;
        }
        // Backward shift: pull later entries of the probe run into the
        // hole unless doing so would move one before its home slot.
        for (std::size_t j = (i + 1) & mask_; entries_[j].key != 0; j = (j + 1) & mask_) {
            const std::size_t h = home(entries_[j].key);
            const bool stays = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);
            if (!stays) {
                entries_[i] = entries_[j];
                i = j;
            }
        }
        entries_[i] = {};
    }

private:
    struct entry {
        uint64_t key = 0;
        uint32_t value = 0;
    };

    std::size_t home(uint64_t k) const noexcept {
        const uint64_t h = k * 0x9e3779b97f4a7c15ull;
        return static_cast<std::size_t>(h ^ (h >> 32)) & mask_;
    }

    std::vector<entry> entries_;
    std::size_t mask_;
};

} // namespace

// ---------------------------------------------------------------------
// tcp_stack

bool tcp_stack::listen_set::contains(uint16_t p) const noexcept {
    return std::find(ports, ports + count, p) != ports + count;
}

tcp_stack::tcp_stack(port &p, tcp_params params)
    : pool_(p.get_pool()), local_mac_(p.mac_address()), params_(params) {
    const std::optional<uint32_t> ip = p.ipv4_address();
    if (!ip) {
        throw dpdk_error("tcp_stack: port " + std::to_string(p.port_id()) +
                          " has no IPv4 address (call port::set_ipv4_address first)");
    }
    local_ip_ = *ip;
    if (params_.max_connections == 0) {
        throw dpdk_error("tcp_stack: max_connections must be at least 1");
    }

    uint16_t mtu = RTE_ETHER_MTU;
    if (rte_eth_dev_get_mtu(p.port_id(), &mtu) != 0) {
        mtu = RTE_ETHER_MTU; // driver can't say; assume standard Ethernet
    }
    const uint16_t mtu_mss = static_cast<uint16_t>(mtu - sizeof(rte_ipv4_hdr) - sizeof(rte_tcp_hdr));
    mss_ = std::min(params_.mss, mtu_mss);
    if (mss_ == 0) {
        throw dpdk_error("tcp_stack: mss must be nonzero");
    }

    slots_.reset(new tcp_connection[params_.max_connections]);
}

tcp_stack::~tcp_stack() = default;

void tcp_stack::listen(uint16_t local_port) {
    if (listening_.contains(local_port)) {
        return;
    }
    if (listening_.count == listen_set::max_ports) {
        throw dpdk_error("tcp_stack: at most " + std::to_string(listen_set::max_ports) +
                          " listening ports");
    }
    listening_.ports[listening_.count++] = local_port;
}

// ---------------------------------------------------------------------
// tcp_receiver (inbound thread)

struct tcp_receiver::impl {
    // Room for one immediate control segment per received packet (ARP
    // reply, SYN-ACK or RST) plus one coalesced ACK per connection
    // touched -- itself at most one per packet.
    static constexpr std::size_t max_control = 2 * packet_burst::max_size;

    tcp_stack *stack;
    std::shared_ptr<tx_queue> txq;
    // Fed by handle_arp's RFC 826 merge rule. Nothing reads it yet: every
    // TCP segment goes to the MAC that connection's SYN came from. An
    // active open (connect) would be its first reader.
    neighbor_table neighbors;
    flow_table flows;
    tcp_stats stats;

    // Slot bookkeeping. Every vector is reserved to its maximum size up
    // front, so push_back never allocates on the hot path.
    std::vector<uint32_t> free_slots;       // LIFO
    std::vector<uint32_t> awaiting_release; // terminal, waiting for the sender's close()
    std::vector<uint32_t> ack_list;         // this poll(): connections owed an ACK
    std::vector<uint32_t> finalize_list;    // this poll(): connections that went terminal

    packet control[max_control];
    std::size_t n_control = 0;

    impl(tcp_stack *s, std::shared_ptr<tx_queue> q)
        : stack(s), txq(std::move(q)), flows(s->params_.max_connections) {
        const uint32_t n = s->params_.max_connections;
        free_slots.reserve(n);
        for (uint32_t i = n; i-- > 0;) {
            free_slots.push_back(i); // slot 0 is handed out first
        }
        awaiting_release.reserve(n);
        ack_list.reserve(packet_burst::max_size);
        finalize_list.reserve(packet_burst::max_size);
    }

    tcp_connection &slot(uint32_t i) noexcept { return stack->slots_[i]; }

    tcp_event make_event(tcp_event_type type, uint32_t idx, std::span<const std::byte> data = {}) noexcept {
        tcp_connection &c = slot(idx);
        return tcp_event{type, conn_id{idx, c.generation.load(std::memory_order_relaxed)}, data,
                         c.peer_ip, c.peer_port, c.local_port};
    }

    segment_addr reply_addr(const tcp_connection &c) const noexcept {
        return {stack->local_mac_, c.peer_mac, stack->local_ip_, c.peer_ip, c.local_port, c.peer_port};
    }

    void queue_control(packet pkt) noexcept {
        if (!pkt || n_control == max_control) {
            ++stats.control_tx_dropped;
            return;
        }
        control[n_control++] = std::move(pkt);
    }

    void queue_ack(uint32_t idx) noexcept {
        tcp_connection &c = slot(idx);
        if (!c.ack_pending) {
            c.ack_pending = true;
            ack_list.push_back(idx);
        }
    }

    void schedule_finalize(uint32_t idx) noexcept {
        tcp_connection &c = slot(idx);
        if (!c.finalize_pending) {
            c.finalize_pending = true;
            finalize_list.push_back(idx);
        }
    }

    // Moves a connection to terminal state. The outbound thread sees
    // `terminated` and stops sending; the slot itself is only recycled
    // once close() has also been called (see finalize()).
    void terminate(uint32_t idx, tcp_event_type why, tcp_events &events) noexcept {
        tcp_connection &c = slot(idx);
        c.state = tcp_state::closed;
        c.terminated.store(1, std::memory_order_release);
        if (why == tcp_event_type::reset) {
            c.ack_pending = false; // never ACK into a reset connection
        }
        events.push_back(make_event(why, idx));
        schedule_finalize(idx);
    }

    void release_slot(uint32_t idx) noexcept {
        tcp_connection &c = slot(idx);
        c.state = tcp_state::free;
        c.ack_pending = false;
        c.finalize_pending = false;
        free_slots.push_back(idx);
    }

    // Responds to a segment that matches no connection (RFC 793 "reset
    // generation"): if it carried an ACK, the RST takes that as its seq;
    // otherwise the RST ACKs everything the segment occupied.
    void send_rst_for(const segment &seg) noexcept {
        if (seg.has(RTE_TCP_RST_FLAG)) {
            return; // never answer a reset with a reset
        }
        const segment_addr a{stack->local_mac_, seg.eth->src_addr, stack->local_ip_,
                             seg.src_ip,        seg.dst_port,      seg.src_port};
        packet rst;
        if (seg.has(RTE_TCP_ACK_FLAG)) {
            rst = build_segment(*stack->pool_, a, seg.ack, 0, RTE_TCP_RST_FLAG, 0, {});
        } else {
            const uint32_t occupied = seg.len + (seg.has(RTE_TCP_SYN_FLAG) ? 1 : 0) +
                                      (seg.has(RTE_TCP_FIN_FLAG) ? 1 : 0);
            rst = build_segment(*stack->pool_, a, 0, seg.seq + occupied,
                                RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG, 0, {});
        }
        ++stats.rst_sent;
        queue_control(std::move(rst));
    }

    void send_syn_ack(const tcp_connection &c) noexcept {
        queue_control(build_segment(*stack->pool_, reply_addr(c), c.iss, c.rcv_nxt_local,
                                    RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                                    stack->params_.receive_window, {}, stack->mss_));
    }

    // A SYN for a listening port: take a free slot, initialize it, and
    // answer with SYN-ACK. The slot stays invisible to the application
    // (and so to the outbound thread) until the handshake completes.
    void open(const segment &seg) noexcept {
        if (free_slots.empty()) {
            ++stats.no_free_slot;
            send_rst_for(seg);
            return;
        }
        const uint32_t idx = free_slots.back();
        free_slots.pop_back();
        tcp_connection &c = slot(idx);

        c.peer_ip = seg.src_ip;
        c.peer_port = seg.src_port;
        c.local_port = seg.dst_port;
        c.peer_mac = seg.eth->src_addr;
        c.send_mss = std::min(parse_mss_option(seg.options, seg.options_len), stack->mss_);
        c.iss = static_cast<uint32_t>(rte_rand());
        c.irs = seg.seq;
        c.rcv_nxt_local = seg.seq + 1;
        c.snd_una = c.iss;
        c.snd_wnd = seg.window;
        c.snd_wl1 = seg.seq;
        c.snd_wl2 = 0;
        c.state = tcp_state::syn_received;
        c.ack_pending = false;
        c.finalize_pending = false;

        // The generation bump comes first and out_flags is reset last,
        // with release: a stale tcp_sender call racing this re-init and
        // observing the reset flags is then guaranteed to also observe the
        // new generation, and bail (see tcp_sender::lookup's re-check).
        uint32_t gen = c.generation.load(std::memory_order_relaxed) + 1;
        if (gen == 0) {
            gen = 1;
        }
        c.generation.store(gen, std::memory_order_relaxed);
        c.rcv_nxt.store(c.rcv_nxt_local, std::memory_order_relaxed);
        c.send_window.store(pack_window(c.snd_una, c.snd_wnd), std::memory_order_relaxed);
        c.terminated.store(0, std::memory_order_relaxed);
        c.snd_nxt.store(c.iss + 1, std::memory_order_relaxed);
        c.out_flags.store(0, std::memory_order_release);

        flows.insert(flow_table::key(c.peer_ip, c.peer_port, c.local_port), idx);
        send_syn_ack(c);
    }

    // Folds the outbound thread's close() into our state machine.
    void sync_out_flags(tcp_connection &c) noexcept {
        if ((c.out_flags.load(std::memory_order_acquire) & out_fin_sent) == 0) {
            return;
        }
        if (c.state == tcp_state::established) {
            c.state = tcp_state::fin_wait_1;
        } else if (c.state == tcp_state::close_wait) {
            c.state = tcp_state::last_ack;
        }
    }

    bool rst_acceptable(const tcp_connection &c, const segment &seg) const noexcept {
        return seq_geq(seg.seq, c.rcv_nxt_local) &&
               seq_lt(seg.seq, c.rcv_nxt_local + stack->params_.receive_window);
    }

    void publish_window(tcp_connection &c) noexcept {
        c.send_window.store(pack_window(c.snd_una, c.snd_wnd), std::memory_order_release);
    }

    // ACK processing for a synchronized connection. Returns false if the
    // segment must be dropped without looking at its payload.
    bool process_ack(uint32_t idx, const segment &seg, tcp_events &events) noexcept {
        tcp_connection &c = slot(idx);
        // flags before snd_nxt: once fin_sent is visible, so is the
        // snd_nxt that already counts the FIN (stored before it).
        const uint8_t out = c.out_flags.load(std::memory_order_acquire);
        const uint32_t snd_nxt = c.snd_nxt.load(std::memory_order_acquire);

        if (seq_gt(seg.ack, snd_nxt)) {
            queue_ack(idx); // ACKs something never sent: re-sync the peer, drop
            return false;
        }
        bool changed = false;
        if (seq_gt(seg.ack, c.snd_una)) {
            c.snd_una = seg.ack;
            changed = true;
        }
        // RFC 793 SND.WL1/WL2: only a segment at least as new as the last
        // window update may change the window, so a reordered old segment
        // can't shrink it back.
        if (seq_geq(seg.ack, c.snd_una) &&
            (seq_lt(c.snd_wl1, seg.seq) || (c.snd_wl1 == seg.seq && seq_leq(c.snd_wl2, seg.ack)))) {
            if (c.snd_wnd != seg.window) {
                c.snd_wnd = seg.window;
                changed = true;
            }
            c.snd_wl1 = seg.seq;
            c.snd_wl2 = seg.ack;
        }
        if (changed) {
            publish_window(c);
        }

        if ((out & out_fin_sent) != 0 && seg.ack == snd_nxt) {
            switch (c.state) {
                case tcp_state::fin_wait_1:
                    c.state = tcp_state::fin_wait_2;
                    break;
                case tcp_state::closing:
                case tcp_state::last_ack:
                    terminate(idx, tcp_event_type::closed, events);
                    return false;
                default:
                    break;
            }
        }
        return true;
    }

    // In-order payload delivery and FIN handling.
    void process_data(uint32_t idx, const segment &seg, tcp_events &events) noexcept {
        tcp_connection &c = slot(idx);
        const bool can_receive = c.state == tcp_state::established ||
                                 c.state == tcp_state::fin_wait_1 ||
                                 c.state == tcp_state::fin_wait_2;
        const bool fin = seg.has(RTE_TCP_FIN_FLAG);
        if (!can_receive) {
            // The peer already FINed; anything occupying sequence space
            // now is a retransmission. Re-ACK so it stops.
            if (seg.len > 0 || fin) {
                queue_ack(idx);
            }
            return;
        }
        if (seg.len == 0 && !fin) {
            return; // pure ACK: already handled, and never ACK an ACK
        }

        if (seq_gt(seg.seq, c.rcv_nxt_local)) {
            // A gap: something before this was lost or reordered. Without
            // reassembly the only safe move is to drop it and re-ACK
            // rcv_nxt so the peer resends from there.
            ++stats.out_of_order;
            queue_ack(idx);
            return;
        }

        const uint32_t end = seg.seq + seg.len;
        if (seq_gt(end, c.rcv_nxt_local)) {
            // New bytes start at rcv_nxt; trim any already-delivered front.
            const uint32_t skip = c.rcv_nxt_local - seg.seq;
            events.push_back(make_event(tcp_event_type::data, idx,
                                        {seg.payload + skip, seg.len - skip}));
            c.rcv_nxt_local = end;
        }
        queue_ack(idx); // new data, or a duplicate to re-ACK

        if (fin && end == c.rcv_nxt_local) {
            c.rcv_nxt_local += 1;
            switch (c.state) {
                case tcp_state::established:
                    c.state = tcp_state::close_wait;
                    events.push_back(make_event(tcp_event_type::peer_closed, idx));
                    break;
                case tcp_state::fin_wait_1:
                    c.state = tcp_state::closing;
                    break;
                case tcp_state::fin_wait_2:
                    // No TIME_WAIT (see tcp_stack.h): the ACK of this FIN
                    // goes out in this poll's flush and the connection is done.
                    terminate(idx, tcp_event_type::closed, events);
                    break;
                default:
                    break;
            }
        }
        c.rcv_nxt.store(c.rcv_nxt_local, std::memory_order_release);
    }

    void process_segment(uint32_t idx, const segment &seg, tcp_events &events) noexcept {
        tcp_connection &c = slot(idx);
        sync_out_flags(c);

        switch (c.state) {
            case tcp_state::free:
            case tcp_state::closed:
                return; // terminal: waiting for finalize, ignore stragglers

            case tcp_state::syn_received:
                if (seg.has(RTE_TCP_RST_FLAG)) {
                    if (rst_acceptable(c, seg)) {
                        // Never accepted, so the application never saw it
                        // and the outbound thread can't hold it: recycle now.
                        ++stats.rst_received;
                        flows.erase(flow_table::key(c.peer_ip, c.peer_port, c.local_port));
                        release_slot(idx);
                    }
                    return;
                }
                if (seg.has(RTE_TCP_SYN_FLAG)) {
                    if (!seg.has(RTE_TCP_ACK_FLAG) && seg.seq == c.irs) {
                        send_syn_ack(c); // our SYN-ACK was lost or slow
                    }
                    return;
                }
                if (!seg.has(RTE_TCP_ACK_FLAG)) {
                    return;
                }
                if (seg.ack != c.iss + 1) {
                    send_rst_for(seg);
                    return;
                }
                c.snd_una = seg.ack;
                c.snd_wnd = seg.window;
                c.snd_wl1 = seg.seq;
                c.snd_wl2 = seg.ack;
                publish_window(c);
                c.state = tcp_state::established;
                events.push_back(make_event(tcp_event_type::accepted, idx));
                process_data(idx, seg, events); // the final ACK may carry data
                return;

            default:
                break;
        }

        if (seg.has(RTE_TCP_RST_FLAG)) {
            if (rst_acceptable(c, seg)) {
                ++stats.rst_received;
                terminate(idx, tcp_event_type::reset, events);
            }
            return;
        }
        if (seg.has(RTE_TCP_SYN_FLAG)) {
            queue_ack(idx); // RFC 5961 challenge ACK; the connection stands
            return;
        }
        if (!seg.has(RTE_TCP_ACK_FLAG)) {
            return;
        }
        if (!process_ack(idx, seg, events)) {
            return;
        }
        process_data(idx, seg, events);
    }

    void process_tcp(const rte_ether_hdr *eth, const rte_ipv4_hdr *ip, uint16_t ip_hdr_len,
                     uint16_t ip_total_len, tcp_events &events) noexcept {
        const auto *l4 = reinterpret_cast<const uint8_t *>(ip) + ip_hdr_len;
        const uint16_t l4_len = ip_total_len - ip_hdr_len;
        if (l4_len < sizeof(rte_tcp_hdr)) {
            ++stats.malformed;
            return;
        }
        const auto *tcp = reinterpret_cast<const rte_tcp_hdr *>(l4);
        const uint16_t data_off = static_cast<uint16_t>((tcp->data_off >> 4) * 4);
        if (data_off < sizeof(rte_tcp_hdr) || data_off > l4_len) {
            ++stats.malformed;
            return;
        }
        if (stack->params_.verify_checksums && !tcp_checksum_ok(ip, l4, l4_len)) {
            ++stats.bad_checksum;
            return;
        }

        const segment seg{
            eth,
            rte_be_to_cpu_32(ip->src_addr),
            rte_be_to_cpu_16(tcp->src_port),
            rte_be_to_cpu_16(tcp->dst_port),
            rte_be_to_cpu_32(tcp->sent_seq),
            rte_be_to_cpu_32(tcp->recv_ack),
            tcp->tcp_flags,
            rte_be_to_cpu_16(tcp->rx_win),
            reinterpret_cast<const std::byte *>(l4 + data_off),
            static_cast<uint32_t>(l4_len - data_off),
            l4 + sizeof(rte_tcp_hdr),
            data_off - sizeof(rte_tcp_hdr),
        };

        const uint32_t idx = flows.find(flow_table::key(seg.src_ip, seg.src_port, seg.dst_port));
        if (idx != flow_table::npos) {
            process_segment(idx, seg, events);
            return;
        }
        if (seg.has(RTE_TCP_SYN_FLAG) && !seg.has(RTE_TCP_ACK_FLAG) && !seg.has(RTE_TCP_RST_FLAG) &&
            stack->listening_.contains(seg.dst_port)) {
            open(seg);
            return;
        }
        send_rst_for(seg);
    }

    void process(const packet &pkt, tcp_events &events) noexcept {
        if (pkt.length() < sizeof(rte_ether_hdr)) {
            return;
        }
        if (!pkt.is_contiguous()) {
            ++stats.multi_segment;
            return;
        }
        const auto *eth = reinterpret_cast<const rte_ether_hdr *>(pkt.data());

        if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
            packet reply;
            if (handle_arp(pkt, stack->local_mac_, stack->local_ip_, neighbors, *stack->pool_,
                           reply) == arp_result::replied) {
                queue_control(std::move(reply));
            }
            return;
        }
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            return;
        }

        const uint16_t l3_avail = static_cast<uint16_t>(pkt.length() - sizeof(rte_ether_hdr));
        if (l3_avail < sizeof(rte_ipv4_hdr)) {
            ++stats.malformed;
            return;
        }
        const auto *ip = reinterpret_cast<const rte_ipv4_hdr *>(eth + 1);
        const uint16_t ip_hdr_len = static_cast<uint16_t>((ip->version_ihl & 0x0f) * 4);
        // total_length, not the frame length: short frames arrive padded
        // to the Ethernet minimum, and the padding isn't payload.
        const uint16_t ip_total_len = rte_be_to_cpu_16(ip->total_length);
        if ((ip->version_ihl >> 4) != 4 || ip_hdr_len < sizeof(rte_ipv4_hdr) ||
            ip_total_len < ip_hdr_len || ip_total_len > l3_avail) {
            ++stats.malformed;
            return;
        }
        if (rte_be_to_cpu_32(ip->dst_addr) != stack->local_ip_) {
            return; // not ours
        }
        const uint32_t src_ip = rte_be_to_cpu_32(ip->src_addr);
        if (src_ip == 0) {
            ++stats.malformed;
            return;
        }
        if (stack->params_.verify_checksums && rte_ipv4_cksum(ip) != 0) {
            ++stats.bad_checksum;
            return;
        }
        if (ip->next_proto_id != IPPROTO_TCP) {
            return;
        }
        if ((rte_be_to_cpu_16(ip->fragment_offset) &
             (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
            ++stats.ip_fragments;
            return;
        }
        process_tcp(eth, ip, ip_hdr_len, ip_total_len, events);
    }

    // Recycles terminal slots whose close() has since arrived.
    void reap_released() noexcept {
        for (std::size_t i = 0; i < awaiting_release.size();) {
            const uint32_t idx = awaiting_release[i];
            if ((slot(idx).out_flags.load(std::memory_order_acquire) & out_released) != 0) {
                release_slot(idx);
                awaiting_release[i] = awaiting_release.back();
                awaiting_release.pop_back();
            } else {
                ++i;
            }
        }
    }

    void flush_acks() noexcept {
        for (const uint32_t idx : ack_list) {
            tcp_connection &c = slot(idx);
            if (!c.ack_pending) {
                continue; // cancelled by a reset
            }
            c.ack_pending = false;
            // seq = snd_nxt as the outbound thread last published it --
            // see the reordering note in tcp_stack.h.
            queue_control(build_segment(*stack->pool_, reply_addr(c),
                                        c.snd_nxt.load(std::memory_order_acquire), c.rcv_nxt_local,
                                        RTE_TCP_ACK_FLAG, stack->params_.receive_window, {}));
        }
        ack_list.clear();
    }

    void flush_control() noexcept {
        if (n_control == 0) {
            return;
        }
        const uint16_t sent = txq->send_burst({control, n_control});
        stats.control_tx_dropped += n_control - sent;
        for (std::size_t i = sent; i < n_control; ++i) {
            control[i] = packet{};
        }
        n_control = 0;
    }

    // Runs after the control flush, so a connection that just closed has
    // already had the ACK of the peer's FIN sent from its slot.
    void finalize() noexcept {
        for (const uint32_t idx : finalize_list) {
            tcp_connection &c = slot(idx);
            c.finalize_pending = false;
            flows.erase(flow_table::key(c.peer_ip, c.peer_port, c.local_port));
            if ((c.out_flags.load(std::memory_order_acquire) & out_released) != 0) {
                release_slot(idx);
            } else {
                awaiting_release.push_back(idx);
            }
        }
        finalize_list.clear();
    }
};

tcp_receiver::tcp_receiver(std::unique_ptr<impl> state) noexcept : impl_(std::move(state)) {}
tcp_receiver::tcp_receiver(tcp_receiver &&) noexcept = default;
tcp_receiver &tcp_receiver::operator=(tcp_receiver &&) noexcept = default;
tcp_receiver::~tcp_receiver() = default;

tcp_events tcp_receiver::poll(packet_burst &burst) noexcept {
    tcp_events events;
    impl_->reap_released();
    for (const packet &pkt : burst) {
        impl_->process(pkt, events);
    }
    impl_->flush_acks();
    impl_->flush_control();
    impl_->finalize();
    return events;
}

const tcp_stats &tcp_receiver::stats() const noexcept { return impl_->stats; }

tcp_receiver tcp_stack::receiver(std::shared_ptr<tx_queue> control_txq) {
    if (receiver_taken_) {
        throw dpdk_error("tcp_stack::receiver() may only be called once");
    }
    if (!control_txq) {
        throw dpdk_error("tcp_stack::receiver(): null control tx queue");
    }
    receiver_taken_ = true;
    return tcp_receiver(std::make_unique<tcp_receiver::impl>(this, std::move(control_txq)));
}

// ---------------------------------------------------------------------
// tcp_sender (outbound thread)

tcp_sender tcp_stack::sender(std::shared_ptr<tx_queue> data_txq) {
    if (sender_taken_) {
        throw dpdk_error("tcp_stack::sender() may only be called once");
    }
    if (!data_txq) {
        throw dpdk_error("tcp_stack::sender(): null data tx queue");
    }
    sender_taken_ = true;
    return tcp_sender(this, std::move(data_txq));
}

tcp_connection *tcp_sender::lookup(conn_id conn) const noexcept {
    if (conn.slot >= stack_->params_.max_connections) {
        return nullptr;
    }
    tcp_connection &c = stack_->slots_[conn.slot];
    if (c.generation.load(std::memory_order_acquire) != conn.generation) {
        return nullptr;
    }
    // If our flags look released-or-not because the inbound thread is
    // re-initializing this slot for a new connection right now, the
    // acquire here guarantees the re-check below sees its new generation.
    const uint8_t out = c.out_flags.load(std::memory_order_acquire);
    if (c.generation.load(std::memory_order_relaxed) != conn.generation ||
        (out & out_released) != 0) {
        return nullptr;
    }
    return &c;
}

bool tcp_sender::sendable(conn_id conn) const noexcept {
    const tcp_connection *c = lookup(conn);
    return c != nullptr && (c->out_flags.load(std::memory_order_relaxed) & out_fin_sent) == 0 &&
           c->terminated.load(std::memory_order_acquire) == 0;
}

std::size_t tcp_sender::send_space(conn_id conn) const noexcept {
    if (!sendable(conn)) {
        return 0;
    }
    const tcp_connection *c = lookup(conn);
    return window_space(c->send_window.load(std::memory_order_acquire),
                        c->snd_nxt.load(std::memory_order_relaxed));
}

std::size_t tcp_sender::send(conn_id conn, std::span<const std::byte> bytes) noexcept {
    if (bytes.empty() || !sendable(conn)) {
        return 0;
    }
    tcp_connection *c = lookup(conn);

    // We're snd_nxt's only writer, so a relaxed read of our own value is exact.
    const uint32_t base = c->snd_nxt.load(std::memory_order_relaxed);
    const std::size_t limit =
        std::min(bytes.size(), window_space(c->send_window.load(std::memory_order_acquire), base));
    if (limit == 0) {
        return 0;
    }
    const uint32_t ack = c->rcv_nxt.load(std::memory_order_acquire);
    const segment_addr a{stack_->local_mac_, c->peer_mac,  stack_->local_ip_,
                         c->peer_ip,         c->local_port, c->peer_port};
    const uint16_t window = stack_->params_.receive_window;

    std::size_t done = 0; // bytes accepted by the NIC so far
    while (done < limit) {
        // Build up to one tx burst's worth of segments.
        packet segs[packet_burst::max_size];
        std::size_t seg_end[packet_burst::max_size]; // offset just past each segment
        std::size_t n = 0;
        std::size_t off = done;
        while (n < packet_burst::max_size && off < limit) {
            const std::size_t len = std::min<std::size_t>(c->send_mss, limit - off);
            const uint8_t flags = RTE_TCP_ACK_FLAG | (off + len == limit ? RTE_TCP_PSH_FLAG : 0);
            segs[n] = build_segment(*stack_->pool_, a, base + static_cast<uint32_t>(off), ack,
                                    flags, window, bytes.subspan(off, len));
            if (!segs[n]) {
                break; // pool exhausted: send what we have
            }
            off += len;
            seg_end[n++] = off;
        }
        if (n == 0) {
            break;
        }

        // Publish before transmitting, so the inbound thread's view of
        // snd_nxt is never behind what's on the wire (tcp_stack.h).
        c->snd_nxt.store(base + static_cast<uint32_t>(off), std::memory_order_release);
        const uint16_t sent = txq_->send_burst({segs, n});
        if (sent < n) {
            // Unsent segments are freed with segs; pull snd_nxt back to
            // the end of the last one the NIC actually took.
            done = sent > 0 ? seg_end[sent - 1] : done;
            c->snd_nxt.store(base + static_cast<uint32_t>(done), std::memory_order_release);
            break;
        }
        done = off;
        if (n < packet_burst::max_size && off < limit) {
            break; // stopped early on pool exhaustion
        }
    }
    return done;
}

void tcp_sender::close(conn_id conn) noexcept {
    tcp_connection *c = lookup(conn);
    if (c == nullptr) {
        return;
    }
    uint8_t out = c->out_flags.load(std::memory_order_relaxed);
    if ((out & out_fin_sent) == 0 && c->terminated.load(std::memory_order_acquire) == 0) {
        const uint32_t seq = c->snd_nxt.load(std::memory_order_relaxed);
        const segment_addr a{stack_->local_mac_, c->peer_mac,  stack_->local_ip_,
                             c->peer_ip,         c->local_port, c->peer_port};
        // snd_nxt (now counting the FIN) before the flag, both before the
        // FIN hits the wire: the inbound thread loads the flag first, so
        // it always pairs fin_sent with the final snd_nxt.
        c->snd_nxt.store(seq + 1, std::memory_order_release);
        out |= out_fin_sent;
        c->out_flags.store(out, std::memory_order_release);

        packet fin[1];
        for (unsigned attempt = 0; attempt < kCloseRetries; ++attempt) {
            if (!fin[0]) {
                fin[0] = build_segment(*stack_->pool_, a, seq,
                                       c->rcv_nxt.load(std::memory_order_acquire),
                                       RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                                       stack_->params_.receive_window, {});
            }
            if (fin[0] && txq_->send_burst(fin) == 1) {
                break;
            }
        }
    }
    // Last touch of the slot from this thread: from here on the inbound
    // thread may recycle it as soon as the connection is also terminal.
    c->out_flags.store(out | out_released, std::memory_order_release);
}

} // namespace dpdk
