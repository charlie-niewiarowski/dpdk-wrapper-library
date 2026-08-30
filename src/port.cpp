//
// Created by cniew on 8/29/26.
//

#include "port.h"

#include <string>
#include <rte_ethdev.h>
#include "dpdk_error.h"

namespace dpdk {

namespace {

constexpr unsigned kDefaultNumMbufs = 8192;
constexpr uint16_t kDefaultRingSize = 1024;

void check_valid_port(uint16_t port_id) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        throw dpdk_error("Port " + std::to_string(port_id) + " is not a valid DPDK port");
    }
}

std::shared_ptr<packet_pool> make_default_pool(uint16_t port_id, uint16_t elt_size) {
    check_valid_port(port_id);
    const std::string name = "PORT_" + std::to_string(port_id) + "_POOL";
    const int socket_id = rte_eth_dev_socket_id(port_id);
    return std::make_shared<packet_pool>(name.c_str(), kDefaultNumMbufs, elt_size,
                                          static_cast<unsigned>(socket_id));
}

} // namespace

port::port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues, uint16_t elt_size)
    : port(port_id, n_rx_queues, n_tx_queues, make_default_pool(port_id, elt_size)) {}

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

    if (rte_eth_dev_start(port_id_) < 0) {
        throw dpdk_error("rte_eth_dev_start failed on port " + std::to_string(port_id_));
    }
}

port::port(port &&other) noexcept
    : port_id_(other.port_id_),
      pool_(std::move(other.pool_)),
      rx_queues_(std::move(other.rx_queues_)),
      tx_queues_(std::move(other.tx_queues_)),
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

std::shared_ptr<rx_queue> port::get_rx_queue(uint16_t queue_id) const {
    return rx_queues_.at(queue_id);
}

std::shared_ptr<tx_queue> port::get_tx_queue(uint16_t queue_id) const {
    return tx_queues_.at(queue_id);
}

packet_burst port::receive_burst(uint16_t queue_id, std::size_t max_count) const {
    return rx_queues_.at(queue_id)->receive_burst(max_count);
}

uint16_t port::send_burst(uint16_t queue_id, std::span<packet> pkts) const {
    return tx_queues_.at(queue_id)->send_burst(pkts);
}

uint16_t port::port_id() const noexcept { return port_id_; }

} // namespace dpdk
