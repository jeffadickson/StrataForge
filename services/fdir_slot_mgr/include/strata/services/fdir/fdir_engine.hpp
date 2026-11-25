#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace strata::services::fdir {

/// Watchpoint comparison operators (cFS LC + ESA PUS ST[12]).
enum class CompareOp : uint8_t {
    GreaterThan = 0U,
    LessThan    = 1U,
    EqualTo     = 2U,
    NotEqualTo  = 3U,
    OutOfRange  = 4U, // value < low_limit || value > high_limit
};

/// ARINC 653 Multi-Level Recovery Escalation Ladder + DLR ScOSA Failover.
enum class RecoveryLevel : uint8_t {
    LocalWarn              = 0U, // Emit WARNING_HI, no restart
    WarmRestartPartition   = 1U, // hyp_part_ctl(WARM_RESTART) + CriticalStore<T> restore
    QuiesceAndReloadSlot   = 2U, // SCSR.QUIESCE -> NoC INI / Decoupler -> partial PDI reload
    ScosaFailoverSpareSlot = 3U, // Migrate personality to pre-verified spare DFX slot
    SystemSafeMode         = 4U, // Quiesce payload slots & enter minimal beacon mode
};

/// Declarative Watchpoint entry (uploadable via ParamBank<T> without recompiling FSW!).
struct WatchpointDef {
    uint16_t  wp_id{0U};
    uint16_t  channel_id{0U};
    CompareOp op{CompareOp::GreaterThan};
    int64_t   low_limit{0};
    int64_t   high_limit{0};
    uint16_t  debounce_ticks{1U}; // Must persist for N consecutive samples to trip
    bool      enabled{false};
};

/// Declarative Actionpoint rule (cFS LC AP + ESA PUS ST[19] Event-Action).
struct ActionpointDef {
    uint16_t      ap_id{0U};
    uint32_t      required_wp_mask{0U}; // Bitmask of watchpoint indices (0..31)
    bool          require_all{true};    // true = AND (all bits), false = OR (any bit)
    RecoveryLevel level{RecoveryLevel::LocalWarn};
    uint8_t       target_partition{0U};
    uint16_t      target_slot_id{0U};
    uint8_t       max_slot_reloads_before_failover{2U};
    bool          enabled{false};
};

/// Versal PMC XilSEM / Kintex Rad-Hard Scrubber CRAM Frame-to-Slot coordinate mapping.
struct CramFrameRange {
    uint32_t frame_addr_start{0U};
    uint32_t frame_addr_end{0U}; // Inclusive
    uint16_t slot_id{0U};
    uint8_t  owner_partition{0U};
    bool     enabled{false};
};

/// DLR ScOSA Offline Reconfiguration Failover Entry (pre-verified deterministic spare mapping).
struct ScosaFailoverEntry {
    uint16_t primary_slot_id{0U};
    uint16_t spare_slot_id{0U};
    uint16_t personality_id{0U};
    uint8_t  standby_partition{0U};
    bool     enabled{false};
};

/// Concrete recovery directive emitted by FdirEngine for execution by Hypervisor / SlotManager.
struct RecoveryDirective {
    RecoveryLevel level{RecoveryLevel::LocalWarn};
    uint16_t      source_id{0U}; // Actionpoint ID or CRAM slot ID
    uint8_t       target_partition{0U};
    uint16_t      primary_slot_id{0U};
    uint16_t      failover_slot_id{0U};
    uint16_t      personality_id{0U};
};

/// Unified Class B Autonomous FDIR & Reconfiguration Engine.
/// Fuses cFS LC, ESA PUS ST[12]/ST[19], ARINC 653 HM, Versal XilSEM / Kintex Scrubber, and DLR ScOSA.
template <size_t MaxWps = 32U, size_t MaxAps = 16U, size_t MaxSlots = 8U>
    requires(MaxWps <= 32U)
