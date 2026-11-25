#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/hyp/abi.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::hyp {

/// Static hardware memory & MMIO region permission flags (Stage-2 MMU / MPU / XMPU).
enum class MemPerm : uint8_t {
    None      = 0U,
    ReadOnly  = 1U,
    ReadWrite = 2U,
    ReadExec  = 3U,
    DeviceRw  = 4U,
};

/// Static memory/MMIO isolation region descriptor.
struct MemRegionDesc {
    uint8_t  owner_partition{0U};
    MemPerm  perm{MemPerm::None};
    uint16_t flags{0U}; // Bit 0: CriticalStore (preserved across WarmRestart), Bit 1: SharedConsumerRO
    uint64_t phys_base{0U};
    uint64_t size_bytes{0U};
};

inline constexpr uint16_t kRegionCriticalStore    = 1U << 0U;
inline constexpr uint16_t kRegionSharedConsumerRo = 1U << 1U;
inline constexpr uint16_t kRegionStackOrBss       = 1U << 2U;

/// Hardware trap diagnostic record captured by the hypervisor and forwarded to unprivileged FDIR.
struct TrapContext {
    uint8_t  partition_id{0U};
    uint8_t  trap_kind{0U}; // 0=GuestFaultReport, 1=DataAbort, 2=InstrAbort, 3=Align, 4=Undef, 5=Priv
    uint16_t gen_id{0U};
    uint64_t pc{0U};
    uint64_t lr{0U};
    uint64_t syndrome{0U};
    uint64_t fault_addr{0U};
    uint64_t tai_ns{0U};
};

/// Static per-partition descriptor configured from mission.toml.
struct PartitionConfig {
    uint8_t     partition_id{0U};
    Criticality criticality{Criticality::ClassD};
    uint8_t     cpu_id{0U};
    bool        privileged_slot_ctl{false}; // True only for Class B SlotManager / FDIR
    bool        privileged_part_ctl{false}; // True only for Class B FDIR supervisor
    uint32_t    budget_us{1000U};
    uint32_t    period_minor_frames{1U};
    uint32_t    offset_minor_frames{0U};
};

/// Static doorbell authorization & token-bucket rate limit entry (STRATA-HYP-004).
struct DoorbellChannelConfig {
    uint16_t channel_id{0U};
    uint8_t  src_partition{0U};
    uint8_t  dst_partition{0U};
    uint32_t max_tokens{4U};
    uint32_t refill_per_minor_frame{1U};
};

/// Architecture Backend Concept implemented by Versal A72 EL2, R5F MPU, Kintex MicroBlaze, & Host SIL.
template <typename B>
concept HypervisorArchBackend = requires(B& b, uint8_t pid, const MemRegionDesc& reg,
                                         uint16_t channel_id, uint16_t slot_id, SlotOpCode op,
                                         uint64_t arg, uint64_t diag_word) {
    { b.program_region(reg) } noexcept -> std::same_as<Status>;
    { b.activate_partition_context(pid) } noexcept -> std::same_as<Status>;
    { b.send_ipi(pid, channel_id) } noexcept -> std::same_as<Status>;
    { b.scrub_partition_ram(pid, bool{}) } noexcept -> std::same_as<Status>;
    { b.execute_slot_op(slot_id, op, arg) } noexcept -> std::same_as<Status>;
    { b.write_flight_recorder(pid, diag_word) } noexcept -> std::same_as<Status>;
    { b.read_hw_tai_ns() } noexcept -> std::same_as<uint64_t>;
};

