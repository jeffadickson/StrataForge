#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace strata::services::cdh {

enum class SeqTimeMode : uint8_t {
    Immediate = 0U,
    Relative  = 1U, // Execute after offset_ns from previous step
    Absolute  = 2U, // Execute once TAI >= time_ns
};

/// Compact binary sequence step (CRC-protected sequence image).
struct SeqStep {
    SeqTimeMode time_mode{SeqTimeMode::Immediate};
    uint8_t     target_partition{0U};
    uint16_t    opcode{0U};
    uint32_t    arg0{0U};
    uint64_t    time_or_offset_ns{0U};
};

/// Multi-slot time-tagged Command Sequencer with CRC-32C validation and safe-mode abort.
template <size_t MaxSlots = 4U, size_t MaxStepsPerSeq = 32U>
class SeqEngine {
public:
    /// Load and CRC-32C-verify a binary sequence into slot_idx.
    [[nodiscard]] Status load(size_t slot_idx, std::span<const SeqStep> steps,
                              uint32_t expected_crc, TaiTimestamp now) noexcept {
        if (slot_idx >= MaxSlots || steps.empty() || steps.size() > MaxStepsPerSeq) {
            return Status::InvalidArg;
        }
        const uint32_t computed = Crc32c::compute(std::as_bytes(steps));
        if (computed != expected_crc) {
            return Status::CrcMismatch;
        }
        auto& s     = slots_[slot_idx];
        s.active    = true;
        s.step_cnt  = steps.size();
        s.cursor    = 0U;
        s.anchor_ns = now.ns();
        for (size_t i = 0U; i < steps.size(); ++i) {
            s.steps[i] = steps[i];
        }
        return Status::Ok;
    }

    /// Tick all active concurrent sequence slots and invoke dispatch_fn(step) when due.
    template <typename DispatchFn>
    uint32_t tick(TaiTimestamp now, DispatchFn&& dispatch_fn) noexcept {
        uint32_t executed = 0U;
        for (auto& s : slots_) {
            if (!s.active || s.cursor >= s.step_cnt) {
                continue;
            }
            const auto& step = s.steps[s.cursor];
            bool        due  = false;
            switch (step.time_mode) {
                case SeqTimeMode::Immediate:
                    due = true;
                    break;
                case SeqTimeMode::Relative:
                    due = (now.ns() >= s.anchor_ns + step.time_or_offset_ns);
                    break;
                case SeqTimeMode::Absolute:
                    due = (now.ns() >= step.time_or_offset_ns);
                    break;
            }
            if (due) {
                const CmdResult res = dispatch_fn(step);
                ++executed;
                if (res != CmdResult::Ok && res != CmdResult::InProgress) {
                    // Abort sequence cleanly on command failure
                    s.active = false;
                    ++abort_count_;
                } else {
                    ++s.cursor;
                    s.anchor_ns = now.ns();
                    if (s.cursor >= s.step_cnt) {
                        s.active = false;
                        ++completed_count_;
                    }
                }
            }
        }
        return executed;
    }

    /// Immediate FDIR safe-mode kill switch: aborts all running sequences.
    void abort_all() noexcept {
        for (auto& s : slots_) {
            if (s.active) {
                s.active = false;
                ++abort_count_;
            }
        }
    }

    [[nodiscard]] bool is_slot_active(size_t slot_idx) const noexcept {
        return slot_idx < MaxSlots && slots_[slot_idx].active;
    }

    [[nodiscard]] uint32_t completed_count() const noexcept { return completed_count_; }
    [[nodiscard]] uint32_t abort_count() const noexcept { return abort_count_; }

private:
    struct Slot {
        bool                                 active{false};
        size_t                               step_cnt{0U};
        size_t                               cursor{0U};
        uint64_t                             anchor_ns{0U};
        std::array<SeqStep, MaxStepsPerSeq>  steps{};
    };

    std::array<Slot, MaxSlots> slots_{};
    uint32_t                   completed_count_{0U};
    uint32_t                   abort_count_{0U};
};

} // namespace strata::services::cdh
