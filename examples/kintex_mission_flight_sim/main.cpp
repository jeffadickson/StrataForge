// =============================================================================
// STRATA-20 End-to-End Kintex (XQRKU060) Dual-Instrument Mission Simulator
// Consumes build-time auto-generated `strata_mission_icd.hpp` and
// `strata_hyp_tables.hpp` from `manifests/examples/kintex_dual_instrument.toml`.
//
// Exercises Kintex-specific mixed-criticality spaceflight workflows:
//  1. KintexMicroBlazeBackend scheduling 4 partitions (cdh_b, fdir_slot_mgr_b,
//     inst_spatial_c, inst_dfx_d) and rejecting Versal-only AIE ops.
//  2. LiDAR spatial zero-copy FramePool -> DataTopic -> SpaceWire InterNodeBridge
//     forwarding to an adjacent spacecraft avionics board.
//  3. Repeated CRAM SEU faults in SDR dynamic slot -> FdirEngine escalates from
//     single-slot ICAPE3 reload to automatic Spare-Slot Failover (`FailoverSpareSlot`)
//     while restoring SDR operational state from ping-pong CriticalStore<T>.
// =============================================================================

#include "strata_hyp_tables.hpp"
#include "strata_mission_icd.hpp"

#include <microblaze/kintex_mb_backend.hpp>
#include <strata/middleware.hpp>
#include <strata/services/comms/link_adapters.hpp>
#include <strata/services/fdir/fdir_engine.hpp>
#include <strata/services/fdir/slot_manager.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define KINTEX_CHECK(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "KINTEX FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

using namespace strata;
using namespace strata::generated;

static_assert(kManifestHash == kHypManifestHash, "Kintex ICD and Hypervisor hashes must match");

struct SdrCheckpoint {
    uint32_t center_freq_khz{2'250'000U};
    uint32_t packets_demodulated{0U};
};

struct KintexIcapBackend {
    uint64_t expected_hash{kManifestHash};
    uint32_t icap_loads{0U};
    uint32_t ext_scrubber_pauses{0U};
    uint32_t ext_scrubber_reinits{0U};

    [[nodiscard]] bool verify_signature(const services::fdir::SignedSlotImage& img) noexcept {
        return img.expected_manifest_hash == expected_hash;
    }
    [[nodiscard]] Status quiesce_and_decouple(uint16_t, services::fdir::SlotFabricKind) noexcept {
        return Status::Ok;
    }
    [[nodiscard]] Status pause_scrubber(uint16_t, services::fdir::SlotFabricKind) noexcept {
        ++ext_scrubber_pauses;
        return Status::Ok;
    }
    [[nodiscard]] Status load_bitstream(uint16_t, services::fdir::SlotFabricKind kind,
                                        const services::fdir::SignedSlotImage&) noexcept {
        if (kind != services::fdir::SlotFabricKind::KintexPlDfx) return Status::InvalidArg;
        ++icap_loads;
        return Status::Ok;
    }
    [[nodiscard]] Status reinit_scrubber(uint16_t, services::fdir::SlotFabricKind) noexcept {
        ++ext_scrubber_reinits;
        return Status::Ok;
    }
    [[nodiscard]] Status release_decoupler(uint16_t, services::fdir::SlotFabricKind) noexcept {
        return Status::Ok;
    }
    [[nodiscard]] Status run_sbit(uint16_t) noexcept { return Status::Ok; }
    [[nodiscard]] uint64_t read_scsr_manifest_hash(uint16_t) noexcept { return expected_hash; }
    [[nodiscard]] Status set_slot_active(uint16_t) noexcept { return Status::Ok; }
};

/// Static `.bss` arena keeping CPU stack well under 4 KB flight ceiling.
struct KintexArena {
    hyp::arch::KintexMicroBlazeBackend                              mb_backend{};
    hyp::HypervisorCore<hyp::arch::KintexMicroBlazeBackend>         hypervisor{
        mb_backend, kMinorFrameUs, kMajorFrameUs,
        static_cast<uint8_t>(PartitionId::fdir_slot_mgr_b)};
    std::array<std::byte, 8U * 512U>                                lidar_ddr{};
    CriticalStoreStorage<SdrCheckpoint>                             sdr_cds_storage{};
    services::fdir::FdirEngine<16U, 8U, 8U>                         fdir{};
    KintexIcapBackend                                               icap_backend{};
    services::fdir::SlotManager<KintexIcapBackend, 4U>              slot_mgr{icap_backend};
    services::comms::InterNodeBridge<128U>                          spw_tx{1U, services::comms::BridgeLinkProfile::SpaceWire};
    services::comms::InterNodeBridge<128U>                          spw_rx{2U, services::comms::BridgeLinkProfile::SpaceWire};
};

KintexArena g_kintex{};

} // namespace

