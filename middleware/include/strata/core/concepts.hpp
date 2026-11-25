#pragma once

#include <strata/core/types.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace strata {

/// Shared-memory / wire-safe POD constraint: no vtables, no pointers-to-members, memcpy-safe.
template <typename T>
concept WirePod = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                  std::is_default_constructible_v<T>;

/// Power-of-two compile-time check for lock-free ring buffer depths.
template <size_t N>
concept PowerOfTwo = (N > 0U) && ((N & (N - 1U)) == 0U);

/// Concept for hardware or host-SIL doorbell notification backends.
template <typename T>
concept DoorbellNotifier = requires(T& notifier, uint32_t channel_id) {
    { notifier.notify(channel_id) } noexcept;
};

/// No-op doorbell notifier for polling-only or host unit tests.
struct NullNotifier {
    constexpr void notify(uint32_t) noexcept {}
};
static_assert(DoorbellNotifier<NullNotifier>);

/// Concept for TAI hardware clock or mock SIL clock sources.
template <typename T>
concept TimeSource = requires(const T& clk) {
    { clk.now() } noexcept -> std::same_as<TaiTimestamp>;
};

/// Concept for 32-bit MMIO register access backends (real AXI4-Lite MMIO, host mock, or Verilator).
template <typename T>
concept RegisterTransport = requires(T& bus, const T& cbus, uint32_t offset, uint32_t val) {
    { cbus.read32(offset) } noexcept -> std::same_as<uint32_t>;
    { bus.write32(offset, val) } noexcept;
};

/// Concept for bare-metal instrument components managed by PartitionRuntime.
template <typename T>
concept InstrumentComponent = requires(T& comp, GenId gen) {
    { comp.on_slot_ready(gen) } noexcept -> std::same_as<Status>;
};

} // namespace strata
