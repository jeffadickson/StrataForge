#pragma once

#include <strata/core/types.hpp>
#include <strata/data/data_topic.hpp>
#include <strata/data/frame_lease.hpp>
#include <strata/ipc/blackboard.hpp>
#include <strata/ipc/critical_store.hpp>
#include <strata/ipc/event_ring.hpp>
#include <strata/ipc/param_bank.hpp>
#include <strata/ipc/spsc_channel.hpp>
#include <strata/runtime/partition_runtime.hpp>

#include <cstddef>
#include <cstdint>

namespace strata::runtime {

/// Parameterized per-partition configuration descriptors for STRATA-20 Subtopology Bundles.
/// Allows independent partitions within the same mission build to instantiate distinct queue
/// depths, pool capacities, and safety profiles with zero global configuration collisions.

struct CdhBundleConfig {
    uint8_t     partition_id{0U};
    Criticality criticality{Criticality::ClassB};
    size_t      cmd_queue_depth{32U};
    size_t      event_ring_depth{64U};
};

struct CommsBundleConfig {
    uint8_t     partition_id{1U};
    Criticality criticality{Criticality::ClassB};
    size_t      uplink_ring_depth{32U};
    size_t      downlink_ring_depth{64U};
    size_t      event_ring_depth{32U};
};

struct StorageBundleConfig {
    uint8_t     partition_id{2U};
    Criticality criticality{Criticality::ClassC};
    size_t      ingest_pool_slots{16U};
    size_t      ingest_slot_bytes{4096U};
    size_t      max_subscribers{4U};
    size_t      event_ring_depth{32U};
};

struct FdirBundleConfig {
    uint8_t     partition_id{3U};
    Criticality criticality{Criticality::ClassB};
    size_t      fault_ring_depth{32U};
    size_t      event_ring_depth{64U};
};

/// Minimal compact command envelope carried across partition SPSC boundaries.
struct alignas(8) CmdMessage {
    uint16_t opcode{0U};
    uint16_t flags{0U};
    uint32_t seq_id{0U};
    uint64_t arg0{0U};
    uint64_t arg1{0U};
};
static_assert(WirePod<CmdMessage>);

/// Minimal compact fault notification envelope forwarded to the FDIR partition.
struct alignas(8) FaultNotification {
    uint8_t  source_partition{0U};
    uint8_t  severity{0U};
    uint16_t fault_code{0U};
    uint16_t source_gen{0U};
    uint16_t slot_id{0xFFFFU};
    uint64_t tai_ns{0U};
    uint64_t aux_data{0U};
};
static_assert(WirePod<FaultNotification>);

/// Partition heartbeat published on a Blackboard for dual-layer health supervision.
struct alignas(8) PartitionHeartbeat {
    uint8_t  partition_id{0U};
    uint8_t  mode{0U};
    uint16_t gen_id{0U};
    uint32_t beat_count{0U};
    uint64_t tai_ns{0U};
    uint32_t cpu_used_us{0U};
    uint32_t overrun_count{0U};
};
static_assert(WirePod<PartitionHeartbeat>);

/// Composable Command & Data Handling Subtopology Bundle (STRATA-MW-016).
template <CdhBundleConfig Cfg, WirePod CheckpointT = PartitionHeartbeat>
struct CdhBundle {
    static constexpr CdhBundleConfig kConfig = Cfg;
    static_assert(PowerOfTwo<Cfg.cmd_queue_depth>, "CdhBundle cmd_queue_depth must be power of two");
    static_assert(PowerOfTwo<Cfg.event_ring_depth>, "CdhBundle event_ring_depth must be power of two");

    SpscRingStorage<CmdMessage, Cfg.cmd_queue_depth>     cmd_ingress_storage{};
    SpscProducer<CmdMessage, Cfg.cmd_queue_depth>        cmd_producer{cmd_ingress_storage, 0U, NullNotifier{}, GenId{0U}};
    SpscConsumer<CmdMessage, Cfg.cmd_queue_depth>        cmd_consumer{cmd_ingress_storage};
    SpscRingStorage<EventRecord, Cfg.event_ring_depth>   event_storage{};
    EventWriter<Cfg.event_ring_depth>                    events{event_storage, Cfg.partition_id};
    BlackboardStorage<PartitionHeartbeat>                heartbeat_storage{};
    BlackboardWriter<PartitionHeartbeat>                 heartbeat_writer{heartbeat_storage};
    CriticalStoreStorage<CheckpointT>                    checkpoint_storage{};
    CriticalStore<CheckpointT>                           checkpoint{checkpoint_storage, 0x5354'5241ULL};
};

/// Composable Communications Subtopology Bundle (STRATA-MW-016).
template <CommsBundleConfig Cfg>
struct CommsBundle {
    static constexpr CommsBundleConfig kConfig = Cfg;
    static_assert(PowerOfTwo<Cfg.uplink_ring_depth>, "CommsBundle uplink_ring_depth must be power of two");
    static_assert(PowerOfTwo<Cfg.downlink_ring_depth>, "CommsBundle downlink_ring_depth must be power of two");
    static_assert(PowerOfTwo<Cfg.event_ring_depth>, "CommsBundle event_ring_depth must be power of two");

