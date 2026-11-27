#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata {

/// ============================================================================
/// Hamming(72,64) SECDED ECC Codec (STRATA-STOR-007)
/// Single-Error Correct, Double-Error Detect on every 64-bit word stored in
/// refresh-free non-volatile / rad-hard memory (MRAM, FRAM, NVSRAM, rad-hard SRAM).
/// Uses a clean extended Hamming(72,64) parity matrix (7 syndrome bits + 1 overall
/// parity bit) guaranteeing:
///   - Any 1-bit flip in data[63:0] OR ecc[7:0] -> corrected in-place.
///   - Any 2-bit flip -> reliably detected as DoubleBitDetected.
/// ============================================================================
enum class EccStatus : uint8_t {
    Clean              = 0U,
    SingleBitCorrected = 1U,
    DoubleBitDetected  = 2U,
};

struct alignas(8) EccWord64 {
    uint64_t data{0U};
    uint8_t  ecc{0U};
    uint8_t  pad[7]{};
};

class Secded64 {
public:
    /// Map data bit index i in [0..63] to standard Hamming position pos in [1..71]
    /// skipping powers of two (1, 2, 4, 8, 16, 32, 64 -> reserved for parity bits p0..p6).
    [[nodiscard]] static constexpr uint8_t data_bit_to_pos(uint8_t bit_idx) noexcept {
        uint8_t pos   = 3U;
        uint8_t count = 0U;
        while (true) {
            const bool is_pow2 = (pos & (pos - 1U)) == 0U;
            if (!is_pow2) {
                if (count == bit_idx) return pos;
                ++count;
            }
            ++pos;
        }
    }

    /// Compute 8-bit SECDED code: bits [6:0] = Hamming parity p0..p6, bit 7 = overall even parity.
    [[nodiscard]] static constexpr uint8_t compute_ecc(uint64_t data) noexcept {
        uint8_t p = 0U;
        for (uint8_t i = 0U; i < 64U; ++i) {
            if (((data >> i) & 1ULL) != 0ULL) {
                p ^= data_bit_to_pos(i);
            }
        }
        // Bit 7 = overall even parity across all 64 data bits + 7 Hamming parity bits
        const uint8_t parity_all = static_cast<uint8_t>(
            (__builtin_popcountll(data) + __builtin_popcount(static_cast<unsigned int>(p))) & 1);
        return static_cast<uint8_t>((static_cast<uint32_t>(p) & 0x7FU) |
                                    (static_cast<uint32_t>(parity_all) << 7U));
    }

    [[nodiscard]] static constexpr EccWord64 encode(uint64_t data) noexcept {
        return EccWord64{.data = data, .ecc = compute_ecc(data)};
    }

    /// Verify and heal an EccWord64 in-place.
    [[nodiscard]] static constexpr EccStatus decode_and_heal(EccWord64& word) noexcept {
        const uint8_t stored_p7  = static_cast<uint8_t>(word.ecc & 0x7FU);
        const uint8_t stored_ov  = static_cast<uint8_t>((word.ecc >> 7U) & 1U);
        const uint8_t expected   = compute_ecc(word.data);
        const uint8_t exp_p7     = static_cast<uint8_t>(expected & 0x7FU);
        const uint8_t syndrome   = static_cast<uint8_t>(stored_p7 ^ exp_p7);

        // Compute overall parity mismatch across stored data + stored 8-bit ecc
        const uint8_t total_ones = static_cast<uint8_t>(
            (__builtin_popcountll(word.data) + __builtin_popcount(static_cast<unsigned int>(word.ecc))) & 1);
        const bool overall_err = (total_ones != 0U);
        (void)stored_ov;

        if (syndrome == 0U && !overall_err) {
            return EccStatus::Clean;
        }
        if (syndrome == 0U && overall_err) {
            // Single-bit flip in the overall parity bit itself (ecc bit 7)
            word.ecc ^= 0x80U;
            return EccStatus::SingleBitCorrected;
        }
        if (syndrome != 0U && !overall_err) {
            // Non-zero syndrome with even overall parity => at least 2 bit errors (uncorrectable)
            return EccStatus::DoubleBitDetected;
        }

        // syndrome != 0 && overall_err => single-bit error at Hamming position `syndrome`
        if ((syndrome & (syndrome - 1U)) == 0U) {
            // Power-of-two position => single-bit flip inside Hamming parity bits ecc[6:0]
            word.ecc ^= syndrome;
            return EccStatus::SingleBitCorrected;
        }

        // Locate data bit whose Hamming position matches `syndrome`
        for (uint8_t i = 0U; i < 64U; ++i) {
            if (data_bit_to_pos(i) == syndrome) {
                word.data ^= (1ULL << i);
                return EccStatus::SingleBitCorrected;
            }
        }
        return EccStatus::DoubleBitDetected;
    }
};

