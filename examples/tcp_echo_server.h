// A TCP echo server in the exchange gateway's shape: one inbound thread
// that owns every RX queue plus the control TX queue and runs
// tcp_receiver::poll(), one outbound thread that owns the data TX queue
// and runs tcp_sender, and a lock-free SPSC ring between them carrying
// "echo these bytes" / "peer closed" / "peer reset" work items.
//
// Header-only so the tap example (examples/tcp_echo.cpp) and the
// privileged integration test (tests/tap_integration_test.cpp) run the
// same code.

#ifndef TCP_ECHO_SERVER_H
#define TCP_ECHO_SERVER_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include <pthread.h>
#include <sched.h>

#include "port.h"
#include "tcp_stack.h"

namespace echo {

// Bounded single-producer/single-consumer ring. The producer only writes
// head_, the consumer only writes tail_; each reads the other's with
// acquire. Capacity must be a power of two.
template <typename T, std::size_t Capacity>
class spsc_ring {
    static_assert((Capacity & (Capacity - 1)) == 0);

public:
    // Producer side. Returns a slot to fill and commit(), or nullptr if full.
    T *reserve() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) {
            return nullptr;
        }
        return &items_[head & (Capacity - 1)];
    }
    void commit() noexcept { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    // Consumer side. Returns the oldest item, or nullptr if empty; pop()
    // once done with it.
    T *front() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return &items_[tail & (Capacity - 1)];
    }
    void pop() noexcept { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    T items_[Capacity];
};

struct work_item {
    enum class kind : uint8_t { data, peer_closed, reset };
    kind what;
    dpdk::conn_id conn;
    uint16_t len;
    // One segment's payload always fits: it's bounded by the MTU.
    std::byte bytes[2048];
};

// rte_eal_init pins the calling thread to the main lcore, and every
// std::thread it spawns inherits that one-CPU mask. A real gateway pins
// each thread to its own dedicated core; the example just undoes the
// inherited pin so the two threads don't share one CPU.
inline void unpin_this_thread() {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        CPU_SET(c, &all);
    }
    pthread_setaffinity_np(pthread_self(), sizeof(all), &all);
}

class server {
public:
    // p must have been added with n_queues >= 2 RX and TX queues (tap
    // requires the counts to match) and have its IPv4 address set. Queue
    // 0 is the control TX queue, queue 1 the data TX queue, and every RX
    // queue belongs to the inbound thread.
    server(dpdk::port &p, uint16_t n_queues, uint16_t listen_port,
           dpdk::tcp_params params = {})
        : stack_(p, params),
          rx_(stack_.receiver(p.claim_tx_queue(uint16_t{0}))),
          tx_(stack_.sender(p.claim_tx_queue(uint16_t{1}))),
          ring_(std::make_unique<ring_type>()),
          released_gen_(params.max_connections, 0) {
        stack_.listen(listen_port);
        for (uint16_t q = 0; q < n_queues; ++q) {
            rxqs_.push_back(p.claim_rx_queue(q));
        }
    }

    // Inbound thread body.
    void run_inbound(const std::atomic<bool> &stop) {
        unpin_this_thread();
        while (!stop.load(std::memory_order_relaxed)) {
            drain_overflow();
            // Tap steers a flow to whichever queue it was last written
            // on (control or data), so every RX queue has to be drained.
            for (const auto &rxq : rxqs_) {
                dpdk::packet_burst burst = rxq->receive_burst();
                for (const dpdk::tcp_event &ev : rx_.poll(burst)) {
                    handle(ev);
                }
            }
        }
    }

