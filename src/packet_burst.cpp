//
// Created by cniew on 8/29/26.
//

#include "packet_burst.h"

#include <utility>

namespace dpdk {

void packet_burst::push_back(packet &&pkt) noexcept {
    ::new (static_cast<void *>(storage_ + count_ * sizeof(packet))) packet(std::move(pkt));
    ++count_;
}

packet_burst::packet_burst(packet_burst &&other) noexcept {
    for (std::size_t i = 0; i < other.count_; ++i) {
        push_back(std::move(*other.slot(i)));
    }
}

packet_burst &packet_burst::operator=(packet_burst &&other) noexcept {
    if (this != &other) {
        for (std::size_t i = 0; i < count_; ++i) {
            slot(i)->~packet();
        }
        count_ = 0;
        for (std::size_t i = 0; i < other.count_; ++i) {
            push_back(std::move(*other.slot(i)));
        }
    }
    return *this;
}

packet_burst::~packet_burst() {
    for (std::size_t i = 0; i < count_; ++i) {
        slot(i)->~packet();
    }
}

} // namespace dpdk