/// ============================================================================
/// Bit-Wise Triple Modular Redundant (TMR 2-of-3 Majority Voted) Storage Block
/// Survives total corruption of any single replica and heals damaged replicas
/// automatically in-place during read or background scrub.
/// ============================================================================
template <WirePod T>
struct alignas(64) TmrBlock {
    std::array<T, 3U> replicas{};

    void write(const T& val) noexcept {
        for (auto& r : replicas) {
            r = val;
        }
        std::atomic_thread_fence(std::memory_order_release);
    }

    /// Perform bit-wise 2-of-3 majority vote across all 3 replicas; returns voted value and
    /// sets `repaired = true` if any replica disagreed (healing all 3 replicas in-place).
    [[nodiscard]] T read_and_heal(bool& repaired) noexcept {
        std::array<std::byte, sizeof(T)> a{};
        std::array<std::byte, sizeof(T)> b{};
        std::array<std::byte, sizeof(T)> c{};
        std::memcpy(a.data(), &replicas[0], sizeof(T));
        std::memcpy(b.data(), &replicas[1], sizeof(T));
        std::memcpy(c.data(), &replicas[2], sizeof(T));

        std::array<std::byte, sizeof(T)> voted{};
        repaired = false;
        for (size_t i = 0U; i < sizeof(T); ++i) {
            const uint8_t ba = std::to_integer<uint8_t>(a[i]);
            const uint8_t bb = std::to_integer<uint8_t>(b[i]);
            const uint8_t bc = std::to_integer<uint8_t>(c[i]);
            const uint8_t mv = static_cast<uint8_t>((ba & bb) | (bb & bc) | (ba & bc));
            voted[i]         = static_cast<std::byte>(mv);
            if (ba != mv || bb != mv || bc != mv) {
                repaired = true;
            }
        }

        T out{};
        std::memcpy(&out, voted.data(), sizeof(T));
        if (repaired) {
            write(out);
        }
        return out;
    }
};

/// ============================================================================
/// Refresh-Free Reboot-Surviving Non-Volatile Flight Log (STRATA-STOR-007)
/// Designed for MRAM / FRAM / NVSRAM / Battery-Backed SRAM placed in linker
/// section `.nv_flight_log (NOLOAD)`. Retains crash dumps, reboot markers, and
/// critical flight events across cold reboots & power cycles without DRAM refresh.
/// ============================================================================
enum class ResetReason : uint16_t {
    PowerOnCold     = 0U,
    WatchdogReset   = 1U,
    SoftwareWarm    = 2U,
    BrownoutRecover = 3U,
    RadiationFault  = 4U,
};

enum class NvRecordKind : uint16_t {
    RebootMarker  = 1U,
    CrashTrap     = 2U,
    CriticalEvent = 3U,
    FdirAction    = 4U,
    DfxTransition = 5U,
    SeuAlert      = 6U,
};

/// Fixed 64-byte logical flight log record (encoded as 8 SECDED-protected 64-bit words).
struct alignas(8) NvLogRecord {
    uint32_t     boot_epoch{0U};
    uint32_t     seq_num{0U};
    uint64_t     tai_ns{0U};
    NvRecordKind kind{NvRecordKind::CriticalEvent};
    uint16_t     source_id{0U};
    uint32_t     code{0U};
    uint64_t     aux0{0U};
    uint64_t     aux1{0U};
    uint64_t     aux2{0U};
    uint32_t     crc32c{0U};
    uint32_t     reserved{0U};
};
static_assert(sizeof(NvLogRecord) == 56U);
static_assert(WirePod<NvLogRecord>);

