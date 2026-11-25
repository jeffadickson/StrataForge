#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace strata::hw {

/// ============================================================================
/// Deterministic Flight Shadow Recorder & Offline Replay Transport (STRATA-VER-003)
/// Wraps any RegisterTransport to capture every MMIO read/write transaction into
/// a fixed zero-heap circular ring, then replays captured reads bit-for-bit
/// offline for anomaly reproduction without hardware present.
/// ============================================================================
struct ShadowMmioEvent {
    uint32_t offset{0U};
    uint32_t value{0U};
    bool     is_write{false};
};

template <RegisterTransport Underlying, size_t Capacity = 256U>
class ShadowRecordingTransport {
public:
    explicit ShadowRecordingTransport(Underlying& bus) noexcept : bus_(&bus) {}

    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept {
        const uint32_t val = bus_->read32(offset);
        record(ShadowMmioEvent{offset, val, false});
        return val;
    }

    void write32(uint32_t offset, uint32_t value) noexcept {
        bus_->write32(offset, value);
        record(ShadowMmioEvent{offset, value, true});
    }

    [[nodiscard]] std::span<const ShadowMmioEvent> trace() const noexcept {
        return std::span<const ShadowMmioEvent>{log_.data(), count_};
    }

    void clear_trace() noexcept { count_ = 0U; }

private:
    void record(ShadowMmioEvent ev) const noexcept {
        if (count_ < Capacity) {
            log_[count_++] = ev;
        }
    }

    Underlying*                                   bus_;
    mutable std::array<ShadowMmioEvent, Capacity> log_{};
    mutable size_t                                count_{0U};
};

/// Offline bit-exact replay transport verifying flight writes and returning recorded reads.
class ShadowReplayTransport {
public:
    explicit ShadowReplayTransport(std::span<const ShadowMmioEvent> trace) noexcept
        : trace_(trace) {}

    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept {
        if (cursor_ >= trace_.size()) {
            ++mismatch_count_;
            return 0U;
        }
        const auto& ev = trace_[cursor_++];
        if (ev.is_write || ev.offset != offset) {
            ++mismatch_count_;
        }
        return ev.value;
    }

    void write32(uint32_t offset, uint32_t value) noexcept {
        if (cursor_ >= trace_.size()) {
            ++mismatch_count_;
            return;
        }
        const auto& ev = trace_[cursor_++];
        if (!ev.is_write || ev.offset != offset || ev.value != value) {
            ++mismatch_count_;
        }
    }

    [[nodiscard]] bool     completed_cleanly() const noexcept {
        return (cursor_ == trace_.size()) && (mismatch_count_ == 0U);
    }
    [[nodiscard]] uint32_t mismatch_count() const noexcept { return mismatch_count_; }
    [[nodiscard]] size_t   cursor() const noexcept { return cursor_; }

private:
    std::span<const ShadowMmioEvent> trace_{};
    mutable size_t                   cursor_{0U};
    mutable uint32_t                 mismatch_count_{0U};
};

static_assert(RegisterTransport<ShadowReplayTransport>);

} // namespace strata::hw