/// ============================================================================
/// Static Spatial Memory & Peripheral Isolation Table (STRATA-HYP-003)
/// Enforces non-overlapping writable regions across partitions while allowing
/// explicit Read-Only consumer mappings to shared producer regions.
/// ============================================================================
template <size_t MaxRegions = 64U>
class MemoryIsolationTable {
public:
    [[nodiscard]] Status add_region(const MemRegionDesc& desc) noexcept {
        if (desc.size_bytes == 0U || (desc.phys_base % 4096U) != 0U ||
            (desc.size_bytes % 4096U) != 0U) {
            return Status::InvalidArg;
        }
        const uint64_t new_end = desc.phys_base + desc.size_bytes;
        if (new_end < desc.phys_base) return Status::InvalidArg;

        for (size_t i = 0U; i < count_; ++i) {
            const auto& r     = regions_[i];
            const uint64_t r_end = r.phys_base + r.size_bytes;
            const bool overlaps  = (desc.phys_base < r_end) && (r.phys_base < new_end);
            if (!overlaps) continue;

            // Only allow overlap across distinct partitions if the incoming mapping is ReadOnly
            // shared-consumer (or existing is ReadOnly shared-consumer and incoming is its single writer).
            if (r.owner_partition == desc.owner_partition) {
                return Status::ValidationFailed;
            }
            const bool incoming_ro =
                (desc.perm == MemPerm::ReadOnly) && ((desc.flags & kRegionSharedConsumerRo) != 0U);
            const bool existing_ro =
                (r.perm == MemPerm::ReadOnly) && ((r.flags & kRegionSharedConsumerRo) != 0U);
            if (!incoming_ro && !existing_ro) {
                return Status::PermissionDenied; // Disallow dual-writer overlap across partitions!
            }
        }

        if (count_ >= MaxRegions) return Status::Full;
        regions_[count_++] = desc;
        return Status::Ok;
    }

    /// Check if `partition_id` has permission to access `[addr, addr + len)` for write or read.
    [[nodiscard]] bool check_access(uint8_t partition_id, uint64_t addr, uint64_t len,
                                    bool is_write) const noexcept {
        if (len == 0U || addr + len < addr) return false;
        for (size_t i = 0U; i < count_; ++i) {
            const auto& r = regions_[i];
            if (r.owner_partition != partition_id) continue;
            if (addr >= r.phys_base && (addr + len) <= (r.phys_base + r.size_bytes)) {
                if (!is_write) return r.perm != MemPerm::None;
                return r.perm == MemPerm::ReadWrite || r.perm == MemPerm::DeviceRw;
            }
        }
        return false;
    }

    [[nodiscard]] std::span<const MemRegionDesc> regions() const noexcept {
        return std::span<const MemRegionDesc>{regions_.data(), count_};
    }

private:
    std::array<MemRegionDesc, MaxRegions> regions_{};
    size_t                                count_{0U};
};

/// ============================================================================
/// Doorbell ACL & Token-Bucket Rate Limiter (STRATA-HYP-004)
/// ============================================================================
template <size_t MaxChannels = 32U>
class DoorbellRouter {
public:
    [[nodiscard]] Status register_channel(const DoorbellChannelConfig& cfg) noexcept {
        if (cfg.max_tokens == 0U) return Status::InvalidArg;
        for (size_t i = 0U; i < count_; ++i) {
            if (channels_[i].cfg.channel_id == cfg.channel_id) return Status::InvalidArg;
        }
        if (count_ >= MaxChannels) return Status::Full;
        channels_[count_++] = ChannelState{.cfg = cfg, .tokens = cfg.max_tokens, .throttled = 0U};
        return Status::Ok;
    }

    /// Authorize and consume one rate-limit token on HypCall::Notify.
    [[nodiscard]] Expected<uint8_t, Status> try_notify(uint8_t caller_partition,
                                                       uint16_t channel_id) noexcept {
        for (size_t i = 0U; i < count_; ++i) {
            auto& ch = channels_[i];
            if (ch.cfg.channel_id != channel_id) continue;
            if (ch.cfg.src_partition != caller_partition) {
                return Unexpected(Status::PermissionDenied);
            }
            if (ch.tokens == 0U) {
                ++ch.throttled;
                return Unexpected(Status::Busy);
            }
            --ch.tokens;
            return ch.cfg.dst_partition;
        }
        return Unexpected(Status::InvalidArg);
    }

    /// Refill all doorbell token buckets on every minor frame boundary.
    void on_minor_frame_tick() noexcept {
        for (size_t i = 0U; i < count_; ++i) {
            auto& ch      = channels_[i];
            const auto nr = ch.tokens + ch.cfg.refill_per_minor_frame;
            ch.tokens     = (nr > ch.cfg.max_tokens) ? ch.cfg.max_tokens : nr;
        }
    }

    [[nodiscard]] uint32_t throttled_count(uint16_t channel_id) const noexcept {
        for (size_t i = 0U; i < count_; ++i) {
            if (channels_[i].cfg.channel_id == channel_id) return channels_[i].throttled;
        }
        return 0U;
    }

private:
    struct ChannelState {
        DoorbellChannelConfig cfg{};
        uint32_t              tokens{0U};
        uint32_t              throttled{0U};
    };