int main() {
    using namespace strata;
    using namespace strata::generated;

    auto& k = g_kintex;

    // 1. Register all 4 Kintex partitions from auto-generated kPartitionTable
    for (const auto& pcfg : kPartitionTable) {
        KINTEX_CHECK(k.hypervisor.register_partition(pcfg) == Status::Ok);
    }
    const uint8_t fdir_pid = static_cast<uint8_t>(PartitionId::fdir_slot_mgr_b);
    const uint8_t sdr_pid  = static_cast<uint8_t>(PartitionId::inst_dfx_d);

    // Verify Kintex backend rejects Versal-only AIE ops and accepts ICAPE3 DFX LoadPartialPdi
    KINTEX_CHECK(k.hypervisor.handle_hypcall(fdir_pid, hyp::HypCall::SlotOp, 1U,
                                             static_cast<uint64_t>(hyp::SlotOpCode::AieColumnReset))
                     .error() == Status::InvalidArg);
    KINTEX_CHECK(k.hypervisor.handle_hypcall(fdir_pid, hyp::HypCall::SlotOp, 1U,
                                             static_cast<uint64_t>(hyp::SlotOpCode::LoadPartialPdi))
                     .has_value());

    // 2. LiDAR Spatial Zero-Copy Pool -> DataTopic -> SpaceWire InterNodeBridge Cross-Board Tunnel
    static FramePool<8U, 512U>                     lidar_pool(k.lidar_ddr, 0x8000'0000ULL);
    static DataTopic<FramePool<8U, 512U>, 2U, 8U>  lidar_topic(lidar_pool);
    const auto cdh_sub = lidar_topic.add_subscriber(kGuaranteedControlQoS);
    KINTEX_CHECK(cdh_sub.has_value());

    const auto slot_idx = lidar_pool.allocate_slot(GenId{1U});
    KINTEX_CHECK(slot_idx.has_value());
    FrameDesc lidar_desc{
        .phys_addr     = lidar_pool.slot_phys_addr(slot_idx.value()),
        .tai_ns        = 15'000'000ULL,
        .length_bytes  = 512U,
        .pool_slot_idx = slot_idx.value(),
        .src_slot_id   = kHwSlots[0].slot_id,
        .producer_gen  = 1U,
        .flags         = 0x05U,
        .crc32c        = 0x55AA'1234U,
    };
    FrameLease<FramePool<8U, 512U>> lease(lidar_pool, lidar_desc);
    KINTEX_CHECK(lidar_topic.publish(std::move(lease), TaiTimestamp{15'000'000ULL}) == Status::Ok);
    const auto rx_view = lidar_topic.try_receive(cdh_sub.value());
    KINTEX_CHECK(rx_view.has_value());

    std::array<std::byte, 128U> spw_wire{};
    const auto wlen = k.spw_tx.encode_descriptor(2U, 3U, rx_view.value().desc(), spw_wire);
    KINTEX_CHECK(wlen.has_value());
    const auto decoded = k.spw_rx.decode(std::span<const std::byte>{spw_wire.data(), wlen.value()});
    KINTEX_CHECK(decoded.has_value() && decoded.value().payload.size() == sizeof(FrameDesc));

    // 3. Repeated CRAM SEU in SDR Slot 1 -> Escalates to Spare-Slot Failover (Slot 2) + CriticalStore Restore
    CriticalStore<SdrCheckpoint> sdr_cds(k.sdr_cds_storage, kManifestHash);
    sdr_cds.checkpoint(SdrCheckpoint{.center_freq_khz = 2'287'500U, .packets_demodulated = 4096U});

    KINTEX_CHECK(k.fdir.register_cram_range(services::fdir::CramFrameRange{
                     .frame_addr_start = 0x0010'0000U,
                     .frame_addr_end   = 0x0010'FFFFU,
                     .slot_id          = kHwSlots[1].slot_id,
                     .owner_partition  = sdr_pid,
                     .enabled          = true}) == Status::Ok);
    KINTEX_CHECK(k.fdir.register_slot_failover(services::fdir::SlotFailoverEntry{
                     .primary_slot_id   = kHwSlots[1].slot_id,
                     .spare_slot_id     = 2U,
                     .personality_id    = 0x0055U,
                     .standby_partition = sdr_pid,
                     .enabled           = true}) == Status::Ok);

    KINTEX_CHECK(k.slot_mgr.register_slot(1U, services::fdir::SlotFabricKind::KintexPlDfx) == Status::Ok);
    KINTEX_CHECK(k.slot_mgr.register_slot(2U, services::fdir::SlotFabricKind::KintexPlDfx) == Status::Ok);

    services::fdir::SignedSlotImage sdr_rm{
        .image_id               = 55U,
        .expected_manifest_hash = kManifestHash,
        .staged_in_verified_vol = true,
    };

    // First SEU -> reload primary slot 1 via ICAPE3
    const auto d1 = k.fdir.handle_uncorrectable_cram_upset(0x0010'0400U, /*max_reloads=*/1U);
    KINTEX_CHECK(d1.level == services::fdir::RecoveryLevel::QuiesceAndReloadSlot && d1.primary_slot_id == 1U);
    KINTEX_CHECK(k.slot_mgr.load_and_verify(1U, sdr_rm) == Status::Ok);
    KINTEX_CHECK(k.slot_mgr.activate(1U) == Status::Ok);

    // Second SEU exceeds max_reloads=1 -> FdirEngine automatically promotes to FailoverSpareSlot (Slot 2)!
    const auto d2 = k.fdir.handle_uncorrectable_cram_upset(0x0010'0404U, /*max_reloads=*/1U);
    KINTEX_CHECK(d2.level == services::fdir::RecoveryLevel::FailoverSpareSlot && d2.failover_slot_id == 2U);
    KINTEX_CHECK(k.slot_mgr.load_and_verify(d2.failover_slot_id, sdr_rm) == Status::Ok);
    KINTEX_CHECK(k.slot_mgr.activate(d2.failover_slot_id) == Status::Ok);

    // Failover partition seamlessly restores SDR tuning & demod counters from CriticalStore
    const auto restored = sdr_cds.restore();
    KINTEX_CHECK(restored.has_value() && restored.value().center_freq_khz == 2'287'500U &&
                 restored.value().packets_demodulated == 4096U);

    std::printf(
        "PASS: kintex_mission_flight_sim | HASH=0x%016llX | parts=%zu | icap_loads=%u | "
        "failover_slot=%u | spw_rx=%u\n",
        static_cast<unsigned long long>(kManifestHash), kPartitionTable.size(),
        k.icap_backend.icap_loads, d2.failover_slot_id, k.spw_rx.rx_frames());
    return 0;
}
