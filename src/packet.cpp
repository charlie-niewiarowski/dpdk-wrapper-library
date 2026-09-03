//
// Created by cniew on 8/29/26.
//

#include "packet.h"

#include <cstring>

namespace dpdk {

packet::packet(rte_mbuf *pkt) noexcept : pkt_(pkt) {}

packet::packet(packet &&other) noexcept : pkt_(other.pkt_) {
    other.pkt_ = nullptr;
}

packet &packet::operator=(packet &&other) noexcept {
    if (this != &other) {
        if (pkt_ != nullptr) {
            rte_pktmbuf_free(pkt_);
        }
        pkt_ = other.pkt_;
        other.pkt_ = nullptr;
    }
    return *this;
}

packet::~packet() {
    if (pkt_ != nullptr) {
        rte_pktmbuf_free(pkt_);
    }
}

rte_mbuf *packet::release() noexcept {
    rte_mbuf *pkt = pkt_;
    pkt_ = nullptr;
    return pkt;
}

uint8_t *packet::data() const noexcept {
    return rte_pktmbuf_mtod(pkt_, uint8_t *);
}

uint16_t packet::length() const noexcept {
    return pkt_->pkt_len;
}

uint8_t *packet::append(uint16_t len) noexcept {
    return reinterpret_cast<uint8_t *>(rte_pktmbuf_append(pkt_, len));
}

uint8_t *packet::append(Payload payload) noexcept {
    uint8_t *dst = append(static_cast<uint16_t>(payload.size()));
    if (dst == nullptr) {
        return nullptr;
    }
    std::memcpy(dst, payload.data(), payload.size());
    return dst;
}

void packet::set_l2_len(uint16_t len) noexcept { pkt_->l2_len = len; }

void packet::set_l3_len(uint16_t len) noexcept { pkt_->l3_len = len; }

} // namespace dpdk
