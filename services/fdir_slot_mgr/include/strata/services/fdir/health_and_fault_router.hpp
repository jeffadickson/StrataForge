#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/hyp/abi.hpp>
#include <strata/hw/slot_handle.hpp>
#include <strata/ipc/blackboard.hpp>
#include <strata/runtime/subtopology.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace strata::services::fdir {

/// ============================================================================
/// Dual-Layer HealthMonitor (STRATA-FDIR-003)
/// Monitors monotonic per-partition Blackboard heartbeats + CPU overrun flags
/// in software AND verifies every active PL slot services its hardware SCSR
/// watchdog within its configured timeout window.
/// ============================================================================
enum class HealthIssue : uint8_t {
    None                 = 0U,
    PartitionMissedBeat  = 1U,
    PartitionCpuOverrun  = 2U,
    SlotWatchdogTimeout  = 3U,
    SlotHardwareFault    = 4U,
};

struct HealthFinding {
    HealthIssue issue{HealthIssue::None};
    uint16_t    target_id{0U}; // partition_id or slot_id
    uint32_t    detail{0U};
};

template <size_t MaxPartitions = 16U, size_t MaxSlots = 16U>
class HealthMonitor {
public:
    struct PartitionWatch {
        uint8_t                                               partition_id{0U};
        const BlackboardStorage<runtime::PartitionHeartbeat>* hb_bb{nullptr};
        DurationNs                                            timeout{DurationNs{50'000'000ULL}};
        uint32_t                                              max_overruns{3U};
        uint32_t                                              last_beat{0U};
        TaiTimestamp                                          last_change_tai{};
        bool                                                  initialized{false};
        bool                                                  active{false};
    };

    struct SlotWatch {
        uint16_t     slot_id{0U};
        DurationNs   wdt_window{DurationNs{20'000'000ULL}};
        TaiTimestamp last_kick_tai{};
        bool         active{false};
    };

