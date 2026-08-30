//
// Created by cniew on 8/29/26.
//

#ifndef INSTANCE_H
#define INSTANCE_H

#include <cstdint>
#include <vector>
#include "port.h"

namespace dpdk {

// Represents the DPDK runtime itself: brings up the EAL (hugepages,
// lcores, PCI/vdev probing) on construction and tears it down on
// destruction. Owns every port configured through it.
class instance {
public:
    instance(int argc, char **argv);

    instance(instance &&) noexcept;
    instance &operator=(instance &&) noexcept;
    instance(const instance &) = delete;
    instance &operator=(const instance &) = delete;
    ~instance();

    port &add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                    uint16_t elt_size = port::default_elt_size);
    port &get_port(uint16_t port_id);

private:
    bool owns_eal_ = true;
    std::vector<port> ports_;
};

} // namespace dpdk

#endif //INSTANCE_H
