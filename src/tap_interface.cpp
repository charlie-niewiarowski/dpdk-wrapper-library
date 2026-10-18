//
// Kernel-side configuration for the net_tap path; see tap_interface.h.
//

#include "tap_interface.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <rte_flow.h>

#include "dpdk_error.h"

namespace dpdk {

namespace {

// Closes the ioctl socket on every exit path, including throws.
struct socket_fd {
    int fd;
    ~socket_fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

[[noreturn]] void fail(const std::string &ifname, const char *what) {
    throw dpdk_error("configure_kernel_interface(" + ifname + "): " + what + " failed: " +
                      std::strerror(errno));
}

void set_inet_addr(sockaddr &sa, uint32_t addr) {
    auto *sin = reinterpret_cast<sockaddr_in *>(&sa);
    std::memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(addr);
}

} // namespace

void configure_kernel_interface(const std::string &ifname, uint32_t addr, uint8_t prefix_len) {
    if (ifname.empty() || ifname.size() >= IFNAMSIZ) {
        throw dpdk_error("configure_kernel_interface: bad interface name \"" + ifname + "\"");
    }
    if (prefix_len > 32) {
        throw dpdk_error("configure_kernel_interface: prefix length must be 0-32");
    }

    socket_fd sock{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (sock.fd < 0) {
        fail(ifname, "socket");
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);

    set_inet_addr(ifr.ifr_addr, addr);
    if (::ioctl(sock.fd, SIOCSIFADDR, &ifr) < 0) {
        fail(ifname, "SIOCSIFADDR");
    }

    // Setting the netmask after the address replaces the kernel's
    // classful guess and installs the connected route for the subnet.
    const uint32_t mask = prefix_len == 0 ? 0 : ~uint32_t{0} << (32 - prefix_len);
    set_inet_addr(ifr.ifr_netmask, mask);
    if (::ioctl(sock.fd, SIOCSIFNETMASK, &ifr) < 0) {
        fail(ifname, "SIOCSIFNETMASK");
    }

    // The PMD already brings the link up on rte_eth_dev_start; doing it
    // here too makes the helper order-independent.
    if (::ioctl(sock.fd, SIOCGIFFLAGS, &ifr) < 0) {
        fail(ifname, "SIOCGIFFLAGS");
    }
    ifr.ifr_flags |= IFF_UP;
    if (::ioctl(sock.fd, SIOCSIFFLAGS, &ifr) < 0) {
        fail(ifname, "SIOCSIFFLAGS");
    }
}

void steer_rx_to_queue(const port &p, uint16_t queue_id) {
    const rte_flow_attr attr{.ingress = 1};
    // An ETH item with no spec/mask matches every frame.
    const rte_flow_item pattern[] = {
        {.type = RTE_FLOW_ITEM_TYPE_ETH},
        {.type = RTE_FLOW_ITEM_TYPE_END},
    };
    const rte_flow_action_queue queue{.index = queue_id};
    const rte_flow_action actions[] = {
        {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue},
        {.type = RTE_FLOW_ACTION_TYPE_END},
    };
    rte_flow_error error{};
    if (rte_flow_create(p.port_id(), &attr, pattern, actions, &error) == nullptr) {
        throw dpdk_error("steer_rx_to_queue: port " + std::to_string(p.port_id()) + " queue " +
                          std::to_string(queue_id) + ": " +
                          (error.message != nullptr ? error.message : "rte_flow_create failed"));
    }
}

} // namespace dpdk