/// Backing storage struct intended for placement in refresh-free `.nv_flight_log (NOLOAD)`.
template <size_t Capacity = 64U>
    requires PowerOfTwo<Capacity>
struct alignas(64) NvFaultLogStorage {
    static constexpr size_t   kCapacity    = Capacity;
    static constexpr uint32_t kHeaderMagic = 0x5354'4E56U; // "STNV"
    static constexpr uint64_t kSlotCommit  = 0x5354'434D'4954'5631ULL; // "STCMITV1"
    static constexpr size_t   kWordsPerRec = sizeof(NvLogRecord) / sizeof(uint64_t); // 7 words

    struct alignas(8) ControlHeader {
        uint32_t magic{0U};
        uint32_t boot_epoch{0U};
        uint64_t manifest_hash{0U};
        uint32_t next_seq{1U};
        uint32_t head_idx{0U};
        uint32_t total_written{0U};
        uint32_t tmr_repairs{0U};
        uint32_t ecc_ce_count{0U};
        uint32_t ecc_ue_count{0U};
        uint32_t header_crc32c{0U};
        uint32_t reserved{0U};
    };
    static_assert(WirePod<ControlHeader>);

    /// Torn-write-safe SECDED slot: payload words + ECC written first; commit_fence written LAST.
    struct alignas(8) ProtectedSlot {
        std::array<EccWord64, kWordsPerRec> words{};
        EccWord64                           commit_fence{};
    };

    TmrBlock<ControlHeader>                header{};
    std::array<ProtectedSlot, Capacity>    slots{};
};

struct NvScrubReport {
    uint32_t tmr_repairs{0U};
    uint32_t ecc_single_bit_corrected{0U};
    uint32_t ecc_double_bit_detected{0U};
};

template <size_t Capacity = 64U>
    requires PowerOfTwo<Capacity>
class NvFaultLog {
public:
    using StorageT = NvFaultLogStorage<Capacity>;
    using HeaderT  = typename StorageT::ControlHeader;

    explicit NvFaultLog(StorageT& backing) noexcept : mem_(&backing) {}

    /// Mount existing refresh-free MRAM/FRAM block on boot without erasing prior flight logs.
    /// Increments `boot_epoch`, skips any uncommitted torn slot from sudden power loss, and
    /// appends an automatic `RebootMarker` record. Formats cleanly on first-ever power-up.
    [[nodiscard]] Status mount_on_boot(uint64_t manifest_hash, ResetReason reason,
                                       TaiTimestamp now, bool force_cold_format = false) noexcept {
        bool    repaired = false;
        HeaderT hdr      = mem_->header.read_and_heal(repaired);

        const bool valid_existing = !force_cold_format &&
                                    (hdr.magic == StorageT::kHeaderMagic) &&
                                    (hdr.manifest_hash == manifest_hash) &&
                                    (compute_header_crc(hdr) == hdr.header_crc32c);

        if (!valid_existing) {
            // Fresh initialization (first power-up or manifest change)
            hdr = HeaderT{
                .magic         = StorageT::kHeaderMagic,
                .boot_epoch    = 1U,
                .manifest_hash = manifest_hash,
                .next_seq      = 1U,
                .head_idx      = 0U,
                .total_written = 0U,
                .tmr_repairs   = 0U,
                .ecc_ce_count  = 0U,
                .ecc_ue_count  = 0U,
                .header_crc32c = 0U,
                .reserved      = 0U,
            };
            for (auto& s : mem_->slots) {
                s.commit_fence = Secded64::encode(0ULL);
            }
        } else {
            // Survive reboot! Preserve all prior logs and bump boot_epoch.
            ++hdr.boot_epoch;
            if (repaired) {
                ++hdr.tmr_repairs;
            }
            // Torn-write recovery: if a sudden power loss left the current head_idx half-written
            // without commit_fence, clear its fence so it is cleanly overwritten next.
            auto& tail_slot = mem_->slots[hdr.head_idx & (Capacity - 1U)];
            auto  fence     = tail_slot.commit_fence;
            const auto st   = Secded64::decode_and_heal(fence);
            if (st != EccStatus::Clean || fence.data != StorageT::kSlotCommit) {
                tail_slot.commit_fence = Secded64::encode(0ULL);
            }
        }

        commit_header(hdr);
        mounted_ = true;

        // Automatically record reboot boundary marker with reset cause and boot epoch
        return append(NvRecordKind::RebootMarker, /*source_id=*/0U, static_cast<uint32_t>(reason),
                      now, hdr.boot_epoch, hdr.total_written, 0U);
    }