    // Outbound thread body.
    void run_outbound(const std::atomic<bool> &stop) {
        unpin_this_thread();
        while (!stop.load(std::memory_order_relaxed)) {
            work_item *item = ring_->front();
            if (item == nullptr) {
                continue;
            }
            switch (item->what) {
                case work_item::kind::data: {
                    // Retry until the window takes it all, or the
                    // connection dies under us (then drop the rest).
                    std::size_t sent = 0;
                    while (sent < item->len && tx_.sendable(item->conn) &&
                           !stop.load(std::memory_order_relaxed)) {
                        sent += tx_.send(item->conn, {item->bytes + sent, item->len - sent});
                    }
                    break;
                }
                case work_item::kind::peer_closed:
                    // Everything the peer sent is already echoed (the ring
                    // is FIFO), so finish our direction too.
                    close_once(item->conn);
                    break;
                case work_item::kind::reset:
                    close_once(item->conn); // still owed: it frees the slot
                    break;
            }
            ring_->pop();
        }
    }

    const dpdk::tcp_stats &stats() const noexcept { return rx_.stats(); }
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> finished{0}; // closed + reset

private:
    using ring_type = spsc_ring<work_item, 4096>;

    void handle(const dpdk::tcp_event &ev) {
        using dpdk::tcp_event_type;
        switch (ev.type) {
            case tcp_event_type::accepted:
                accepted.fetch_add(1, std::memory_order_relaxed);
                return;
            case tcp_event_type::data:
                for (std::size_t off = 0; off < ev.data.size();) {
                    work_item item;
                    const std::size_t n = std::min(ev.data.size() - off, sizeof(item.bytes));
                    item.what = work_item::kind::data;
                    item.conn = ev.conn;
                    item.len = static_cast<uint16_t>(n);
                    std::memcpy(item.bytes, ev.data.data() + off, n);
                    enqueue(item);
                    off += n;
                }
                return;
            case tcp_event_type::peer_closed:
            case tcp_event_type::reset: {
                work_item item;
                item.what = ev.type == tcp_event_type::reset ? work_item::kind::reset
                                                             : work_item::kind::peer_closed;
                item.conn = ev.conn;
                item.len = 0;
                enqueue(item);
                if (ev.type == tcp_event_type::reset) {
                    finished.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            case tcp_event_type::closed:
                // Only reachable after our close(), so nothing is owed.
                finished.fetch_add(1, std::memory_order_relaxed);
                return;
        }
    }

    // The inbound thread must never wait for the outbound thread. The
    // outbound thread can be stuck on a full peer window that only an ACK
    // can open -- and ACKs are processed by the inbound thread's poll().
    // Blocking here on a full ring deadlocks the two (reproducible with a
    // small ring and a fast client). tcp_stack has no receive-side
    // backpressure (its window is constant), so the application has to
    // absorb bursts: spill to an unbounded inbound-owned queue, refilled
    // into the ring as space frees. A gateway needs the same rule.
    void enqueue(const work_item &item) {
        if (overflow_.empty()) {
            if (work_item *slot = ring_->reserve()) {
                copy_item(*slot, item);
                ring_->commit();
                return;
            }
        }
        overflow_.push_back(item);
    }

    void drain_overflow() {
        while (!overflow_.empty()) {
            work_item *slot = ring_->reserve();
            if (slot == nullptr) {
                return;
            }
            copy_item(*slot, overflow_.front());
            ring_->commit();
            overflow_.pop_front();
        }
    }

    static void copy_item(work_item &dst, const work_item &src) {
        dst.what = src.what;
        dst.conn = src.conn;
        dst.len = src.len;
        std::memcpy(dst.bytes, src.bytes, src.len);
    }

    // close() must happen exactly once per connection; a peer FIN
    // followed by a reset would otherwise ask twice. Outbound-thread only.
    void close_once(dpdk::conn_id conn) {
        uint32_t &gen = released_gen_[conn.slot];
        if (gen != conn.generation) {
            gen = conn.generation;
            tx_.close(conn);
        }
    }

    dpdk::tcp_stack stack_;
    dpdk::tcp_receiver rx_;
    dpdk::tcp_sender tx_;
    std::vector<std::shared_ptr<dpdk::rx_queue>> rxqs_;
    std::unique_ptr<ring_type> ring_;
    std::vector<uint32_t> released_gen_; // per slot; outbound thread only
    std::deque<work_item> overflow_;     // inbound thread only
};

} // namespace echo

#endif // TCP_ECHO_SERVER_H