class FdirEngine {
public:
    [[nodiscard]] Status add_watchpoint(const WatchpointDef& def) noexcept {
        for (size_t i = 0U; i < MaxWps; ++i) {
            if (!wps_[i].def.enabled) {
                wps_[i].def         = def;
                wps_[i].def.enabled = true;
                wps_[i].consec_hits = 0U;
                wps_[i].tripped     = false;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] Status add_actionpoint(const ActionpointDef& def) noexcept {
        for (auto& ap : aps_) {
            if (!ap.def.enabled) {
                ap.def         = def;
                ap.def.enabled = true;
                ap.fired_count = 0U;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] Status register_cram_range(const CramFrameRange& range) noexcept {
        for (auto& r : cram_map_) {
            if (!r.enabled) {
                r         = range;
                r.enabled = true;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] Status register_scosa_failover(const ScosaFailoverEntry& entry) noexcept {
        for (auto& f : failovers_) {
            if (!f.enabled) {
                f         = entry;
                f.enabled = true;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Feed one telemetry sample for channel_id; updates debounce and tripped WP bitmask.
    void evaluate_sample(uint16_t channel_id, int64_t value) noexcept {
        for (size_t i = 0U; i < MaxWps; ++i) {
            auto& wp = wps_[i];
            if (!wp.def.enabled || wp.def.channel_id != channel_id) {
                continue;
            }
            if (check_violation(wp.def, value)) {
                if (wp.consec_hits < wp.def.debounce_ticks) {
                    ++wp.consec_hits;
                }
                if (wp.consec_hits >= wp.def.debounce_ticks) {
                    wp.tripped = true;
                    tripped_wp_mask_ |= (1U << i);
                }
            } else {
                wp.consec_hits = 0U;
                wp.tripped     = false;
                tripped_wp_mask_ &= ~(1U << i);
            }
        }
    }

    /// Evaluate all Actionpoints and emit triggered RecoveryDirectives to callback(dir).
    template <typename ActionFn>
    uint32_t evaluate_actionpoints(ActionFn&& callback) noexcept {
        uint32_t triggered = 0U;
        for (auto& ap : aps_) {
            if (!ap.def.enabled || ap.def.required_wp_mask == 0U) {
                continue;
            }
            const bool match = ap.def.require_all
                                   ? ((tripped_wp_mask_ & ap.def.required_wp_mask) ==
                                      ap.def.required_wp_mask)
                                   : ((tripped_wp_mask_ & ap.def.required_wp_mask) != 0U);
            if (!match) {
                continue;
            }
            ++ap.fired_count;
            ++triggered;

            RecoveryDirective dir{
                .level            = ap.def.level,
                .source_id        = ap.def.ap_id,
                .target_partition = ap.def.target_partition,
                .primary_slot_id  = ap.def.target_slot_id,
            };

            // Automatic escalation: if slot reloads exceed threshold, promote to DLR ScOSA failover!
            if (dir.level == RecoveryLevel::QuiesceAndReloadSlot &&
                dir.primary_slot_id < MaxSlots) {
                ++slot_reload_counts_[dir.primary_slot_id];
                if (slot_reload_counts_[dir.primary_slot_id] >
                    ap.def.max_slot_reloads_before_failover) {
                    if (const auto* fo = find_failover(dir.primary_slot_id)) {
                        dir.level            = RecoveryLevel::ScosaFailoverSpareSlot;
                        dir.failover_slot_id = fo->spare_slot_id;
                        dir.personality_id   = fo->personality_id;
                        dir.target_partition = fo->standby_partition;
                    }
                }
            }

            callback(dir);
        }
        return triggered;
    }

    /// Correlate a Versal PMC XilSEM or Kintex Scrubber uncorrectable CRAM frame address
    /// directly to its physical slot and compute surgical single-slot recovery (or ScOSA failover).
    [[nodiscard]] RecoveryDirective
    handle_uncorrectable_cram_upset(uint32_t frame_addr, uint8_t max_reloads = 2U) noexcept {
        for (const auto& r : cram_map_) {
            if (!r.enabled) continue;
            if (frame_addr >= r.frame_addr_start && frame_addr <= r.frame_addr_end) {
                const uint16_t slot = r.slot_id;
                if (slot < MaxSlots) {
                    ++slot_reload_counts_[slot];
                    if (slot_reload_counts_[slot] > max_reloads) {
                        if (const auto* fo = find_failover(slot)) {
                            return RecoveryDirective{
                                .level            = RecoveryLevel::ScosaFailoverSpareSlot,
                                .source_id        = slot,
                                .target_partition = fo->standby_partition,
                                .primary_slot_id  = slot,
                                .failover_slot_id = fo->spare_slot_id,
                                .personality_id   = fo->personality_id,
                            };
                        }
                    }
                }
                return RecoveryDirective{
                    .level            = RecoveryLevel::QuiesceAndReloadSlot,
                    .source_id        = slot,
                    .target_partition = r.owner_partition,
                    .primary_slot_id  = slot,
                };
            }
        }
        // Uncorrectable upset in static Class B shell outside dynamic slots -> System Safe Mode!
        return RecoveryDirective{.level = RecoveryLevel::SystemSafeMode};
    }

    [[nodiscard]] uint32_t tripped_watchpoint_mask() const noexcept { return tripped_wp_mask_; }

private:
    struct WpRuntime {
        WatchpointDef def{};
        uint16_t      consec_hits{0U};
        bool          tripped{false};
    };

    struct ApRuntime {
        ActionpointDef def{};
        uint32_t       fired_count{0U};
    };

    [[nodiscard]] static bool check_violation(const WatchpointDef& d, int64_t val) noexcept {
        switch (d.op) {
            case CompareOp::GreaterThan: return val > d.high_limit;
            case CompareOp::LessThan:    return val < d.low_limit;
            case CompareOp::EqualTo:     return val == d.low_limit;
            case CompareOp::NotEqualTo:  return val != d.low_limit;
            case CompareOp::OutOfRange:  return val < d.low_limit || val > d.high_limit;
        }
        return false;
    }

    [[nodiscard]] const ScosaFailoverEntry* find_failover(uint16_t primary_slot) const noexcept {
        for (const auto& f : failovers_) {
            if (f.enabled && f.primary_slot_id == primary_slot) return &f;
        }
        return nullptr;
    }

    std::array<WpRuntime, MaxWps>            wps_{};
    std::array<ApRuntime, MaxAps>            aps_{};
    std::array<CramFrameRange, MaxSlots>     cram_map_{};
    std::array<ScosaFailoverEntry, MaxSlots> failovers_{};
    std::array<uint8_t, MaxSlots>            slot_reload_counts_{};
    uint32_t                                 tripped_wp_mask_{0U};
};

} // namespace strata::services::fdir