    /// Append a crash dump, reboot marker, or critical event with SECDED ECC and torn-write safety.
    [[nodiscard]] Status append(NvRecordKind kind, uint16_t source_id, uint32_t code,
                                TaiTimestamp now, uint64_t aux0 = 0U, uint64_t aux1 = 0U,
                                uint64_t aux2 = 0U) noexcept {
        if (!mounted_) return Status::PreconditionFailed;

        bool    repaired = false;
        HeaderT hdr      = mem_->header.read_and_heal(repaired);
        if (repaired) ++hdr.tmr_repairs;

        NvLogRecord rec{
            .boot_epoch = hdr.boot_epoch,
            .seq_num    = hdr.next_seq++,
            .tai_ns     = now.ns(),
            .kind       = kind,
            .source_id  = source_id,
            .code       = code,
            .aux0       = aux0,
            .aux1       = aux1,
            .aux2       = aux2,
            .crc32c     = 0U,
            .reserved   = 0U,
        };
        rec.crc32c = compute_record_crc(rec);

        const size_t slot_idx = hdr.head_idx & (Capacity - 1U);
        auto&        slot     = mem_->slots[slot_idx];

        // 1. Invalidate commit fence FIRST so sudden power loss mid-write never looks valid
        slot.commit_fence = Secded64::encode(0ULL);
        std::atomic_thread_fence(std::memory_order_release);

        // 2. Write all 7 SECDED-encoded 64-bit data words
        uint64_t raw_words[StorageT::kWordsPerRec]{};
        std::memcpy(raw_words, &rec, sizeof(rec));
        for (size_t w = 0U; w < StorageT::kWordsPerRec; ++w) {
            slot.words[w] = Secded64::encode(raw_words[w]);
        }
        std::atomic_thread_fence(std::memory_order_release);

        // 3. Commit fence LAST
        slot.commit_fence = Secded64::encode(StorageT::kSlotCommit);
        std::atomic_thread_fence(std::memory_order_release);

        // 4. Advance TMR header atomically across all 3 replicas
        hdr.head_idx = static_cast<uint32_t>((slot_idx + 1U) & (Capacity - 1U));
        ++hdr.total_written;
        commit_header(hdr);
        return Status::Ok;
    }

    /// Read and SECDED-heal entry at ring slot index `slot_idx`.
    [[nodiscard]] Expected<NvLogRecord, Status> read_slot(size_t slot_idx) noexcept {
        if (!mounted_ || slot_idx >= Capacity) return Unexpected(Status::InvalidArg);

        auto& slot = mem_->slots[slot_idx];
        const auto fence_st = Secded64::decode_and_heal(slot.commit_fence);
        if (fence_st == EccStatus::DoubleBitDetected) {
            bump_ecc_counters(0U, 1U);
            return Unexpected(Status::CrcMismatch);
        }
        if (fence_st == EccStatus::SingleBitCorrected) {
            bump_ecc_counters(1U, 0U);
        }
        if (slot.commit_fence.data != StorageT::kSlotCommit) {
            return Unexpected(Status::Empty);
        }

        uint64_t raw_words[StorageT::kWordsPerRec]{};
        uint32_t ce = 0U;
        for (size_t w = 0U; w < StorageT::kWordsPerRec; ++w) {
            const auto st = Secded64::decode_and_heal(slot.words[w]);
            if (st == EccStatus::DoubleBitDetected) {
                bump_ecc_counters(ce, 1U);
                return Unexpected(Status::CrcMismatch);
            }
            if (st == EccStatus::SingleBitCorrected) {
                ++ce;
            }
            raw_words[w] = slot.words[w].data;
        }
        if (ce > 0U) {
            bump_ecc_counters(ce, 0U);
        }

        NvLogRecord rec{};
        std::memcpy(&rec, raw_words, sizeof(rec));
        if (compute_record_crc(rec) != rec.crc32c) {
            bump_ecc_counters(0U, 1U);
            return Unexpected(Status::CrcMismatch);
        }
        return rec;
    }