    std::array<ChannelState, MaxChannels> channels_{};
    size_t                                count_{0U};
};

/// ============================================================================
/// Bare-Metal Type-1 Hypervisor Core (STRATA-HYP-001..009)
/// ============================================================================
template <HypervisorArchBackend Backend, size_t MaxPartitions = 16U, size_t MaxChannels = 32U,
          size_t MaxRegions = 64U, size_t MaxTrapQueue = 16U>
class HypervisorCore {
public:
    struct PartitionState {
        PartitionConfig cfg{};
        PartitionMode   mode{PartitionMode::ColdStart};
        GenId           gen{0U};
        bool            registered{false};
        bool            halted{false};
        bool            waiting_event{false};
        bool            yielded_this_frame{false};
        uint32_t        slice_used_us{0U};
        uint32_t        overrun_count{0U};
        uint32_t        warm_restart_count{0U};
        uint32_t        cold_restart_count{0U};
    };

    explicit HypervisorCore(Backend& backend, uint32_t minor_frame_us, uint32_t major_frame_us,
                            uint8_t fdir_supervisor_part_id = 1U) noexcept
        : backend_(&backend),
          minor_frame_us_(minor_frame_us),
          major_frame_us_(major_frame_us),
          minor_frames_per_major_((minor_frame_us > 0U) ? (major_frame_us / minor_frame_us) : 1U),
          fdir_supervisor_id_(fdir_supervisor_part_id) {}

    [[nodiscard]] Status register_partition(const PartitionConfig& cfg) noexcept {
        if (cfg.partition_id >= MaxPartitions || cfg.budget_us == 0U ||
            cfg.period_minor_frames == 0U) {
            return Status::InvalidArg;
        }
        auto& p = partitions_[cfg.partition_id];
        if (p.registered) return Status::InvalidArg;

        // Enforce per-CPU minor-frame schedulability budget
        uint32_t cpu_sum = cfg.budget_us;
        for (const auto& other : partitions_) {
            if (other.registered && other.cfg.cpu_id == cfg.cpu_id) {
                cpu_sum += other.cfg.budget_us;
            }
        }
        if (cpu_sum > minor_frame_us_) {
            return Status::ValidationFailed;
        }

        p = PartitionState{
            .cfg        = cfg,
            .mode       = PartitionMode::ColdStart,
            .gen        = GenId{1U},
            .registered = true,
            .halted     = false,
        };
        return Status::Ok;
    }

    [[nodiscard]] Status map_region(const MemRegionDesc& desc) noexcept {
        const Status st = mem_table_.add_region(desc);
        if (st != Status::Ok) return st;
        return backend_->program_region(desc);
    }

    [[nodiscard]] Status register_doorbell(const DoorbellChannelConfig& cfg) noexcept {
        return doorbells_.register_channel(cfg);
    }

    /// Advance deterministic Minor/Major frame boundary (STRATA-HYP-002),
    /// refreshing time slices, refilling doorbell token buckets, and updating SharedTimePage.
    void begin_minor_frame(uint64_t tai_ns, uint64_t pps_count = 0U) noexcept {
        // Seqlock write to Read-Only SharedTimePage
        time_page_.seq = time_page_.seq + 1U;
        time_page_.tai_ns    = tai_ns;
        time_page_.pps_count = pps_count;
        time_page_.epoch     = major_frame_idx_;
        time_page_.seq       = time_page_.seq + 1U;

        doorbells_.on_minor_frame_tick();

        for (auto& p : partitions_) {
            if (!p.registered) continue;
            p.slice_used_us      = 0U;
            p.yielded_this_frame = false;
            if (!p.halted && (p.mode == PartitionMode::ColdStart || p.mode == PartitionMode::WarmStart)) {
                // Keep boot mode visible until first dispatch completes
            }
        }

        ++minor_frame_idx_;
        if (minor_frame_idx_ >= minor_frames_per_major_) {
            minor_frame_idx_ = 0U;
            ++major_frame_idx_;
        }
    }

    /// Charge execution time to a partition and enforce its hard minor-frame CPU time slice.
    [[nodiscard]] Status charge_cpu_slice(uint8_t partition_id, uint32_t elapsed_us) noexcept {
        if (partition_id >= MaxPartitions || !partitions_[partition_id].registered) {
            return Status::InvalidArg;
        }
        auto& p = partitions_[partition_id];
        if (p.halted || p.yielded_this_frame) return Status::PreconditionFailed;
        p.slice_used_us += elapsed_us;
        if (p.slice_used_us > p.cfg.budget_us) {
            ++p.overrun_count;
            p.yielded_this_frame = true; // Preempt immediately on budget expiry
            return Status::DeadlineMissed;
        }
        return Status::Ok;
    }

