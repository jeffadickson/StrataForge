#include <strata/hyp/abi.hpp>
#include <strata/middleware.hpp>

#include <array>
#include <cstddef>
#include <cstdio>

namespace {

using namespace strata::literals;

constexpr uint64_t kMissionManifestHash = 0xFDD1'4727'5E07'25EFULL;

struct HsiHk {
    uint64_t          tai_ns{0U};
    strata::SlotState slot_state{strata::SlotState::Standby};
    uint32_t          frames_published{0U};
    uint32_t          exposure_us{0U};
    uint32_t          param_ver{0U};
};

struct HsiParams {
    uint32_t exposure_us{2000U};
    uint32_t gain{4U};
};

struct HsiCheckpoint {
    uint32_t total_frames{0U};
};

struct CmdSetExposure {
    uint32_t exposure_us{0U};
};

using RegExposureUs = strata::mmio::Reg<0x1000U, strata::mmio::RW, uint32_t>;
using SimPool       = strata::FramePool<8U, 1024U>;
using SimTopic      = strata::DataTopic<SimPool, 4U, 8U>;

struct HyperspecInstrument {
    strata::hw::SlotHandle<strata::mmio::MockRegisterBank<8192U>>& slot;
    SimPool&                                                       pool;
    SimTopic&                                                      topic;
    strata::BlackboardWriter<HsiHk>&                               hk;
    strata::ParamBank<HsiParams>&                                  params;
    strata::CriticalStore<HsiCheckpoint>&                          cds;
    uint32_t                                                       frames_published{0U};

    [[nodiscard]] strata::Status on_slot_ready(strata::GenId) noexcept {
        if (!slot.verify_manifest_hash(kMissionManifestHash)) {
            return strata::Status::ManifestMismatch;
        }
        if (auto saved = cds.restore(); saved) {
            frames_published = saved->total_frames;
        }
        slot.write_reg<RegExposureUs>(params.active().exposure_us);
        slot.command(strata::SlotCmd::StartActive);
        return strata::Status::Ok;
    }

    [[nodiscard]] strata::CmdResult on_set_exposure(const CmdSetExposure& cmd) noexcept {
        auto st = params.stage(
            HsiParams{.exposure_us = cmd.exposure_us, .gain = params.active().gain},
            [](const HsiParams& p) noexcept {
                return p.exposure_us >= 100U && p.exposure_us <= 50'000U;
            });
        if (st != strata::Status::Ok || params.commit() != strata::Status::Ok) {
            return strata::CmdResult::InvalidArg;
        }
        slot.write_reg<RegExposureUs>(params.active().exposure_us);
        return strata::CmdResult::Ok;
    }

    void step_100hz(strata::TaiTimestamp now) noexcept {
        auto idx = pool.allocate_slot(strata::GenId{1U});
        if (!idx) {
            return;
        }
        strata::FrameDesc desc{
            .phys_addr     = pool.slot_phys_addr(*idx),
            .tai_ns        = now.ns(),
            .length_bytes  = 256U,
            .pool_slot_idx = *idx,
            .src_slot_id   = slot.slot_id(),
            .producer_gen  = 1U,
        };
        strata::FrameLease<SimPool> lease(pool, desc);
        auto bytes = lease.writable_bytes();
        bytes[0]   = static_cast<std::byte>(frames_published & 0xFFU);
        lease.mutable_desc().crc32c = strata::Crc32c::compute(bytes.subspan(0U, 256U));

        if (topic.publish(std::move(lease), now) == strata::Status::Ok) {
            ++frames_published;
            cds.checkpoint(HsiCheckpoint{.total_frames = frames_published});
        }
        slot.kick_watchdog();
    }

    void step_1hz(strata::TaiTimestamp now) noexcept {
        hk.publish(HsiHk{
                       .tai_ns           = now.ns(),
                       .slot_state       = slot.state(),
                       .frames_published = frames_published,
                       .exposure_us      = params.active().exposure_us,
                       .param_ver        = params.version(),
                   },
                   now);
    }
};

static_assert(strata::InstrumentComponent<HyperspecInstrument>);

} // namespace

