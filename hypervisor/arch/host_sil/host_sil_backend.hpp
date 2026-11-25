#pragma once

#include <strata/hyp/hypervisor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace strata::hyp::arch {

/// ============================================================================
/// Deterministic Host SIL Hypervisor Backend (STRATA-HYP-009)
/// Simulates per-partition RAM (.bss/stack + CriticalStore), hardware IPIs,
/// privileged SlotOps, TAI clock, and protected flight recorder ring on host.
/// ============================================================================
template <size_t MaxPartitions = 16U, size_t RamPerPartition = 256U,
          size_t CriticalStorePerPartition = 128U, size_t RecorderCap = 64U>
class HostSilBackend {
public:
    struct PartitionMemory {
        std::array<std::byte, RamPerPartition>           bss_and_stack{};
        std::array<std::byte, CriticalStorePerPartition> critical_store{};
    };

    struct DiagEntry {
        uint8_t  partition_id{0U};
        uint64_t word{0U};
    };

    [[nodiscard]] Status program_region(const MemRegionDesc& reg) noexcept {
        (void)reg;
        ++programmed_regions_;
        return Status::Ok;
    }

    [[nodiscard]] Status activate_partition_context(uint8_t pid) noexcept {
        active_pid_ = pid;
        return Status::Ok;
    }

    [[nodiscard]] Status send_ipi(uint8_t pid, uint16_t channel_id) noexcept {
        last_ipi_pid_     = pid;
        last_ipi_channel_ = channel_id;
        ++ipi_count_;
        return Status::Ok;
    }

    /// Zero stack/.bss on WarmRestart while preserving CriticalStore; zero both on ColdRestart.
    [[nodiscard]] Status scrub_partition_ram(uint8_t pid, bool preserve_critical_store) noexcept {
        if (pid >= MaxPartitions) return Status::InvalidArg;
        ram_[pid].bss_and_stack.fill(std::byte{0});
        if (!preserve_critical_store) {
            ram_[pid].critical_store.fill(std::byte{0});
        }
        return Status::Ok;
    }

    [[nodiscard]] Status execute_slot_op(uint16_t slot_id, SlotOpCode op, uint64_t arg) noexcept {
        last_slot_id_  = slot_id;
        last_slot_op_  = op;
        last_slot_arg_ = arg;
        ++slot_op_count_;
        return Status::Ok;
    }

    [[nodiscard]] Status write_flight_recorder(uint8_t pid, uint64_t diag_word) noexcept {
        recorder_[recorder_head_ % RecorderCap] = DiagEntry{pid, diag_word};
        ++recorder_head_;
        return Status::Ok;
    }

    [[nodiscard]] uint64_t read_hw_tai_ns() noexcept { return sim_tai_ns_; }

    void set_sim_tai_ns(uint64_t ns) noexcept { sim_tai_ns_ = ns; }
    [[nodiscard]] PartitionMemory& partition_ram(uint8_t pid) noexcept { return ram_[pid]; }
    [[nodiscard]] uint8_t  active_partition() const noexcept { return active_pid_; }
    [[nodiscard]] uint32_t ipi_count() const noexcept { return ipi_count_; }
    [[nodiscard]] uint32_t slot_op_count() const noexcept { return slot_op_count_; }
    [[nodiscard]] uint32_t programmed_regions() const noexcept { return programmed_regions_; }
    [[nodiscard]] size_t   recorder_writes() const noexcept { return recorder_head_; }
    [[nodiscard]] DiagEntry recorder_entry(size_t i) const noexcept {
        return recorder_[i % RecorderCap];
    }

private:
    std::array<PartitionMemory, MaxPartitions> ram_{};
    std::array<DiagEntry, RecorderCap>         recorder_{};
    size_t                                     recorder_head_{0U};
    uint64_t                                   sim_tai_ns_{0U};
    uint8_t                                    active_pid_{0xFFU};
    uint8_t                                    last_ipi_pid_{0xFFU};
    uint16_t                                   last_ipi_channel_{0U};
    uint32_t                                   ipi_count_{0U};
    uint16_t                                   last_slot_id_{0U};
    SlotOpCode                                 last_slot_op_{SlotOpCode::Quiesce};
    uint64_t                                   last_slot_arg_{0U};
    uint32_t                                   slot_op_count_{0U};
    uint32_t                                   programmed_regions_{0U};
};

static_assert(HypervisorArchBackend<HostSilBackend<>>);

} // namespace strata::hyp::arch
