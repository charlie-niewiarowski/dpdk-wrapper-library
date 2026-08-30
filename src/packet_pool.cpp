//
// Created by cniew on 8/29/26.
//

#include "packet_pool.h"

#include <string>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include "dpdk_error.h"

namespace dpdk {

packet_pool::packet_pool(const char *name, unsigned n_mbufs, uint16_t elt_size,
                          unsigned socket_id, unsigned cache_size)
    : pool_(rte_pktmbuf_pool_create(name, n_mbufs, cache_size, /*priv_size=*/0,
                                     elt_size, static_cast<int>(socket_id))) {
    if (pool_ == nullptr) {
        throw dpdk_error(std::string("rte_pktmbuf_pool_create(\"") + name +
                          "\") failed: " + rte_strerror(rte_errno));
    }
}

packet_pool::packet_pool(packet_pool &&other) noexcept : pool_(other.pool_) {
    other.pool_ = nullptr;
}

packet_pool &packet_pool::operator=(packet_pool &&other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr) {
            rte_mempool_free(pool_);
        }
        pool_ = other.pool_;
        other.pool_ = nullptr;
    }
    return *this;
}

packet_pool::~packet_pool() {
    if (pool_ != nullptr) {
        rte_mempool_free(pool_);
    }
}

packet packet_pool::get() const {
    return get_packet(rte_pktmbuf_alloc(pool_));
}

packet packet_pool::get_packet(rte_mbuf *mbuf) noexcept {
    return packet(mbuf);
}

unsigned packet_pool::socket_id() const noexcept {
    return static_cast<unsigned>(pool_->socket_id);
}

} // namespace dpdk
