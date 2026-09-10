//
// Created by cniew on 8/29/26.
//

#ifndef RUNTIME_H
#define RUNTIME_H

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "packet_pool.h"
#include "port.h"

namespace dpdk {

enum class eal_proc_type {
    primary,
    secondary,
    auto_detect,
};

enum class eal_iova_mode {
    pa,
    va,
};

// This is an aggregate on purpose so it can be built with designated
// initializers, naming only the params being set, e.g.:
//
//   dpdk::runtime_params params{
//       .core_list = "0-3",
//       .n_memory_channels = 4,
//       .vdevs = {"net_ring0"},
//   };
struct runtime_params {
    // never parsed as an option, but required to be present
    std::string program_name = "dpdk_app";

    // -l <core_list>, e.g. "0-3" or "0,2,4-7"
    std::optional<std::string> core_list;

    // --main-lcore <id>
    std::optional<unsigned> main_lcore;

    // -n <n_channels>
    std::optional<unsigned> n_memory_channels;

    // -m <n_mb> (legacy total-memory sizing; prefer socket_mem when pinning
    // memory to specific NUMA sockets)
    std::optional<unsigned> legacy_mem_mb;

    // --socket-mem <per-socket-MB-list>, e.g. "1024,1024"
    std::optional<std::string> socket_mem;

    // --socket-limit <per-socket-MB-list>
    std::optional<std::string> socket_limit;

    // --huge-dir <path>
    std::optional<std::string> huge_dir;

    // --file-prefix <prefix> (lets independent DPDK processes on the same
    // host use separate hugepage/shared-config namespaces)
    std::optional<std::string> file_prefix;

    // --proc-type primary|secondary|auto
    std::optional<eal_proc_type> proc_type;

    // --iova-mode pa|va
    std::optional<eal_iova_mode> iova_mode;

    // --log-level <level> or <component>:<level>
    std::optional<std::string> log_level;

    // --no-huge (run on regular pages instead of hugepages -- paired with
    // legacy_mem_mb in most no-hugepage/CI setups)
    std::optional<bool> no_huge;

    // --no-pci (skip PCI bus scanning entirely; typical for vdev-only runs)
    std::optional<bool> no_pci;

    // --in-memory (no shared-config/hugepage files on disk at all; implies
    // --no-shconf and fresh hugepages every run)
    std::optional<bool> in_memory;

    // -a <pci_id>, repeatable: PCI allowlist
    std::vector<std::string> pci_allow;

    // -b <pci_id>, repeatable: PCI blocklist
    std::vector<std::string> pci_block;

    // --vdev <driver_args>, repeatable, e.g. "net_ring0" or
    // "net_pcap0,rx_pcap=in.pcap,tx_pcap=out.pcap"
    std::vector<std::string> vdevs;

    // Escape hatch for any EAL flag not modeled above (appended verbatim,
    // in order, after everything else).
    std::vector<std::string> extra_args;
};

// Represents the DPDK runtime itself: brings up the EAL (hugepages,
// lcores, PCI/vdev probing) on construction and tears it down on
// destruction. Owns every port (and, transitively, every port's
// packet_pool)
// Exactly one runtime may exist per process at a time
class runtime {
public:
    static constexpr uint16_t default_elt_size = 2048;

    explicit runtime(runtime_params params);

    runtime(runtime &&) noexcept;
    runtime &operator=(runtime &&) noexcept;
    runtime(const runtime &) = delete;
    runtime &operator=(const runtime &) = delete;
    ~runtime();

    port &add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                    uint16_t elt_size = default_elt_size);
    port &add_port(uint16_t port_id, uint16_t n_rx_queues, uint16_t n_tx_queues,
                    std::shared_ptr<packet_pool> pool);
    port &get_port(uint16_t port_id);

    // Escape hatch for a custom pool (a different size class, or one
    // meant to be shared across several ports) without constructing
    // packet_pool directly -- its constructor is private to runtime.
    std::shared_ptr<packet_pool> create_pool(const char *name, unsigned n_mbufs,
                                              uint16_t elt_size, unsigned socket_id,
                                              unsigned cache_size = 256);

private:
    std::shared_ptr<packet_pool> make_default_pool(uint16_t port_id, uint16_t elt_size);

    bool owns_eal_ = true;
    // deque, not vector: add_port returns a reference into this
    // container, and unlike vector, deque never invalidates references
    // to existing elements when growing.
    std::deque<port> ports_;
};

} // namespace dpdk

#endif //RUNTIME_H
