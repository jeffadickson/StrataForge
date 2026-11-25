#pragma once

#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <type_traits>

namespace strata {

/// Single declarative transition entry for zero-allocation compile-time Finite State Machines.
/// Implemented in pure C++20 with zero external codegen and zero vtables.
template <typename Context, typename StateEnum, typename EventEnum>
    requires(std::is_enum_v<StateEnum> && std::is_enum_v<EventEnum>)
struct Transition {
    using GuardFn  = bool (*)(const Context&) noexcept;
    using ActionFn = void (*)(Context&) noexcept;

    StateEnum from{};
    EventEnum event{};
    StateEnum to{};
    GuardFn   guard{nullptr};  // nullptr = always allowed
    ActionFn  action{nullptr}; // nullptr = no-op action
};

/// Zero-allocation, deterministic, table-driven Finite State Machine.
template <typename Context, typename StateEnum, typename EventEnum, size_t NumTransitions>
    requires(std::is_enum_v<StateEnum> && std::is_enum_v<EventEnum>)
class Fsm {
public:
    using TransitionEntry = Transition<Context, StateEnum, EventEnum>;
    using Table           = std::array<TransitionEntry, NumTransitions>;

    constexpr Fsm(StateEnum initial_state, Table table) noexcept
        : state_(initial_state), table_(table) {}

    [[nodiscard]] constexpr StateEnum state() const noexcept { return state_; }
    [[nodiscard]] constexpr uint32_t transition_count() const noexcept { return transition_count_; }

    /// Process one event deterministically in O(NumTransitions) with zero heap or virtual dispatch.
    [[nodiscard]] constexpr Status dispatch(Context& ctx, EventEnum ev) noexcept {
        bool matched_state_event = false;
        for (const auto& t : table_) {
            if (t.from == state_ && t.event == ev) {
                matched_state_event = true;
                if (t.guard != nullptr && !t.guard(ctx)) {
                    continue;
                }
                if (t.action != nullptr) {
                    t.action(ctx);
                }
                state_ = t.to;
                ++transition_count_;
                return Status::Ok;
            }
        }
        return matched_state_event ? Status::GuardRejected : Status::UnhandledEvent;
    }

    /// Forced reset (e.g., on FDIR WarmStart/ColdStart recovery).
    constexpr void force_state(StateEnum s) noexcept { state_ = s; }

private:
    StateEnum state_;
    Table     table_;
    uint32_t  transition_count_{0U};
};

} // namespace strata