    SpscRingStorage<FrameDesc, Cfg.uplink_ring_depth>    uplink_storage{};
    SpscProducer<FrameDesc, Cfg.uplink_ring_depth>       uplink_producer{uplink_storage, 0U, NullNotifier{}, GenId{0U}};
    SpscConsumer<FrameDesc, Cfg.uplink_ring_depth>       uplink_consumer{uplink_storage};
    SpscRingStorage<FrameDesc, Cfg.downlink_ring_depth>  downlink_storage{};
    SpscProducer<FrameDesc, Cfg.downlink_ring_depth>     downlink_producer{downlink_storage, 0U, NullNotifier{}, GenId{0U}};
    SpscConsumer<FrameDesc, Cfg.downlink_ring_depth>     downlink_consumer{downlink_storage};
    SpscRingStorage<EventRecord, Cfg.event_ring_depth>   event_storage{};
    EventWriter<Cfg.event_ring_depth>                    events{event_storage, Cfg.partition_id};
    BlackboardStorage<PartitionHeartbeat>                heartbeat_storage{};
    BlackboardWriter<PartitionHeartbeat>                 heartbeat_writer{heartbeat_storage};
};

/// Composable High-Rate Science Storage Subtopology Bundle (STRATA-MW-016).
template <StorageBundleConfig Cfg>
struct StorageBundle {
    static constexpr StorageBundleConfig kConfig = Cfg;
    static_assert(PowerOfTwo<Cfg.ingest_pool_slots>, "StorageBundle ingest_pool_slots must be power of two");
    static_assert(PowerOfTwo<Cfg.event_ring_depth>, "StorageBundle event_ring_depth must be power of two");

    using PoolT  = FramePool<Cfg.ingest_pool_slots, Cfg.ingest_slot_bytes>;
    using TopicT = DataTopic<PoolT, Cfg.max_subscribers, Cfg.ingest_pool_slots>;

    PoolT                                                pool;
    TopicT                                               ingest_topic;
    SpscRingStorage<EventRecord, Cfg.event_ring_depth>   event_storage{};
    EventWriter<Cfg.event_ring_depth>                    events{event_storage, Cfg.partition_id};
    BlackboardStorage<PartitionHeartbeat>                heartbeat_storage{};
    BlackboardWriter<PartitionHeartbeat>                 heartbeat_writer{heartbeat_storage};

    explicit StorageBundle(std::span<std::byte> pool_backing,
                           uint64_t             pool_phys_base = 0x8000'0000ULL) noexcept
        : pool(pool_backing, pool_phys_base), ingest_topic(pool) {}
};

/// Composable Fault Detection, Isolation, and Recovery Subtopology Bundle (STRATA-MW-016).
template <FdirBundleConfig Cfg>
struct FdirBundle {
    static constexpr FdirBundleConfig kConfig = Cfg;
    static_assert(PowerOfTwo<Cfg.fault_ring_depth>, "FdirBundle fault_ring_depth must be power of two");
    static_assert(PowerOfTwo<Cfg.event_ring_depth>, "FdirBundle event_ring_depth must be power of two");

    SpscRingStorage<FaultNotification, Cfg.fault_ring_depth> fault_storage{};
    SpscProducer<FaultNotification, Cfg.fault_ring_depth>    fault_producer{fault_storage, 0U, NullNotifier{}, GenId{0U}};
    SpscConsumer<FaultNotification, Cfg.fault_ring_depth>    fault_consumer{fault_storage};
    SpscRingStorage<EventRecord, Cfg.event_ring_depth>       event_storage{};
    EventWriter<Cfg.event_ring_depth>                        events{event_storage, Cfg.partition_id};
    BlackboardStorage<PartitionHeartbeat>                    heartbeat_storage{};
    BlackboardWriter<PartitionHeartbeat>                     heartbeat_writer{heartbeat_storage};
};

} // namespace strata::runtime