    /// Incremental O(1) background scrubber: heals TMR header + one SECDED slot per idle tick
    /// so single-bit upsets in MRAM/SRAM never accumulate into double-bit errors.
    [[nodiscard]] NvScrubReport scrub_step() noexcept {
        NvScrubReport rep{};
        if (!mounted_) return rep;

        bool    tmr_rep = false;
        HeaderT hdr     = mem_->header.read_and_heal(tmr_rep);
        if (tmr_rep) {
            ++hdr.tmr_repairs;
            ++rep.tmr_repairs;
        }

        auto& slot     = mem_->slots[scrub_cursor_ & (Capacity - 1U)];
        scrub_cursor_  = (scrub_cursor_ + 1U) & (Capacity - 1U);

        const auto fst = Secded64::decode_and_heal(slot.commit_fence);
        if (fst == EccStatus::SingleBitCorrected) {
            ++hdr.ecc_ce_count;
            ++rep.ecc_single_bit_corrected;
        } else if (fst == EccStatus::DoubleBitDetected) {
            ++hdr.ecc_ue_count;
            ++rep.ecc_double_bit_detected;
        }

        if (slot.commit_fence.data == StorageT::kSlotCommit) {
            for (size_t w = 0U; w < StorageT::kWordsPerRec; ++w) {
                const auto st = Secded64::decode_and_heal(slot.words[w]);
                if (st == EccStatus::SingleBitCorrected) {
                    ++hdr.ecc_ce_count;
                    ++rep.ecc_single_bit_corrected;
                } else if (st == EccStatus::DoubleBitDetected) {
                    ++hdr.ecc_ue_count;
                    ++rep.ecc_double_bit_detected;
                }
            }
        }

        if (rep.tmr_repairs > 0U || rep.ecc_single_bit_corrected > 0U ||
            rep.ecc_double_bit_detected > 0U) {
            commit_header(hdr);
        }
        return rep;
    }

    [[nodiscard]] HeaderT header_snapshot() noexcept {
        bool rep = false;
        return mem_->header.read_and_heal(rep);
    }

    [[nodiscard]] size_t valid_entry_count() noexcept {
        const auto h = header_snapshot();
        return (h.total_written < Capacity) ? h.total_written : Capacity;
    }

private:
    static uint32_t compute_header_crc(HeaderT hdr) noexcept {
        hdr.header_crc32c = 0U;
        return Crc32c::compute(std::as_bytes(std::span<const HeaderT, 1>{&hdr, 1U}));
    }

    static uint32_t compute_record_crc(NvLogRecord rec) noexcept {
        rec.crc32c = 0U;
        return Crc32c::compute(std::as_bytes(std::span<const NvLogRecord, 1>{&rec, 1U}));
    }

    void commit_header(HeaderT& hdr) noexcept {
        hdr.header_crc32c = compute_header_crc(hdr);
        mem_->header.write(hdr);
    }

    void bump_ecc_counters(uint32_t ce, uint32_t ue) noexcept {
        bool    rep = false;
        HeaderT hdr = mem_->header.read_and_heal(rep);
        if (rep) ++hdr.tmr_repairs;
        hdr.ecc_ce_count += ce;
        hdr.ecc_ue_count += ue;
        commit_header(hdr);
    }

    StorageT* mem_;
    bool      mounted_{false};
    size_t    scrub_cursor_{0U};
};

} // namespace strata
