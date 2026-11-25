#pragma once

#include <strata/core/concepts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace strata::mmio {

/// Compile-time register access policy tags (prevents writing RO or reading WO registers).
struct RO {};
struct WO {};
struct RW {};
struct RW1C {}; // Write-1-to-clear latched hardware fault/interrupt register

/// Typed compile-time-checked 32-bit MMIO register view over any RegisterTransport.
template <uint32_t OffsetBytes, typename AccessTag, typename ValueT = uint32_t>
    requires((OffsetBytes % 4U) == 0U && sizeof(ValueT) <= 4U &&
             (std::is_integral_v<ValueT> || std::is_enum_v<ValueT>))
class Reg {
public:
    static constexpr uint32_t kOffset = OffsetBytes;

    template <RegisterTransport Transport>
    [[nodiscard]] static ValueT read(const Transport& bus) noexcept
        requires(std::is_same_v<AccessTag, RO> || std::is_same_v<AccessTag, RW> ||
                 std::is_same_v<AccessTag, RW1C>)
    {
        return static_cast<ValueT>(bus.read32(OffsetBytes));
    }

    template <RegisterTransport Transport>
    static void write(Transport& bus, ValueT value) noexcept
        requires(std::is_same_v<AccessTag, WO> || std::is_same_v<AccessTag, RW>)
    {
        bus.write32(OffsetBytes, static_cast<uint32_t>(value));
    }

    template <RegisterTransport Transport>
    static void clear_bits(Transport& bus, uint32_t mask) noexcept
        requires std::is_same_v<AccessTag, RW1C>
    {
        bus.write32(OffsetBytes, mask);
    }
};

/// Direct volatile AXI4-Lite MMIO transport for bare-metal Versal/Kintex targets.
class DirectMmioTransport {
public:
    explicit DirectMmioTransport(uintptr_t base_addr) noexcept : base_(base_addr) {}

    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept {
        return *reinterpret_cast<const volatile uint32_t*>(base_ + offset);
    }

    void write32(uint32_t offset, uint32_t value) noexcept {
        *reinterpret_cast<volatile uint32_t*>(base_ + offset) = value;
    }

private:
    uintptr_t base_;
};
static_assert(RegisterTransport<DirectMmioTransport>);

/// Host SIL / unit-test mock 32-bit register bank (supports RW1C bit clearing simulation).
template <size_t SizeBytes = 8192U>
    requires((SizeBytes % 4U) == 0U)
class MockRegisterBank {
public:
    static constexpr size_t kNumWords = SizeBytes / 4U;

    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept {
        const size_t idx = offset / 4U;
        return (idx < kNumWords) ? words_[idx] : 0U;
    }

    void write32(uint32_t offset, uint32_t value) noexcept {
        const size_t idx = offset / 4U;
        if (idx < kNumWords) {
            if (rw1c_mask_[idx]) {
                words_[idx] &= ~value; // Hardware write-1-to-clear semantics
            } else {
                words_[idx] = value;
            }
            ++write_count_;
        }
    }

    /// Backdoor helper for SIL testbench / hardware simulator to inject RO/latched values.
    void force_word(uint32_t offset, uint32_t value) noexcept {
        const size_t idx = offset / 4U;
        if (idx < kNumWords) {
            words_[idx] = value;
        }
    }

    void mark_rw1c(uint32_t offset) noexcept {
        const size_t idx = offset / 4U;
        if (idx < kNumWords) {
            rw1c_mask_[idx] = true;
        }
    }

    [[nodiscard]] uint32_t write_count() const noexcept { return write_count_; }

private:
    std::array<uint32_t, kNumWords> words_{};
    std::array<bool, kNumWords>     rw1c_mask_{};
    uint32_t                        write_count_{0U};
};
static_assert(RegisterTransport<MockRegisterBank<4096U>>);

} // namespace strata::mmio