int main() {
    // Hardware SCSR mock (Versal Spatial Slot 0)
    strata::mmio::MockRegisterBank<8192U> scsr_bank{};
    scsr_bank.force_word(strata::hw::scsr::RegMagicVer::kOffset, strata::hw::scsr::kMagic);
    scsr_bank.force_word(strata::hw::scsr::RegManifestHashLo::kOffset,
                         static_cast<uint32_t>(kMissionManifestHash & 0xFFFF'FFFFULL));
    scsr_bank.force_word(strata::hw::scsr::RegManifestHashHi::kOffset,
                         static_cast<uint32_t>(kMissionManifestHash >> 32U));
    scsr_bank.force_word(strata::hw::scsr::RegState::kOffset,
                         static_cast<uint32_t>(strata::SlotState::Active));

    strata::hw::SlotHandle slot(scsr_bank, 0U);
    std::array<std::byte, 8U * 1024U> ddr_pool{};
    SimPool  pool(ddr_pool, 0x0000'0008'0000'0000ULL);
    SimTopic topic(pool);

    auto sub_recorder = topic.add_subscriber(strata::kGuaranteedControlQoS);
    auto sub_ml       = topic.add_subscriber(strata::kBestEffortScienceQoS);
    if (!sub_recorder || !sub_ml) return 1;

    strata::BlackboardStorage<HsiHk>         hk_mem{};
    strata::BlackboardWriter<HsiHk>          hk_writer(hk_mem, strata::GenId{1U});
    strata::BlackboardReader<HsiHk>          hk_reader(hk_mem);
    strata::ParamBank<HsiParams>             params(HsiParams{});
    strata::CriticalStoreStorage<HsiCheckpoint> cds_mem{};
    strata::CriticalStore<HsiCheckpoint>     cds(cds_mem, kMissionManifestHash);

    HyperspecInstrument inst{slot, pool, topic, hk_writer, params, cds};
    if (inst.on_slot_ready(strata::GenId{1U}) != strata::Status::Ok) return 1;

    strata::runtime::CyclicScheduler sched(
        strata::runtime::ScheduledTask<1U, 0U, HyperspecInstrument,
                                       &HyperspecInstrument::step_100hz>{&inst},
        strata::runtime::ScheduledTask<5U, 0U, HyperspecInstrument,
                                       &HyperspecInstrument::step_1hz>{&inst});

    strata::runtime::CommandRouter router(
        strata::runtime::CommandRoute<0x0101U, CmdSetExposure, HyperspecInstrument,
                                      &HyperspecInstrument::on_set_exposure>{&inst});

    const CmdSetExposure cmd{.exposure_us = 3500U};
    if (router.dispatch(0x0101U, std::as_bytes(std::span<const CmdSetExposure, 1>{&cmd, 1U})) !=
        strata::CmdResult::Ok) {
        return 1;
    }

    uint32_t recorded = 0U;
    for (uint64_t tick = 0U; tick < 10U; ++tick) {
        const strata::TaiTimestamp now{(tick + 1U) * (10_ms).ns()};
        sched.tick(now);

        if (auto view = topic.try_receive(*sub_recorder); view) {
            ++recorded;
            view->release();
        }
        // Class D ML subscriber intentionally stalls; FDIR reclaims expired leases cleanly
        topic.reclaim_expired(now + 60_ms);
    }

    auto hk_snap = hk_reader.read(strata::TaiTimestamp{(100_ms).ns()});
    if (!hk_snap) return 1;

    std::printf(
        "StrataForge End-to-End SIL PASS | recorded=%u | hk_frames=%u | exposure=%u us | "
        "param_ver=%u | cds_seq=%u | pool_free=%zu/%zu | reclaimed=%u\n",
        recorded, hk_snap->value.frames_published, hk_snap->value.exposure_us,
        hk_snap->value.param_ver, cds.latest_sequence(), pool.free_count(), SimPool::kSlotCount,
        pool.reclaimed_count());
    return 0;
}