    /// Select the next runnable partition on `cpu_id` within the current minor frame.
    [[nodiscard]] Expected<uint8_t, Status> select_next_runnable(uint8_t cpu_id) noexcept {
        for (auto& p : partitions_) {
            if (!p.registered || p.cfg.cpu_id != cpu_id) continue;
            if (p.halted || p.waiting_event || p.yielded_this_frame) continue;
            if (p.slice_used_us >= p.cfg.budget_us) continue;
            const bool phase_match =
                ((minor_frame_idx_ % p.cfg.period_minor_frames) ==
                 (p.cfg.offset_minor_frames % p.cfg.period_minor_frames));
            if (!phase_match) continue;
            (void)backend_->activate_partition_context(p.cfg.partition_id);
            return p.cfg.partition_id;
        }
        return Unexpected(Status::Empty);
    }

    /// Dispatch any of the 8 canonical STRATA-20 hypercalls (`HypCall`).
    [[nodiscard]] Expected<uint64_t, Status>
    handle_hypcall(uint8_t caller_id, HypCall call, uint64_t arg0 = 0U, uint64_t arg1 = 0U,
                   uint64_t arg2 = 0U) noexcept {
        if (caller_id >= MaxPartitions || !partitions_[caller_id].registered) {
            return Unexpected(Status::InvalidArg);
        }
        auto& caller = partitions_[caller_id];

        switch (call) {
            case HypCall::Yield:
                caller.yielded_this_frame = true;
                return 0ULL;

            case HypCall::WaitEvent:
                caller.waiting_event = true;
                return 0ULL;

            case HypCall::Notify: {
                const auto dst = doorbells_.try_notify(caller_id, static_cast<uint16_t>(arg0));
                if (!dst.has_value()) return Unexpected(dst.error());
                const uint8_t dst_pid = dst.value();
                if (dst_pid < MaxPartitions && partitions_[dst_pid].registered) {
                    partitions_[dst_pid].waiting_event = false;
                }
                const Status st = backend_->send_ipi(dst_pid, static_cast<uint16_t>(arg0));
                if (st != Status::Ok) return Unexpected(st);
                return static_cast<uint64_t>(dst_pid);
            }

            case HypCall::TimeRead:
                return backend_->read_hw_tai_ns();

            case HypCall::FaultReport: {
                TrapContext trap{
                    .partition_id = caller_id,
                    .trap_kind    = 0U, // GuestFaultReport
                    .gen_id       = caller.gen.value,
                    .pc           = arg1,
                    .lr           = 0U,
                    .syndrome     = arg0,
                    .fault_addr   = arg2,
                    .tai_ns       = backend_->read_hw_tai_ns(),
                };
                on_hardware_trap(trap);
                return 0ULL;
            }

            case HypCall::SlotOp: {
                if (!caller.cfg.privileged_slot_ctl) {
                    return Unexpected(Status::PermissionDenied);
                }
                const Status st = backend_->execute_slot_op(
                    static_cast<uint16_t>(arg0), static_cast<SlotOpCode>(arg1), arg2);
                if (st != Status::Ok) return Unexpected(st);
                return 0ULL;
            }

            case HypCall::PartCtl: {
                if (!caller.cfg.privileged_part_ctl) {
                    return Unexpected(Status::PermissionDenied);
                }
                const auto target_id = static_cast<uint8_t>(arg0);
                const auto op        = static_cast<PartCtlOp>(arg1);
                const Status st      = execute_part_ctl(target_id, op);
                if (st != Status::Ok) return Unexpected(st);
                return static_cast<uint64_t>(partitions_[target_id].gen.value);
            }

            case HypCall::DiagTrace: {
                const Status st = backend_->write_flight_recorder(caller_id, arg0);
                if (st != Status::Ok) return Unexpected(st);
                return 0ULL;
            }
        }
        return Unexpected(Status::InvalidArg);
    }

