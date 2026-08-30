//
// Created by cniew on 8/29/26.
//

#ifndef PACKET_POOL_H
#define PACKET_POOL_H

#include <cstdint>
#include <unordered_map>
#include "memory_pool.h"

namespace dpdk {

// Owned by instance. Maintains one memory_pool per NUMA socket, all
// sized uniformly (default 2KB, or overridden at construction). Users
// who need multiple size classes should manage memory_pool objects
// directly instead of going through packet_pool.
class packet_pool {
public:
    static constexpr uint16_t default_elt_size = 2048;

    explicit packet_pool(uint16_t elt_size = default_elt_size);

    packet_pool(packet_pool &&) noexcept = default;
    packet_pool &operator=(packet_pool &&) noexcept = default;
    packet_pool(const packet_pool &) = delete;
    packet_pool &operator=(const packet_pool &) = delete;

    // Allocates from the pool matching the calling thread's NUMA socket.
    packet allocate() const;

    // Escape hatch to reach a specific socket's pool directly.
    memory_pool &pool_for(unsigned socket_id);

private:
    std::unordered_map<unsigned, memory_pool> pools_;
};

} // namespace dpdk

#endif //PACKET_POOL_H
