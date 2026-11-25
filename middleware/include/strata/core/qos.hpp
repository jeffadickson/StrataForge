#pragma once

#include <strata/core/types.hpp>

#include <cstddef>
#include <cstdint>

namespace strata {

/// Static DDS-inspired Durability contract (zero runtime discovery overhead).
enum class Durability : uint8_t {
    Volatile       = 0U, // Reader only accepts samples published after attach/read baseline
    TransientLocal = 1U, // Restarted partition immediately sees last valid published snapshot
};

/// Static DDS-inspired Reliability & back-pressure contract for rings and topics.
enum class Reliability : uint8_t {
    Guaranteed           = 0U, // Reject new writes with Status::Full (Class B/C lossless paths)
    BestEffortDropOldest = 1U, // Overwrite or skip stalled Class D consumer without blocking producer
};

/// Compile-time Quality-of-Service policy bundle attached to Blackboards, Channels, and DataTopics.
struct QoSConfig {
    Durability  durability{Durability::TransientLocal};
    Reliability reliability{Reliability::Guaranteed};
    DurationNs  deadline{0U};       // 0 = disabled; >0 flags Status::DeadlineMissed on stale reads
    DurationNs  liveliness{0U};     // 0 = disabled; >0 escalates missing heartbeat to FdirEngine
    DurationNs  lease_timeout{0U};  // 0 = disabled; >0 enables crash-safe DDR slot reclamation
    uint32_t    history_depth{1U};  // Static capacity hint
};

/// Canonical preset QoS profiles for common flight patterns.
inline constexpr QoSConfig kDefaultBlackboardQoS{
    .durability    = Durability::TransientLocal,
    .reliability   = Reliability::BestEffortDropOldest,
    .deadline      = DurationNs{0U},
    .liveliness    = DurationNs{0U},
    .lease_timeout = DurationNs{0U},
    .history_depth = 1U,
};

inline constexpr QoSConfig kGuaranteedControlQoS{
    .durability    = Durability::Volatile,
    .reliability   = Reliability::Guaranteed,
    .deadline      = DurationNs{0U},
    .liveliness    = DurationNs{0U},
    .lease_timeout = DurationNs{0U},
    .history_depth = 16U,
};

inline constexpr QoSConfig kBestEffortScienceQoS{
    .durability    = Durability::Volatile,
    .reliability   = Reliability::BestEffortDropOldest,
    .deadline      = DurationNs{0U},
    .liveliness    = DurationNs{0U},
    .lease_timeout = DurationNs{50'000'000ULL}, // 50 ms default lease timeout
    .history_depth = 8U,
};

} // namespace strata