    /// Capture a hardware exception/trap, halt the offending partition, enqueue the TrapContext,
    /// and wake the unprivileged Class B FDIR supervisor partition (STRATA-HYP-008).
    void on_hardware_trap(const TrapContext& trap) noexcept {
        if (trap.partition_id < MaxPartitions && partitions_[trap.partition_id].registered) {
            partitions_[trap.partition_id].halted = true;
        }
        if (trap_count_ < MaxTrapQueue) {
            const size_t idx = (trap_head_ + trap_count_) % MaxTrapQueue;
            trap_queue_[idx] = trap;
            ++trap_count_;
        }
        if (fdir_supervisor_id_ < MaxPartitions && partitions_[fdir_supervisor_id_].registered) {
            partitions_[fdir_supervisor_id_].waiting_event = false;
            (void)backend_->send_ipi(fdir_supervisor_id_, /*fdir_trap_channel=*/0xFFFFU);
        }
    }

    [[nodiscard]] Expected<TrapContext, Status> pop_trap() noexcept {
        if (trap_count_ == 0U) return Unexpected(Status::Empty);
        const TrapContext t = trap_queue_[trap_head_];
        trap_head_          = (trap_head_ + 1U) % MaxTrapQueue;
        --trap_count_;
        return t;
    }

    [[nodiscard]] const SharedTimePage& shared_time_page() const noexcept { return time_page_; }
    [[nodiscard]] const MemoryIsolationTable<MaxRegions>& memory_table() const noexcept {
        return mem_table_;
    }
    [[nodiscard]] const DoorbellRouter<MaxChannels>& doorbells() const noexcept {
        return doorbells_;
    }
    [[nodiscard]] const PartitionState& partition(uint8_t pid) const noexcept {
        return partitions_[pid];
    }
    [[nodiscard]] uint32_t minor_frame_index() const noexcept { return minor_frame_idx_; }
    [[nodiscard]] uint32_t major_frame_index() const noexcept { return major_frame_idx_; }

private:
    /// Implement WarmRestart (preserves CriticalStore, scrubs stack/.bss, increments GenId)
    /// and ColdRestart (scrubs CriticalStore + stack/.bss, increments GenId) (STRATA-HYP-006).
    [[nodiscard]] Status execute_part_ctl(uint8_t target_id, PartCtlOp op) noexcept {
        if (target_id >= MaxPartitions || !partitions_[target_id].registered) {
            return Status::InvalidArg;
        }
        auto& target = partitions_[target_id];
        switch (op) {
            case PartCtlOp::Halt:
                target.halted = true;
                return Status::Ok;

            case PartCtlOp::SetIdle:
                target.mode   = PartitionMode::Idle;
                target.halted = false;
                return Status::Ok;

            case PartCtlOp::WarmRestart: {
                target.halted = true;
                const Status st = backend_->scrub_partition_ram(target_id, /*preserve_critical_store=*/true);
                if (st != Status::Ok) return st;
                target.gen           = target.gen.next();
                target.mode          = PartitionMode::WarmStart;
                target.halted        = false;
                target.waiting_event = false;
                target.slice_used_us = 0U;
                ++target.warm_restart_count;
                return Status::Ok;
            }

            case PartCtlOp::ColdRestart: {
                target.halted = true;
                const Status st = backend_->scrub_partition_ram(target_id, /*preserve_critical_store=*/false);
                if (st != Status::Ok) return st;
                target.gen           = target.gen.next();
                target.mode          = PartitionMode::ColdStart;
                target.halted        = false;
                target.waiting_event = false;
                target.slice_used_us = 0U;
                ++target.cold_restart_count;
                return Status::Ok;
            }
        }
        return Status::InvalidArg;
    }

    Backend*                           backend_;
    uint32_t                           minor_frame_us_{10'000U};
    uint32_t                           major_frame_us_{1'000'000U};
    uint32_t                           minor_frames_per_major_{100U};
    uint32_t                           minor_frame_idx_{0U};
    uint32_t                           major_frame_idx_{0U};
    uint8_t                            fdir_supervisor_id_{1U};
    SharedTimePage                     time_page_{};
    std::array<PartitionState, MaxPartitions> partitions_{};
    MemoryIsolationTable<MaxRegions>   mem_table_{};
    DoorbellRouter<MaxChannels>        doorbells_{};
    std::array<TrapContext, MaxTrapQueue> trap_queue_{};
    size_t                             trap_head_{0U};
    size_t                             trap_count_{0U};
};

} // namespace strata::hyp
