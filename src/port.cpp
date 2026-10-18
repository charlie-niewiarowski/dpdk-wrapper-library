//
// Created by cniew on 8/29/26.
//

#include "port.h"

#include <string>
#include <rte_ethdev.h>
#include "dpdk_error.h"

namespace dpdk {

namespace {

constexpr uint16_t kDefaultRingSize = 1024;

void check_valid_port(uint16_t port_id) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        throw dpdk_error("Port " + std::to_string(port_id) + " is not a valid DPDK port");
    }
}

} // namespace

port::port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
           std::shared_ptr<packet_pool> pool)
    : port_id_(port_id), pool_(std::move(pool)) {
    check_valid_port(port_id_);

    rte_eth_conf port_conf{};
    if (rte_eth_dev_configure(port_id_, n_rx_queues, n_tx_queues, &port_conf) != 0) {
        throw dpdk_error("rte_eth_dev_configure failed on port " + std::to_string(port_id_));
    }

    uint16_t nb_rxd = kDefaultRingSize;
    uint16_t nb_txd = kDefaultRingSize;
    if (rte_eth_dev_adjust_nb_rx_tx_desc(port_id_, &nb_rxd, &nb_txd) != 0) {
        throw dpdk_error("rte_eth_dev_adjust_nb_rx_tx_desc failed on port " +
                          std::to_string(port_id_));
    }

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id_, &dev_info) != 0) {
        throw dpdk_error("rte_eth_dev_info_get failed on port " + std::to_string(port_id_));
    }

    const int socket_id = rte_eth_dev_socket_id(port_id_);

    for (uint16_t q = 0; q < n_rx_queues; ++q) {
        if (rte_eth_rx_queue_setup(port_id_, q, nb_rxd, socket_id, nullptr,
                                    pool_->native_handle()) < 0) {
            throw dpdk_error("rte_eth_rx_queue_setup failed on port " +
                              std::to_string(port_id_) + " queue " + std::to_string(q));
        }
        rx_queues_.push_back(std::shared_ptr<rx_queue>(new rx_queue(port_id_, q)));
    }

    rte_eth_txconf txconf = dev_info.default_txconf;
    for (uint16_t q = 0; q < n_tx_queues; ++q) {
        if (rte_eth_tx_queue_setup(port_id_, q, nb_txd, socket_id, &txconf) < 0) {
            throw dpdk_error("rte_eth_tx_queue_setup failed on port " +
                              std::to_string(port_id_) + " queue " + std::to_string(q));
        }
        tx_queues_.push_back(std::shared_ptr<tx_queue>(new tx_queue(port_id_, q)));
    }

    rx_claimed_.assign(rx_queues_.size(), 0);
    tx_claimed_.assign(tx_queues_.size(), 0);

    if (rte_eth_dev_start(port_id_) < 0) {
        throw dpdk_error("rte_eth_dev_start failed on port " + std::to_string(port_id_));
    }
}

port::port(port &&other) noexcept
    : port_id_(other.port_id_),
      pool_(std::move(other.pool_)),
      rx_queues_(std::move(other.rx_queues_)),
      tx_queues_(std::move(other.tx_queues_)),
      next_rx_claim_(other.next_rx_claim_),
      next_tx_claim_(other.next_tx_claim_),
      rx_claimed_(std::move(other.rx_claimed_)),
      tx_claimed_(std::move(other.tx_claimed_)),
      ipv4_addr_(other.ipv4_addr_),
      owns_device_(other.owns_device_) {
    other.owns_device_ = false;
}

port &port::operator=(port &&other) noexcept {
    if (this != &other) {
        if (owns_device_) {
            rte_eth_dev_stop(port_id_);
            rte_eth_dev_close(port_id_);
        }
        port_id_ = other.port_id_;
        pool_ = std::move(other.pool_);
        rx_queues_ = std::move(other.rx_queues_);
        tx_queues_ = std::move(other.tx_queues_);
        next_rx_claim_ = other.next_rx_claim_;
        next_tx_claim_ = other.next_tx_claim_;
        rx_claimed_ = std::move(other.rx_claimed_);
        tx_claimed_ = std::move(other.tx_claimed_);
        ipv4_addr_ = other.ipv4_addr_;
        owns_device_ = other.owns_device_;
        other.owns_device_ = false;
    }
    return *this;
}

port::~port() {
    if (owns_device_) {
        rte_eth_dev_stop(port_id_);
        rte_eth_dev_close(port_id_);
    }
}

namespace {

// Shared body of the "next unclaimed" form for both queue kinds: advances
// the cursor past anything already claimed by id before handing one out.
template <typename Queue>
std::shared_ptr<Queue> claim_next(std::vector<std::shared_ptr<Queue>> &queues,
                                   std::vector<char> &claimed, std::size_t &cursor) {
    while (cursor < queues.size() && claimed[cursor]) {
        ++cursor;
    }
    if (cursor >= queues.size()) {
        return nullptr;
    }
    claimed[cursor] = 1;
    return queues[cursor++];
}

template <typename Queue>
std::shared_ptr<Queue> claim_by_id(std::vector<std::shared_ptr<Queue>> &queues,
                                    std::vector<char> &claimed, uint16_t queue_id,
                                    uint16_t port_id, const char *kind) {
    if (queue_id >= queues.size()) {
        throw dpdk_error(std::string("Port ") + std::to_string(port_id) + " has no " + kind +
                          " queue " + std::to_string(queue_id));
    }
    if (claimed[queue_id]) {
        return nullptr;
    }
    claimed[queue_id] = 1;
    return queues[queue_id];
}

} // namespace

std::shared_ptr<rx_queue> port::claim_rx_queue() {
    return claim_next(rx_queues_, rx_claimed_, next_rx_claim_);
}

std::shared_ptr<tx_queue> port::claim_tx_queue() {
    return claim_next(tx_queues_, tx_claimed_, next_tx_claim_);
}

std::shared_ptr<rx_queue> port::claim_rx_queue(uint16_t queue_id) {
    return claim_by_id(rx_queues_, rx_claimed_, queue_id, port_id_, "RX");
}

std::shared_ptr<tx_queue> port::claim_tx_queue(uint16_t queue_id) {
    return claim_by_id(tx_queues_, tx_claimed_, queue_id, port_id_, "TX");
}

packet_burst port::receive_burst(uint16_t queue_id, std::size_t max_count) const {
    return rx_queues_.at(queue_id)->receive_burst(max_count);
}

uint16_t port::send_burst(uint16_t queue_id, std::span<packet> pkts) const {
    return tx_queues_.at(queue_id)->send_burst(pkts);
}

uint16_t port::port_id() const noexcept { return port_id_; }

rte_ether_addr port::mac_address() const {
    rte_ether_addr mac{};
    if (rte_eth_macaddr_get(port_id_, &mac) != 0) {
        throw dpdk_error("rte_eth_macaddr_get failed on port " + std::to_string(port_id_));
    }
    return mac;
}

} // namespace dpdk
