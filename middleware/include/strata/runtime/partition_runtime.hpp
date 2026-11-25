#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>

namespace strata::runtime {

/// Deterministic Cyclic Rate Group / Phased Window task descriptor (F' ActiveRateGroup + ActivePhaser parity).
/// Executes zero-thread, zero-vtable inside a hypervisor partition minor-frame loop.
template <uint32_t PeriodTicks, uint32_t PhaseOffsetTicks, typename ComponentT,
          void (ComponentT::*Method)(TaiTimestamp) noexcept>
    requires(PeriodTicks > 0U && PhaseOffsetTicks < PeriodTicks)
struct ScheduledTask {
    static constexpr uint32_t kPeriod = PeriodTicks;
    static constexpr uint32_t kPhase  = PhaseOffsetTicks;

    ComponentT* target{nullptr};

    constexpr void maybe_run(uint64_t tick_index, TaiTimestamp now) const noexcept {
        if ((tick_index % kPeriod) == kPhase && target != nullptr) {
            (target->*Method)(now);
        }
    }
};

/// Zero-thread-explosion static cyclic scheduler (unrolls all tasks at compile time).
template <typename... Tasks>
class CyclicScheduler {
public:
    explicit constexpr CyclicScheduler(Tasks... tasks) noexcept : tasks_(tasks...) {}

    /// Execute one deterministic minor-frame tick across all due RateGroups / PhasedWindows.
    void tick(TaiTimestamp now) noexcept {
        const uint64_t idx = tick_count_++;
        std::apply([idx, now](const auto&... task) noexcept { (task.maybe_run(idx, now), ...); },
                   tasks_);
    }

    [[nodiscard]] uint64_t tick_count() const noexcept { return tick_count_; }

private:
    std::tuple<Tasks...> tasks_;
    uint64_t             tick_count_{0U};
};

/// Single compile-time typed command route (zero virtual dispatch, zero runtime registration).
template <uint16_t Opcode, WirePod PayloadT, typename ComponentT,
          CmdResult (ComponentT::*Handler)(const PayloadT&) noexcept>
struct CommandRoute {
    static constexpr uint16_t kOpcode      = Opcode;
    static constexpr size_t   kPayloadSize = sizeof(PayloadT);

    ComponentT* target{nullptr};

    [[nodiscard]] CmdResult invoke(std::span<const std::byte> raw_payload) const noexcept {
        if (raw_payload.size() != sizeof(PayloadT) || target == nullptr) {
            return CmdResult::InvalidArg;
        }
        PayloadT cmd{};
        std::memcpy(&cmd, raw_payload.data(), sizeof(PayloadT));
        return (target->*Handler)(cmd);
    }
};

namespace detail {
template <uint16_t... Opcodes>
consteval bool all_opcodes_unique() noexcept {
    constexpr std::array<uint16_t, sizeof...(Opcodes)> arr{Opcodes...};
    for (size_t i = 0U; i < arr.size(); ++i) {
        for (size_t j = i + 1U; j < arr.size(); ++j) {
            if (arr[i] == arr[j]) {
                return false;
            }
        }
    }
    return true;
}
} // namespace detail

/// Compile-time CommandRouter with static duplicate-opcode prevention and zero vtables.
template <typename... Routes>
class CommandRouter {
public:
    static_assert(detail::all_opcodes_unique<Routes::kOpcode...>(),
                  "Duplicate command opcode detected in CommandRouter!");

    explicit constexpr CommandRouter(Routes... routes) noexcept : routes_(routes...) {}

    [[nodiscard]] CmdResult dispatch(uint16_t opcode,
                                     std::span<const std::byte> payload) const noexcept {
        CmdResult result  = CmdResult::UnknownOpcode;
        bool      matched = false;
        std::apply(
            [opcode, payload, &result, &matched](const auto&... route) noexcept {
                (([&]() noexcept {
                     if (!matched && opcode == std::decay_t<decltype(route)>::kOpcode) {
                         result  = route.invoke(payload);
                         matched = true;
                     }
                 }()),
                 ...);
            },
            routes_);
        return result;
    }

private:
    std::tuple<Routes...> routes_;
};

} // namespace strata::runtime
