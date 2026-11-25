#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <atomic>
#include <cstdint>

namespace strata {

/// Double-buffered, CRC-32C-verified Parameter Bank (Primitive 4).
/// Eliminates torn multi-parameter updates via staged validation and atomic minor-frame commit.
template <WirePod ParamT>
class ParamBank {
public:
    explicit ParamBank(const ParamT& defaults) noexcept {
        banks_[0].data    = defaults;
        banks_[0].version = 1U;
        banks_[0].crc32c  = Crc32c::compute_pod(defaults);
        banks_[0].valid   = true;

        banks_[1].valid = false;
        active_idx_.store(0U, std::memory_order_release);
    }

    /// Lock-free ~5 ns read of the currently active parameter set.
    [[nodiscard]] const ParamT& active() const noexcept {
        return banks_[active_idx_.load(std::memory_order_acquire)].data;
    }

    [[nodiscard]] uint32_t version() const noexcept {
        return banks_[active_idx_.load(std::memory_order_acquire)].version;
    }

    /// Verify active bank integrity against radiation-induced RAM SEU bit-flips.
    [[nodiscard]] bool verify_active() const noexcept {
        const auto& b = banks_[active_idx_.load(std::memory_order_acquire)];
        return b.valid && (Crc32c::compute_pod(b.data) == b.crc32c);
    }

    /// Stage a new candidate parameter struct into the inactive shadow bank with custom validator.
    template <typename ValidatorFn>
    [[nodiscard]] Status stage(const ParamT& candidate, ValidatorFn&& validator) noexcept {
        if (!validator(candidate)) {
            return Status::ValidationFailed;
        }
        const uint8_t cur  = active_idx_.load(std::memory_order_acquire);
        const uint8_t next = static_cast<uint8_t>(1U - cur);

        banks_[next].data    = candidate;
        banks_[next].version = banks_[cur].version + 1U;
        banks_[next].crc32c  = Crc32c::compute_pod(candidate);
        banks_[next].valid   = true;
        staged_ready_        = true;
        return Status::Ok;
    }

    /// Stage without extra range validation (still CRC-32C protected).
    [[nodiscard]] Status stage(const ParamT& candidate) noexcept {
        return stage(candidate, [](const ParamT&) noexcept { return true; });
    }

    /// Atomically flip the staged bank live (call on clean minor-frame boundary).
    [[nodiscard]] Status commit() noexcept {
        if (!staged_ready_) {
            return Status::PreconditionFailed;
        }
        const uint8_t cur  = active_idx_.load(std::memory_order_acquire);
        const uint8_t next = static_cast<uint8_t>(1U - cur);
        if (!banks_[next].valid || Crc32c::compute_pod(banks_[next].data) != banks_[next].crc32c) {
            staged_ready_ = false;
            return Status::CrcMismatch;
        }
        active_idx_.store(next, std::memory_order_release);
        staged_ready_ = false;
        has_previous_ = true;
        return Status::Ok;
    }

    /// Roll back atomically to the previously active verified bank.
    [[nodiscard]] Status rollback() noexcept {
        if (!has_previous_) {
            return Status::NoPreviousBank;
        }
        const uint8_t cur  = active_idx_.load(std::memory_order_acquire);
        const uint8_t prev = static_cast<uint8_t>(1U - cur);
        if (!banks_[prev].valid || Crc32c::compute_pod(banks_[prev].data) != banks_[prev].crc32c) {
            return Status::CrcMismatch;
        }
        active_idx_.store(prev, std::memory_order_release);
        staged_ready_ = false;
        return Status::Ok;
    }

private:
    struct Slot {
        ParamT   data{};
        uint32_t version{0U};
        uint32_t crc32c{0U};
        bool     valid{false};
    };

    std::array<Slot, 2U>  banks_{};
    std::atomic<uint8_t>  active_idx_{0U};
    bool                  staged_ready_{false};
    bool                  has_previous_{false};
};

} // namespace strata
