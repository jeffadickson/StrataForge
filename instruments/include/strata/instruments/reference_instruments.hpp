#pragma once

// =============================================================================
// STRATA-20 Reference Mock Flight Instruments
// Concrete zero-heap instruments built on MockInstrumentBase demonstrating rapid
// multi-instrument simulation on shared Versal/Kintex FPGAs:
//   1. HyperspectralImagerMock  — Pushbroom VNIR/SWIR imaging spectrometer
//   2. FluxgateMagnetometerMock — Tri-axial vector boom magnetometer
// =============================================================================

#include <strata/instruments/mock_instrument.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::instruments {

/// Reference Mock Instrument 1: Pushbroom VNIR/SWIR Hyperspectral Imager.
/// Synthesizes 16-bit radiometric pixel lines with dark-current temperature dependence,
/// integration-time scaling, calibration lamp spectral lines, Gaussian point targets,
/// and deterministic cosmic-ray strikes.
template <size_t FrameBytes = 512U>
class HyperspectralImagerMock
    : public MockInstrumentBase<HyperspectralImagerMock<FrameBytes>, FrameBytes> {
public:
    using Base = MockInstrumentBase<HyperspectralImagerMock<FrameBytes>, FrameBytes>;

    explicit HyperspectralImagerMock(uint64_t manifest_hash = 0ULL, uint16_t inst_id = 1U,
                                     uint64_t rng_seed = 0x4853'4930'0001ULL) noexcept
        : Base(make_default_config(manifest_hash, inst_id, rng_seed)) {}

    static typename Base::Config make_default_config(uint64_t manifest_hash, uint16_t inst_id,
                                                     uint64_t rng_seed) noexcept {
        typename Base::Config c{};
        c.instrument_id                = inst_id;
        c.manifest_hash                = manifest_hash;
        c.rng_seed                     = rng_seed;
        c.default_integration_us       = 4000U;
        c.frame_cadence_ticks          = 1U;
        c.thermal.thermal_mass_j_per_k = 38.0f;
        c.thermal.conductance_w_per_k  = 0.36f;
        c.thermal.sink_temp_k          = 215.0f;
        c.thermal.initial_temp_k       = 265.0f;
        c.thermal.heater_max_w         = 14.0f;
        c.thermal.science_power_w      = 15.0f;
        c.thermal.over_temp_limit_k    = 310.0f;
        c.thermal.under_temp_limit_k   = 200.0f;
        return c;
    }

    void synthesize_science_payload(std::span<std::byte> payload, uint32_t frame_seq,
                                    ScenePattern scene, DeterministicRng& rng) noexcept {
        const size_t  pixel_count = payload.size() / sizeof(uint16_t);
        const float   temp_k      = this->thermal_model().temp_k();
        // Dark current rises exponentially-ish with detector temperature above 250 K
        const float   excess_k    = (temp_k > 250.0f) ? (temp_k - 250.0f) : 0.0f;
        const int32_t dark_dn     = static_cast<int32_t>(180.0f + excess_k * 12.0f);
        const float   exp_scale   = static_cast<float>(this->integration_us()) / 4000.0f;
        const int32_t g           = static_cast<int32_t>(this->gain());

        for (size_t i = 0U; i < pixel_count; ++i) {
            int32_t signal = dark_dn;
            switch (scene) {
                case ScenePattern::DarkFrame:
                    break;
                case ScenePattern::FlatField:
                    signal += static_cast<int32_t>(4000.0f * exp_scale);
                    break;
                case ScenePattern::CalibrationLamp:
                    // Sharp spectral emission lines every 16th band
                    signal += ((i % 16U) == 4U || (i % 16U) == 11U)
                                  ? static_cast<int32_t>(22000.0f * exp_scale)
                                  : static_cast<int32_t>(1200.0f * exp_scale);
                    break;
                case ScenePattern::PointTarget: {
                    const size_t center = (pixel_count / 2U + (frame_seq % 16U)) % pixel_count;
                    const int32_t dist  = static_cast<int32_t>(i) - static_cast<int32_t>(center);
                    const float   psf   = 28000.0f * std::exp(-static_cast<float>(dist * dist) / 8.0f);
                    signal += static_cast<int32_t>(psf * exp_scale);
                    break;
                }
                case ScenePattern::ThermalGradient:
                    signal += static_cast<int32_t>((static_cast<float>(i) / static_cast<float>(pixel_count)) *
                                                   18000.0f * exp_scale);
                    break;
                case ScenePattern::CosmicRayBurst:
                    signal += static_cast<int32_t>(3000.0f * exp_scale);
                    if ((rng.next_u16() & 0x1FU) == 0U) {
                        signal += 45000;
                    }
                    break;
            }
            signal = signal * g + rng.signed_noise(24);
            const uint16_t dn =
                static_cast<uint16_t>(std::clamp<int32_t>(signal, 0, 65535));
            std::memcpy(payload.data() + i * sizeof(uint16_t), &dn, sizeof(uint16_t));
        }
    }
};

