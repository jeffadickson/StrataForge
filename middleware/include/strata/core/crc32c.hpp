#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace strata {

namespace detail {
inline constexpr uint32_t kCrc32cPolyReflected = 0x82F6'3B78U;

consteval std::array<uint32_t, 256U> make_crc32c_table() noexcept {
    std::array<uint32_t, 256U> tbl{};
    for (uint32_t i = 0U; i < 256U; ++i) {
        uint32_t c = i;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            c = ((c & 1U) != 0U) ? (kCrc32cPolyReflected ^ (c >> 1U)) : (c >> 1U);
        }
        tbl[i] = c;
    }
    return tbl;
}

inline constexpr std::array<uint32_t, 256U> kCrc32cTable = make_crc32c_table();
} // namespace detail

/// Castagnoli CRC-32C (polynomial 0x82F63B78, reflected) with compile-time lookup table.
/// Used across ParamBank, CriticalStore, FrameDesc, and wire framing.
class Crc32c {
public:
    static constexpr uint32_t kPolyReflected = detail::kCrc32cPolyReflected;

    [[nodiscard]] static constexpr uint32_t update(uint32_t crc,
                                                   std::span<const std::byte> data) noexcept {
        uint32_t state = ~crc;
        for (const std::byte b : data) {
            const auto idx = static_cast<uint8_t>((state ^ std::to_integer<uint8_t>(b)) & 0xFFU);
            state = (state >> 8U) ^ detail::kCrc32cTable[idx];
        }
        return ~state;
    }

    [[nodiscard]] static constexpr uint32_t compute(std::span<const std::byte> data) noexcept {
        return update(0U, data);
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] static uint32_t compute_pod(const T& obj) noexcept {
        return compute(std::as_bytes(std::span<const T, 1>{&obj, 1U}));
    }
};

} // namespace strata
