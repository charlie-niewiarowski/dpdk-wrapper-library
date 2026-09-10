//
// Created by cniew on 8/29/26.
//

#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include "dpdk_error.h"

namespace dpdk {

namespace {

constexpr unsigned kDefaultNumMbufs = 8192;

// Guards against a second runtime being constructed while one is
// alive. Not a singleton accessor -- nothing external can read or
// reach this; it only ever answers "may a new runtime claim ownership."
std::atomic<bool> g_runtime_exists{false};

} // namespace

namespace {

// Appends flag, then value, only when value is engaged -- the single point
// through which an unset optional field means "say nothing to EAL" rather
// than "say something with a default value." Not used for optional<bool>
// fields: those are presence-only flags with no value to render, handled
// separately by append_flag below.
template <typename T>
void append_opt(std::vector<std::string> &args, const char *flag, const std::optional<T> &value) {
    if (!value) {
        return;
    }
    args.emplace_back(flag);
    args.push_back(std::to_string(*value));
}

void append_opt(std::vector<std::string> &args, const char *flag,
                 const std::optional<std::string> &value) {
    if (!value) {
        return;
    }
    args.emplace_back(flag);
    args.push_back(*value);
}

// Presence-only flag: emitted iff the optional is engaged *and* true.
// Explicitly set to false stays silent, same as unset -- there's no EAL
// syntax for "explicitly disable", so both mean "don't pass this flag."
void append_flag(std::vector<std::string> &args, const char *flag,
                  const std::optional<bool> &value) {
    if (value && *value) {
        args.emplace_back(flag);
    }
}

void append_repeated(std::vector<std::string> &args, const char *flag,
                      const std::vector<std::string> &values) {
    for (const std::string &v : values) {
        args.emplace_back(flag);
        args.push_back(v);
    }
}

const char *to_string(eal_proc_type type) {
    switch (type) {
        case eal_proc_type::primary:
            return "primary";
        case eal_proc_type::secondary:
            return "secondary";
        case eal_proc_type::auto_detect:
            return "auto";
    }
    return "auto";
}

const char *to_string(eal_iova_mode mode) {
    switch (mode) {
        case eal_iova_mode::pa:
            return "pa";
        case eal_iova_mode::va:
            return "va";
    }
    return "pa";
}

// Flattens runtime_params into the argv rte_eal_init() expects. Only
// fields the caller actually set (optionals that are engaged, vectors that
// aren't empty) contribute anything -- an unset field is invisible here,
// leaving EAL to fall back to its own default for it.
std::vector<std::string> build_eal_args(const runtime_params &params) {
    std::vector<std::string> args;
    args.push_back(params.program_name);

    append_opt(args, "-l", params.core_list);
    append_opt(args, "--main-lcore", params.main_lcore);
    append_opt(args, "-n", params.n_memory_channels);
    append_opt(args, "-m", params.legacy_mem_mb);
    append_opt(args, "--socket-mem", params.socket_mem);
    append_opt(args, "--socket-limit", params.socket_limit);
    append_opt(args, "--huge-dir", params.huge_dir);
    append_opt(args, "--file-prefix", params.file_prefix);
    if (params.proc_type) {
        args.emplace_back("--proc-type");
        args.emplace_back(to_string(*params.proc_type));
    }
    if (params.iova_mode) {
        args.emplace_back("--iova-mode");
        args.emplace_back(to_string(*params.iova_mode));
    }
    append_opt(args, "--log-level", params.log_level);
    append_flag(args, "--no-huge", params.no_huge);
    append_flag(args, "--no-pci", params.no_pci);
    append_flag(args, "--in-memory", params.in_memory);

    append_repeated(args, "-a", params.pci_allow);
    append_repeated(args, "-b", params.pci_block);
    append_repeated(args, "--vdev", params.vdevs);

    for (const std::string &extra : params.extra_args) {
        args.push_back(extra);
    }

    return args;
}

} // namespace

runtime::runtime(runtime_params params) {
    bool expected = false;
    if (!g_runtime_exists.compare_exchange_strong(expected, true)) {
        throw dpdk_error("Only one dpdk::runtime may exist per process");
    }

    // rte_eal_init() takes char *argv[], not char *const argv[] -- it (and
    // getopt underneath it) may permute entries during parsing, so each
    // element needs its own writable, stable-address storage. eal_args
    // owns that storage for the lifetime of the call; argv just points
    // into it.
    const std::vector<std::string> eal_args = build_eal_args(params);
    std::vector<char *> argv;
    argv.reserve(eal_args.size());
    for (const std::string &arg : eal_args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }

    const int ret = rte_eal_init(static_cast<int>(argv.size()), argv.data());
    if (ret < 0) {
        g_runtime_exists.store(false);
        throw dpdk_error(std::string("rte_eal_init failed: ") + rte_strerror(rte_errno));
    }
}

runtime::runtime(runtime &&other) noexcept
    : owns_eal_(other.owns_eal_), ports_(std::move(other.ports_)) {
    other.owns_eal_ = false;
}

runtime &runtime::operator=(runtime &&other) noexcept {
    if (this != &other) {
        if (owns_eal_) {
            // Ports (and the packet_pool each one owns) must be torn down
            // before the EAL is; rte_eal_cleanup() must be the last DPDK
            // call made.
            ports_.clear();
            rte_eal_cleanup();
            g_runtime_exists.store(false);
        }
        owns_eal_ = other.owns_eal_;
        ports_ = std::move(other.ports_);
        other.owns_eal_ = false;
    }
    return *this;
}

runtime::~runtime() {
    if (owns_eal_) {
        // Same ordering requirement as above: ports must go first.
        ports_.clear();
        rte_eal_cleanup();
        g_runtime_exists.store(false);
    }
}

std::shared_ptr<packet_pool> runtime::create_pool(const char *name, unsigned n_mbufs,
                                                   uint16_t elt_size, unsigned socket_id,
                                                   unsigned cache_size) {
    // Direct construction, not make_shared: packet_pool's constructor is
    // private (friend runtime), and make_shared's internal placement-new
    // happens inside <memory>'s own code, which isn't covered by that
    // friendship.
    return std::shared_ptr<packet_pool>(
        new packet_pool(name, n_mbufs, elt_size, socket_id, cache_size));
}

std::shared_ptr<packet_pool> runtime::make_default_pool(uint16_t port_id, uint16_t elt_size) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
        throw dpdk_error("Port " + std::to_string(port_id) + " is not a valid DPDK port");
    }
    const std::string name = "PORT_" + std::to_string(port_id) + "_POOL";
    const int socket_id = rte_eth_dev_socket_id(port_id);
    return create_pool(name.c_str(), kDefaultNumMbufs, elt_size,
                        static_cast<unsigned>(socket_id));
}

port &runtime::add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                         uint16_t elt_size) {
    return add_port(port_id, n_rx_queues, n_tx_queues, make_default_pool(port_id, elt_size));
}

port &runtime::add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                         std::shared_ptr<packet_pool> pool) {
    // port's constructor is private (friend runtime); deque::emplace_back
    // would construct in-place from inside the standard library's own
    // code, which isn't covered by that friendship either. Construct
    // directly here (a friend call, since it's textually inside a
    // runtime member function), then move the finished port in -- that
    // only needs port's public move constructor.
    port p(port_id, n_rx_queues, n_tx_queues, std::move(pool));
    ports_.push_back(std::move(p));
    return ports_.back();
}

port &runtime::get_port(uint16_t port_id) {
    auto it = std::find_if(ports_.begin(), ports_.end(), [port_id](const port &p) {
        return p.port_id() == port_id;
    });
    if (it == ports_.end()) {
        throw dpdk_error("No port with id " + std::to_string(port_id) +
                          " has been added to this runtime");
    }
    return *it;
}

} // namespace dpdk