    [[nodiscard]] Status register_partition(uint8_t partition_id,
                                            const BlackboardStorage<runtime::PartitionHeartbeat>& bb,
                                            DurationNs timeout,
                                            uint32_t   max_overruns = 3U) noexcept {
        for (auto& w : partitions_) {
            if (!w.active) {
                w = PartitionWatch{
                    .partition_id    = partition_id,
                    .hb_bb           = &bb,
                    .timeout         = timeout,
                    .max_overruns    = max_overruns,
                    .last_beat       = 0U,
                    .last_change_tai = TaiTimestamp{0U},
                    .initialized     = false,
                    .active          = true,
                };
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] Status register_slot(uint16_t slot_id, DurationNs wdt_window,
                                       TaiTimestamp now) noexcept {
        for (auto& s : slots_) {
            if (!s.active) {
                s = SlotWatch{
                    .slot_id       = slot_id,
                    .wdt_window    = wdt_window,
                    .last_kick_tai = now,
                    .active        = true,
                };
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Record a software-observed SCSR.WDT_RELOAD kick on an active PL slot.
    void notify_slot_wdt_kick(uint16_t slot_id, TaiTimestamp now) noexcept {
        for (auto& s : slots_) {
            if (s.active && s.slot_id == slot_id) {
                s.last_kick_tai = now;
                return;
            }
        }
    }

    /// Poll all partitions and active hardware slots, appending findings to `out`.
    template <typename SlotPollFn>
    [[nodiscard]] size_t evaluate(TaiTimestamp now, SlotPollFn&& poll_slot_fault_and_state,
                                  std::span<HealthFinding> out) noexcept {
        size_t count = 0U;

        // 1. Software partition heartbeat & CPU overrun layer
        for (auto& w : partitions_) {
            if (!w.active || w.hb_bb == nullptr) continue;
            BlackboardReader<runtime::PartitionHeartbeat> reader(*w.hb_bb);
            const auto snap = reader.read(now);
            if (!snap.has_value()) {
                if (!w.initialized) {
                    w.last_change_tai = now;
                    w.initialized     = true;
                } else if (elapsed_since(now, w.last_change_tai) > w.timeout && count < out.size()) {
                    out[count++] = HealthFinding{HealthIssue::PartitionMissedBeat, w.partition_id, 0U};
                }
                continue;
            }
            const auto& hb = snap.value().value;
            if (!w.initialized || hb.beat_count != w.last_beat) {
                w.last_beat       = hb.beat_count;
                w.last_change_tai = now;
                w.initialized     = true;
            } else if (elapsed_since(now, w.last_change_tai) > w.timeout && count < out.size()) {
                out[count++] = HealthFinding{HealthIssue::PartitionMissedBeat, w.partition_id, hb.beat_count};
            }

            if (hb.overrun_count > w.max_overruns && count < out.size()) {
                out[count++] =
                    HealthFinding{HealthIssue::PartitionCpuOverrun, w.partition_id, hb.overrun_count};
            }
        }

        // 2. Hardware PL slot watchdog & SCSR fault layer
        for (const auto& s : slots_) {
            if (!s.active) continue;
            const auto [slot_state, fault_bits] = poll_slot_fault_and_state(s.slot_id);
            if ((fault_bits & hw::scsr::kFaultWdtExp) != 0U ||
                (slot_state == SlotState::Active && elapsed_since(now, s.last_kick_tai) > s.wdt_window)) {
                if (count < out.size()) {
                    out[count++] = HealthFinding{HealthIssue::SlotWatchdogTimeout, s.slot_id, fault_bits};
                }
            } else if (fault_bits != 0U && count < out.size()) {
                out[count++] = HealthFinding{HealthIssue::SlotHardwareFault, s.slot_id, fault_bits};
            }
        }

        return count;
    }

private:
    std::array<PartitionWatch, MaxPartitions> partitions_{};
    std::array<SlotWatch, MaxSlots>           slots_{};
};

/// ============================================================================
/// Isolated FaultRouter (STRATA-FDIR-004)
/// Receives hypervisor hardware trap dumps and guest FaultReport notifications,
/// logs compact crash records, triggers isolated Warm/Cold restarts for Class C/D,
/// and reserves spacecraft SafeMode escalation solely for unrecoverable Class B TCB faults.
/// ============================================================================
enum class TrapKind : uint8_t {
    GuestFaultReport = 0U,
    DataAbort        = 1U,
    InstructionAbort = 2U,
    AlignmentFault   = 3U,
    UndefinedInstr   = 4U,
    PrivilegeFault   = 5U,
};

struct CrashRecord {
    uint8_t     partition_id{0U};
    Criticality criticality{Criticality::ClassD};
    TrapKind    kind{TrapKind::GuestFaultReport};
    uint16_t    gen_id{0U};
    uint64_t    pc{0U};
    uint64_t    lr{0U};
    uint64_t    syndrome{0U};
    uint64_t    fault_addr{0U};
    uint64_t    tai_ns{0U};
};

enum class FaultRoutingDecision : uint8_t {
    WarmRestartPartition   = 0U,
    ColdRestartPartition   = 1U,
    HaltPartition          = 2U,
    EscalateSpacecraftSafe = 3U,
};

template <size_t MaxCrashRecords = 32U, size_t MaxPartitions = 16U>
class FaultRouter {
public:
    explicit FaultRouter(uint8_t warm_restart_limit = 2U, uint8_t cold_restart_limit = 1U) noexcept
        : warm_limit_(warm_restart_limit), cold_limit_(cold_restart_limit) {}

    /// Route an incoming hardware trap or guest FaultReport, logging the crash record and
    /// invoking `part_ctl(partition_id, hyp::PartCtlOp)` for isolated recovery.
    template <typename PartCtlFn>
    [[nodiscard]] FaultRoutingDecision handle_trap(const CrashRecord& rec,
                                                   PartCtlFn&&        part_ctl) noexcept {
        // Record compact crash dump into circular ring
        crash_ring_[ring_head_ % MaxCrashRecords] = rec;
        ++ring_head_;
        if (ring_count_ < MaxCrashRecords) ++ring_count_;

        uint8_t& warm_cnt = (rec.partition_id < MaxPartitions) ? warm_counts_[rec.partition_id] : scratch_;
        uint8_t& cold_cnt = (rec.partition_id < MaxPartitions) ? cold_counts_[rec.partition_id] : scratch_;

        // First attempt isolated WarmRestart (preserves CriticalStore<T>)
        if (warm_cnt < warm_limit_) {
            ++warm_cnt;
            part_ctl(rec.partition_id, hyp::PartCtlOp::WarmRestart);
            return FaultRoutingDecision::WarmRestartPartition;
        }

        // Next attempt isolated ColdRestart (scrubs CriticalStore<T>)
        if (cold_cnt < cold_limit_) {
            ++cold_cnt;
            part_ctl(rec.partition_id, hyp::PartCtlOp::ColdRestart);
            return FaultRoutingDecision::ColdRestartPartition;
        }

        // Exhausted local restarts:
        //  - Class C/D partitions are halted in isolation (never escalate spacecraft safe mode!)
        //  - Only unrecoverable Class B TCB faults escalate spacecraft safe mode.
        if (rec.criticality == Criticality::ClassB) {
            ++safe_mode_escalations_;
            part_ctl(rec.partition_id, hyp::PartCtlOp::Halt);
            return FaultRoutingDecision::EscalateSpacecraftSafe;
        }

        part_ctl(rec.partition_id, hyp::PartCtlOp::Halt);
        return FaultRoutingDecision::HaltPartition;
    }

    void reset_restart_counters(uint8_t partition_id) noexcept {
        if (partition_id < MaxPartitions) {
            warm_counts_[partition_id] = 0U;
            cold_counts_[partition_id] = 0U;
        }
    }

    [[nodiscard]] size_t   crash_count() const noexcept { return ring_count_; }
    [[nodiscard]] uint32_t safe_mode_escalations() const noexcept { return safe_mode_escalations_; }
    [[nodiscard]] const CrashRecord& crash_record(size_t idx) const noexcept {
        return crash_ring_[idx % MaxCrashRecords];
    }

private:
    std::array<CrashRecord, MaxCrashRecords> crash_ring_{};
    std::array<uint8_t, MaxPartitions>       warm_counts_{};
    std::array<uint8_t, MaxPartitions>       cold_counts_{};
    size_t                                   ring_head_{0U};
    size_t                                   ring_count_{0U};
    uint8_t                                  warm_limit_{2U};
    uint8_t                                  cold_limit_{1U};
    uint8_t                                  scratch_{0U};
    uint32_t                                 safe_mode_escalations_{0U};
};

} // namespace strata::services::fdir
