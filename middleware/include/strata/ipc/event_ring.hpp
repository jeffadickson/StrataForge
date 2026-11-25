#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/spsc_channel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace strata {

/// Compact fixed-size binary flight event record (eliminates flight-side string formatting).
struct EventRecord {
    uint64_t      first_tai_ns{0U};
    uint64_t      last_tai_ns{0U};
    uint32_t      event_id{0U};
    uint32_t      arg0{0U};
    uint32_t      arg1{0U};
    uint16_t      repeat_count{1U};
    EventSeverity severity{EventSeverity::ActivityHi};
    uint8_t       partition_id{0U};
};

static_assert(WirePod<EventRecord>, "EventRecord must be WirePod");

/// Per-partition bounded EventRing with severity filtering, burst deduplication, and cooldown.
/// Prevents central telemetry/event queue overflows during anomaly floods.
template <size_t Capacity, size_t MaxTrackedIds = 16U>
    requires PowerOfTwo<Capacity>
class EventWriter {
public:
    explicit EventWriter(SpscRingStorage<EventRecord, Capacity>& storage,
                         uint8_t partition_id,
                         DurationNs cooldown = DurationNs{0U}) noexcept
        : producer_(storage, 0U, NullNotifier{}, GenId{0U}),
          partition_id_(partition_id),
          cooldown_(cooldown) {}

    /// Enable or disable specific severity levels at runtime via bitmask.
    void set_severity_mask(uint8_t mask) noexcept { severity_mask_ = mask; }

    [[nodiscard]] bool is_severity_enabled(EventSeverity sev) const noexcept {
        return (severity_mask_ & static_cast<uint8_t>(1U << static_cast<uint8_t>(sev))) != 0U;
    }

    /// Emit a binary event with automatic burst suppression / deduplication.
    /// Fatal events bypass cooldown suppression and severity filtering.
    [[nodiscard]] Status emit(uint32_t event_id, EventSeverity sev, TaiTimestamp now,
                              uint32_t arg0 = 0U, uint32_t arg1 = 0U) noexcept {
        if (sev != EventSeverity::Fatal && !is_severity_enabled(sev)) {
            ++filtered_count_;
            return Status::Ok;
        }

        if (sev != EventSeverity::Fatal && cooldown_.ns() > 0U) {
            if (auto* slot = find_or_alloc_tracker(event_id, now)) {
                if (slot->active && elapsed_since(now, slot->last_emitted) < cooldown_) {
                    if (slot->suppressed < 0xFFFFU) {
                        ++slot->suppressed;
                    }
                    slot->last_seen = now;
                    ++suppressed_count_;
                    return Status::Ok;
                }
                const uint16_t repeats = static_cast<uint16_t>(1U + slot->suppressed);
                const TaiTimestamp first = slot->suppressed > 0U ? slot->first_suppressed : now;
                slot->active           = true;
                slot->last_emitted     = now;
                slot->first_suppressed = now;
                slot->suppressed       = 0U;

                return push_record(event_id, sev, first, now, arg0, arg1, repeats);
            }
        }

        return push_record(event_id, sev, now, now, arg0, arg1, 1U);
    }

    [[nodiscard]] uint32_t filtered_count() const noexcept { return filtered_count_; }
    [[nodiscard]] uint32_t suppressed_count() const noexcept { return suppressed_count_; }
    [[nodiscard]] uint32_t dropped_count() const noexcept { return dropped_count_; }

private:
    struct Tracker {
        uint32_t     event_id{0U};
        TaiTimestamp last_emitted{0U};
        TaiTimestamp first_suppressed{0U};
        TaiTimestamp last_seen{0U};
        uint16_t     suppressed{0U};
        bool         active{false};
    };

    Tracker* find_or_alloc_tracker(uint32_t event_id, TaiTimestamp now) noexcept {
        Tracker* empty_slot = nullptr;
        for (auto& t : trackers_) {
            if (t.active && t.event_id == event_id) {
                return &t;
            }
            if (!t.active && empty_slot == nullptr) {
                empty_slot = &t;
            }
        }
        if (empty_slot != nullptr) {
            empty_slot->event_id         = event_id;
            empty_slot->first_suppressed = now;
            empty_slot->suppressed       = 0U;
            return empty_slot;
        }
        return nullptr;
    }

    Status push_record(uint32_t event_id, EventSeverity sev, TaiTimestamp first_tai,
                       TaiTimestamp last_tai, uint32_t arg0, uint32_t arg1,
                       uint16_t repeat_count) noexcept {
        const EventRecord rec{
            .first_tai_ns = first_tai.ns(),
            .last_tai_ns  = last_tai.ns(),
            .event_id     = event_id,
            .arg0         = arg0,
            .arg1         = arg1,
            .repeat_count = repeat_count,
            .severity     = sev,
            .partition_id = partition_id_,
        };
        const Status st = producer_.try_push(rec);
        if (st != Status::Ok) {
            ++dropped_count_;
        }
        return st;
    }

    SpscProducer<EventRecord, Capacity, kGuaranteedControlQoS> producer_;
    std::array<Tracker, MaxTrackedIds>                         trackers_{};
    uint8_t                                                    partition_id_{0U};
    uint8_t                                                    severity_mask_{0x7FU}; // All enabled
    DurationNs                                                 cooldown_{0U};
    uint32_t                                                   filtered_count_{0U};
    uint32_t                                                   suppressed_count_{0U};
    uint32_t                                                   dropped_count_{0U};
};

} // namespace strata