/// Reference Mock Instrument 2: Tri-Axial Fluxgate Boom Magnetometer.
/// Synthesizes packed 3-axis int32 nano-Tesla samples (Bx, By, Bz) following an orbital
/// dipole field model plus calibration coil pulses and deterministic noise.
struct MagSample {
    int32_t bx_nt{0};
    int32_t by_nt{0};
    int32_t bz_nt{0};
};
static_assert(sizeof(MagSample) == 12U);

template <size_t FrameBytes = 128U>
class FluxgateMagnetometerMock
    : public MockInstrumentBase<FluxgateMagnetometerMock<FrameBytes>, FrameBytes> {
public:
    using Base = MockInstrumentBase<FluxgateMagnetometerMock<FrameBytes>, FrameBytes>;

    explicit FluxgateMagnetometerMock(uint64_t manifest_hash = 0ULL, uint16_t inst_id = 2U,
                                      uint64_t rng_seed = 0x4D41'4730'0002ULL) noexcept
        : Base(make_default_config(manifest_hash, inst_id, rng_seed)) {}

    static typename Base::Config make_default_config(uint64_t manifest_hash, uint16_t inst_id,
                                                     uint64_t rng_seed) noexcept {
        typename Base::Config c{};
        c.instrument_id                = inst_id;
        c.manifest_hash                = manifest_hash;
        c.rng_seed                     = rng_seed;
        c.default_integration_us       = 10000U;
        c.frame_cadence_ticks          = 1U;
        c.thermal.thermal_mass_j_per_k = 18.0f;
        c.thermal.conductance_w_per_k  = 0.20f;
        c.thermal.sink_temp_k          = 230.0f;
        c.thermal.initial_temp_k       = 255.0f;
        c.thermal.heater_max_w         = 6.0f;
        c.thermal.science_power_w      = 3.2f;
        c.thermal.standby_power_w      = 1.4f;
        c.thermal.over_temp_limit_k    = 320.0f;
        c.thermal.under_temp_limit_k   = 190.0f;
        return c;
    }

    void synthesize_science_payload(std::span<std::byte> payload, uint32_t frame_seq,
                                    ScenePattern scene, DeterministicRng& rng) noexcept {
        const size_t n_samples = payload.size() / sizeof(MagSample);
        for (size_t i = 0U; i < n_samples; ++i) {
            const float phase = static_cast<float>(frame_seq * n_samples + i) * 0.05f;
            int32_t     bx    = static_cast<int32_t>(22000.0f * std::cos(phase));
            int32_t     by    = static_cast<int32_t>(14500.0f * std::sin(phase));
            int32_t     bz    = static_cast<int32_t>(31000.0f * std::cos(phase * 0.5f));
            if (scene == ScenePattern::CalibrationLamp) {
                // Known 5000 nT precision calibration coil pulse on +Z
                bz += 5000;
            } else if (scene == ScenePattern::CosmicRayBurst) {
                // Transient spacecraft reaction-wheel magnetic interference spike
                bx += 8500;
            }
            const MagSample s{
                .bx_nt = bx + rng.signed_noise(12),
                .by_nt = by + rng.signed_noise(12),
                .bz_nt = bz + rng.signed_noise(12),
            };
            std::memcpy(payload.data() + i * sizeof(MagSample), &s, sizeof(MagSample));
        }
    }
};

} // namespace strata::instruments
