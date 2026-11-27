#include <strata/instruments/reference_instruments.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define STRATA_CHECK(expr)                                                                   \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::abort();                                                                    \
        }                                                                                    \
    } while (false)

int main() {
    using namespace strata;
    using namespace strata::instruments;

    constexpr uint64_t kManifestHash = 0xEE35'F9CF'C303'6975ULL;

    // 1. HyperspectralImagerMock: power-on, SCSR MMIO, thermal/power physics, zero-copy FramePool
    {
        HyperspectralImagerMock<512U> hsi{kManifestHash, 1U, 0x1234'5678ULL};
        STRATA_CHECK(hsi.mode() == InstrumentMode::Off);
        hsi.power_on();
        STRATA_CHECK(hsi.mode() == InstrumentMode::Standby);

        // Verify typed compile-time strata::mmio::Reg<> access directly over InstrumentRegisterBlock!
        using MagicReg = mmio::Reg<reg::kMagic, mmio::RO>;
        using HashLo   = mmio::Reg<reg::kManifestHashLo, mmio::RO>;
        using ModeReg  = mmio::Reg<reg::kMode, mmio::RW>;
        using FaultReg = mmio::Reg<reg::kFaultFlags, mmio::RW1C>;

        STRATA_CHECK(MagicReg::read(hsi.registers()) == 0x5354'494DU);
        STRATA_CHECK(HashLo::read(hsi.registers()) == static_cast<uint32_t>(kManifestHash & 0xFFFF'FFFFULL));

        // Transition to Science mode via MMIO register write & configure CalibrationLamp scene
        ModeReg::write(hsi.registers(), static_cast<uint32_t>(InstrumentMode::Science));
        STRATA_CHECK(hsi.handle_command(cmd::kSetIntegration, 6000U) == Status::Ok);
        STRATA_CHECK(hsi.handle_command(cmd::kSetScene, static_cast<uint64_t>(ScenePattern::CalibrationLamp)) == Status::Ok);

        // Back a zero-copy FramePool and produce a synthesized science frame
        static std::array<std::byte, 4U * 512U> backing{};
        FramePool<4U, 512U>                     pool{backing, 0x1000'0000ULL};

        const bool ready = hsi.step(TaiTimestamp{1'000'000ULL}, 0.2f);
        STRATA_CHECK(ready);
        STRATA_CHECK(hsi.mode() == InstrumentMode::Science);

        const auto slot_res = hsi.produce_to_frame_pool(pool, GenId{1U}, 0x01U, TaiTimestamp{1'000'000ULL});
        STRATA_CHECK(slot_res.has_value());
        const auto frame = pool.readonly_slot(slot_res.value(), 512U);
        STRATA_CHECK(HyperspectralImagerMock<512U>::verify_frame_crc(frame));

        ScienceFrameHeader hdr{};
        std::memcpy(&hdr, frame.data(), sizeof(hdr));
        STRATA_CHECK(hdr.magic == ScienceFrameHeader::kMagic);
        STRATA_CHECK(hdr.instrument_id == 1U && hdr.frame_seq == 1U && hdr.integration_us == 6000U);
        pool.release_subscriber_slot(slot_res.value(), 0U);
        STRATA_CHECK(pool.free_count() == 4U);

        // 2. Deterministic reproducibility check: identical seed produces byte-identical frames
        HyperspectralImagerMock<512U> hsi_a{kManifestHash, 1U, 0xCAFE'BABEULL};
        HyperspectralImagerMock<512U> hsi_b{kManifestHash, 1U, 0xCAFE'BABEULL};
        hsi_a.power_on();
        hsi_b.power_on();
        STRATA_CHECK(hsi_a.set_mode(InstrumentMode::Science) == Status::Ok);
        STRATA_CHECK(hsi_b.set_mode(InstrumentMode::Science) == Status::Ok);
        hsi_a.set_scene(ScenePattern::PointTarget);
        hsi_b.set_scene(ScenePattern::PointTarget);
        STRATA_CHECK(hsi_a.step(TaiTimestamp{5'000'000ULL}, 0.1f));
        STRATA_CHECK(hsi_b.step(TaiTimestamp{5'000'000ULL}, 0.1f));
        std::array<std::byte, 512U> fa{}, fb{};
        STRATA_CHECK(hsi_a.produce_frame(fa, TaiTimestamp{5'000'000ULL}) == Status::Ok);
        STRATA_CHECK(hsi_b.produce_frame(fb, TaiTimestamp{5'000'000ULL}) == Status::Ok);
        STRATA_CHECK(std::memcmp(fa.data(), fb.data(), fa.size()) == 0);

        // 3. Fault Injection Suite: ThermalRunaway, RegisterSeuBitFlip, FrameCrcCorrupt, DmaStall, WatchdogFreeze
        STRATA_CHECK(hsi.fault_injector().inject_now(
                   InstrumentFaultKind::ThermalRunaway, hsi.tick_count(), 60U, 0U, 120.0f) == Status::Ok);
        const float start_temp = hsi.sample_telemetry().detector_temp_k;
        for (uint32_t t = 0U; t < 40U; ++t) {
            (void)hsi.step(TaiTimestamp{2'000'000ULL + t * 100'000ULL}, 0.5f);
        }
        const auto hk_hot = hsi.sample_telemetry();
        STRATA_CHECK(hk_hot.detector_temp_k > start_temp + 35.0f);
        STRATA_CHECK((hk_hot.fault_flags & fault_bit::kOverTemp) != 0U);

        // Clear fault flags via RW1C MMIO write
        FaultReg::clear_bits(hsi.registers(), fault_bit::kOverTemp);
        STRATA_CHECK((FaultReg::read(hsi.registers()) & fault_bit::kOverTemp) == 0U);

        // Inject Register SEU on reg::kGain bit 3
        const uint32_t gain_before = hsi.registers().read32(reg::kGain);
        STRATA_CHECK(hsi.fault_injector().inject_now(
                   InstrumentFaultKind::RegisterSeuBitFlip, hsi.tick_count(), 1U,
                   (reg::kGain << 8U) | 3U) == Status::Ok);
        (void)hsi.step(TaiTimestamp{9'000'000ULL}, 0.1f);
        STRATA_CHECK(hsi.registers().read32(reg::kGain) == (gain_before ^ (1U << 3U)));
        STRATA_CHECK((hsi.registers().read32(reg::kFaultFlags) & fault_bit::kRegisterSeu) != 0U);

        // Inject FrameCrcCorrupt -> produced frame fails CRC-32C check
        STRATA_CHECK(hsi.fault_injector().inject_now(
                   InstrumentFaultKind::FrameCrcCorrupt, hsi.tick_count(), 2U) == Status::Ok);
        (void)hsi.step(TaiTimestamp{10'000'000ULL}, 0.1f);
        std::array<std::byte, 512U> bad_frame{};
        STRATA_CHECK(hsi.produce_frame(bad_frame, TaiTimestamp{10'000'000ULL}) == Status::Ok);
        STRATA_CHECK(!HyperspectralImagerMock<512U>::verify_frame_crc(bad_frame));
        STRATA_CHECK(hsi.sample_telemetry().crc_error_count == 1U);

        // Inject DmaStall & WatchdogFreeze -> heartbeat stops and DMA returns Timeout
        const uint32_t hb_before = hsi.sample_telemetry().heartbeat;
        STRATA_CHECK(hsi.fault_injector().inject_now(
                   InstrumentFaultKind::DmaStall, hsi.tick_count(), 3U) == Status::Ok);
        STRATA_CHECK(hsi.fault_injector().inject_now(
                   InstrumentFaultKind::WatchdogFreeze, hsi.tick_count(), 3U) == Status::Ok);
        const bool ready_during_stall = hsi.step(TaiTimestamp{11'000'000ULL}, 0.1f);
        STRATA_CHECK(!ready_during_stall);
        STRATA_CHECK(hsi.sample_telemetry().heartbeat == hb_before);
        STRATA_CHECK(hsi.produce_frame(bad_frame, TaiTimestamp{11'000'000ULL}) == Status::Timeout);

        // Clear all faults via command
        STRATA_CHECK(hsi.handle_command(cmd::kClearFaults, 0U) == Status::Ok);
        STRATA_CHECK(hsi.sample_telemetry().fault_flags == 0U);
    }

    // 4. Second Reference Instrument: FluxgateMagnetometerMock with CalibrationLamp coil pulse
    {
        FluxgateMagnetometerMock<128U> mag{kManifestHash, 2U, 0x9988'7766ULL};
        mag.power_on();
        STRATA_CHECK(mag.set_mode(InstrumentMode::Calibration) == Status::Ok);
        mag.set_scene(ScenePattern::CalibrationLamp);
        STRATA_CHECK(mag.step(TaiTimestamp{20'000'000ULL}, 0.1f));

        std::array<std::byte, 128U> frame{};
        STRATA_CHECK(mag.produce_frame(frame, TaiTimestamp{20'000'000ULL}) == Status::Ok);
        STRATA_CHECK(FluxgateMagnetometerMock<128U>::verify_frame_crc(frame));

        MagSample first{};
        std::memcpy(&first, frame.data() + sizeof(ScienceFrameHeader), sizeof(first));
        // Calibration coil adds +5000 nT to Bz (~31000 + 5000 = ~36000 nT)
        STRATA_CHECK(first.bz_nt > 33000);
    }

    std::printf(
        "PASS: Mock Instrument Framework (HyperspectralImagerMock + FluxgateMagnetometerMock, "
        "thermal/power ODE, SCSR MMIO/RW1C, zero-copy FramePool, deterministic fault injection) verified\n");
    return 0;
}
