#pragma once

// =============================================================================
// STRATA-20 Reusable Mock Flight Instrument Framework (STRATA-SIM-001..004)
// Zero-heap, zero-vtable, freestanding C++20 CRTP framework providing:
//   - Instrument operational mode FSM (Off/Boot/Standby/Safe/Science/Cal/Fault)
//   - Deterministic 1st-order thermal & power physics ODE
//   - SCSR-compatible MMIO register bank satisfying strata::RegisterTransport
//   - Deterministic seeded PRNG & synthetic scene/waveform generator
//   - Time-scheduled & on-demand fault injection (ThermalRunaway, SEU, CRC, DMA, WDT)
//   - Zero-copy FramePool science frame producer with CRC-32C trailer
// =============================================================================

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/data/frame_lease.hpp>
#include <strata/hw/reg_map.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::instruments {

/// Operational mode of a spacecraft instrument.
enum class InstrumentMode : uint8_t {
    Off         = 0U,
    Boot        = 1U,
    Standby     = 2U,
    Safe        = 3U,
    Science     = 4U,
    Calibration = 5U,
    Fault       = 6U,
};

/// Selectable synthetic stimulus patterns for deterministic science frame generation.
enum class ScenePattern : uint8_t {
    DarkFrame       = 0U,
    FlatField       = 1U,
    CalibrationLamp = 2U,
    PointTarget     = 3U,
    ThermalGradient = 4U,
    CosmicRayBurst  = 5U,
};

/// Fault/anomaly modes injectable on demand or via deterministic mission timeline.
enum class InstrumentFaultKind : uint8_t {
    None               = 0U,
    ThermalRunaway     = 1U, // Excess thermal dissipation (param_f32 = extra Watts)
    RegisterSeuBitFlip = 2U, // Single-Event Upset bit flip (param_u32 = (reg_offset<<8)|bit)
    FrameCrcCorrupt    = 3U, // Corrupt CRC trailer on next `duration_ticks` frames
    DmaStall           = 4U, // Halt DMA frame emission for `duration_ticks`
    WatchdogFreeze     = 5U, // Freeze heartbeat counter for `duration_ticks`
    PowerBrownout      = 6U, // Drop bus voltage below nominal threshold
    OverCurrent        = 7U, // Spike bus current draw (param_f32 = extra Amps)
    SensorSaturation   = 8U, // Pin detector output to full-scale 0xFFFF
};

/// Standard instrument command opcodes handled uniformly by MockInstrumentBase.
namespace cmd {
inline constexpr uint16_t kNoop           = 0x0001U;
inline constexpr uint16_t kSetMode        = 0x0100U;
inline constexpr uint16_t kSetIntegration = 0x0101U;
inline constexpr uint16_t kSetHeaterPwm   = 0x0102U;
inline constexpr uint16_t kSetScene       = 0x0103U;
inline constexpr uint16_t kSetGain        = 0x0104U;
inline constexpr uint16_t kClearFaults    = 0x0105U;
inline constexpr uint16_t kInjectFault    = 0x0106U;
inline constexpr uint16_t kSelfTest       = 0x0107U;
} // namespace cmd

/// Standard SCSR-compatible MMIO register offsets exposed by InstrumentRegisterBlock.
namespace reg {
inline constexpr uint32_t kMagic          = 0x00U; // RO: 0x5354494D ("STIM")
inline constexpr uint32_t kVersion        = 0x04U; // RO: 0x0001_0000
inline constexpr uint32_t kManifestHashLo = 0x08U; // RO
inline constexpr uint32_t kManifestHashHi = 0x0CU; // RO
inline constexpr uint32_t kCtrl           = 0x10U; // RW: bit0=enable, bit1=reset, bit2=trig_sbit
inline constexpr uint32_t kStatus         = 0x14U; // RO: bit0=ready, bit1=sbit_pass, bit2=fault, bit3=dma_busy
inline constexpr uint32_t kMode           = 0x18U; // RW: InstrumentMode
inline constexpr uint32_t kIntTimeUs      = 0x1CU; // RW: integration/cadence time (us)
inline constexpr uint32_t kHeaterPwm      = 0x20U; // RW: 0..1000 (0.0%..100.0%)
inline constexpr uint32_t kTempMilliK     = 0x24U; // RO: detector temperature in milli-Kelvin
inline constexpr uint32_t kCurrentMilliA  = 0x28U; // RO: bus current draw in mA
inline constexpr uint32_t kBusVoltageMv   = 0x2CU; // RO: bus voltage in mV
inline constexpr uint32_t kFrameCount     = 0x30U; // RO: monotonic frames produced
inline constexpr uint32_t kFaultFlags     = 0x34U; // RW1C: latched fault bitmask
inline constexpr uint32_t kHeartbeat      = 0x38U; // RO: increments every healthy tick
inline constexpr uint32_t kSceneMode      = 0x3CU; // RW: ScenePattern
inline constexpr uint32_t kGain           = 0x40U; // RW: digital gain x1..x16
inline constexpr uint32_t kCrcErrorCount  = 0x44U; // RO: injected/detected frame CRC errors
} // namespace reg

/// Latched hardware fault flag bits in reg::kFaultFlags (RW1C).
namespace fault_bit {
inline constexpr uint32_t kOverTemp        = 1U << 0U;
inline constexpr uint32_t kUnderTemp       = 1U << 1U;
inline constexpr uint32_t kPowerBrownout   = 1U << 2U;
inline constexpr uint32_t kOverCurrent     = 1U << 3U;
inline constexpr uint32_t kDmaStall        = 1U << 4U;
inline constexpr uint32_t kRegisterSeu     = 1U << 5U;
inline constexpr uint32_t kFrameCrcCorrupt = 1U << 6U;
inline constexpr uint32_t kWatchdogFreeze  = 1U << 7U;
} // namespace fault_bit

/// Deterministic zero-heap XorShift64* PRNG for reproducible radiometric/noise simulation.
class DeterministicRng {
public:
    constexpr explicit DeterministicRng(uint64_t seed = 0x57A7'A202'6001ULL) noexcept
        : state_(seed == 0ULL ? 0x9E37'79B9'7F4A'7C15ULL : seed) {}

    constexpr void reseed(uint64_t seed) noexcept {
        state_ = (seed == 0ULL) ? 0x9E37'79B9'7F4A'7C15ULL : seed;
    }

    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        uint64_t x = state_;
        x ^= x >> 12U;
        x ^= x << 25U;
        x ^= x >> 27U;
        state_ = x;
        return x * 0x2545'F491'4F6C'DD1DULL;
    }

    [[nodiscard]] constexpr uint16_t next_u16() noexcept {
        return static_cast<uint16_t>((next_u64() >> 48U) & 0xFFFFU);
    }

    /// Deterministic zero-mean triangular noise in [-amplitude, +amplitude].
    [[nodiscard]] constexpr int32_t signed_noise(int32_t amplitude) noexcept {
        if (amplitude <= 0) return 0;
        const uint32_t span = static_cast<uint32_t>(amplitude) + 1U;
        const int32_t  a    = static_cast<int32_t>(next_u64() % span);
        const int32_t  b    = static_cast<int32_t>(next_u64() % span);
        return a - b;
    }

private:
    uint64_t state_;
};

/// Physical configuration for the 1st-order thermal and electrical power model.
struct ThermalPowerConfig {
    float thermal_mass_j_per_k{45.0f};      // Detector + bench thermal capacitance (J/K)
    float conductance_w_per_k{0.35f};       // Effective thermal conductance to radiator sink (W/K)
    float sink_temp_k{220.0f};              // Cold radiator / deep-space sink temperature (K)
    float initial_temp_k{265.0f};           // Starting temperature (K)
    float heater_max_w{18.0f};              // Heater power at 100% PWM (W)
    float nominal_bus_v{28.0f};             // Nominal spacecraft primary power bus (V)
    float standby_power_w{4.5f};            // Standby electronics power (W)
    float safe_power_w{2.5f};               // Safe mode keep-alive power (W)
    float science_power_w{14.0f};           // Full science electronics + FPGA power (W)
    float cal_power_w{16.5f};               // Science + calibration lamp power (W)
    float over_temp_limit_k{310.0f};        // Hardware over-temp fault latch threshold (K)
    float under_temp_limit_k{200.0f};       // Hardware under-temp fault latch threshold (K)
    float max_current_a{2.5f};              // Over-current fault latch threshold (A)
};

/// Deterministic 1st-order thermal & power physics engine.
class ThermalPowerModel {
public:
    explicit ThermalPowerModel(const ThermalPowerConfig& cfg = {}) noexcept
        : cfg_(cfg), temp_k_(cfg.initial_temp_k), bus_v_(cfg.nominal_bus_v) {}

    void reset() noexcept {
        temp_k_     = cfg_.initial_temp_k;
        bus_v_      = cfg_.nominal_bus_v;
        current_a_  = 0.0f;
        total_w_    = 0.0f;
    }

    void step(InstrumentMode mode, uint16_t heater_pwm_permille, float dt_s,
              float fault_extra_heat_w, bool brownout_active, float fault_extra_current_a) noexcept {
        const float clamped_pwm =
            (heater_pwm_permille > 1000U) ? 1.0f : (static_cast<float>(heater_pwm_permille) / 1000.0f);

        float elec_w = 0.0f;
        switch (mode) {
            case InstrumentMode::Off:         elec_w = 0.0f; break;
            case InstrumentMode::Boot:        elec_w = cfg_.standby_power_w * 1.2f; break;
            case InstrumentMode::Standby:     elec_w = cfg_.standby_power_w; break;
            case InstrumentMode::Safe:        elec_w = cfg_.safe_power_w; break;
            case InstrumentMode::Science:     elec_w = cfg_.science_power_w; break;
            case InstrumentMode::Calibration: elec_w = cfg_.cal_power_w; break;
            case InstrumentMode::Fault:       elec_w = cfg_.safe_power_w; break;
        }

        const float heater_w = (mode == InstrumentMode::Off) ? 0.0f : (clamped_pwm * cfg_.heater_max_w);
        const float heat_in  = elec_w + heater_w + fault_extra_heat_w;
        const float heat_out = cfg_.conductance_w_per_k * (temp_k_ - cfg_.sink_temp_k);
        const float dtemp    = ((heat_in - heat_out) / cfg_.thermal_mass_j_per_k) * dt_s;
        temp_k_ += dtemp;
        if (temp_k_ < 4.0f) temp_k_ = 4.0f;
        if (temp_k_ > 500.0f) temp_k_ = 500.0f;

        bus_v_     = brownout_active ? (cfg_.nominal_bus_v * 0.68f) : cfg_.nominal_bus_v;
        total_w_   = elec_w + heater_w;
        current_a_ = (bus_v_ > 1.0f ? (total_w_ / bus_v_) : 0.0f) + fault_extra_current_a;
    }

    [[nodiscard]] float temp_k() const noexcept { return temp_k_; }
    [[nodiscard]] float bus_v() const noexcept { return bus_v_; }
    [[nodiscard]] float current_a() const noexcept { return current_a_; }
    [[nodiscard]] float total_power_w() const noexcept { return total_w_; }
    [[nodiscard]] const ThermalPowerConfig& config() const noexcept { return cfg_; }

private:
    ThermalPowerConfig cfg_;
    float              temp_k_{265.0f};
    float              bus_v_{28.0f};
    float              current_a_{0.0f};
    float              total_w_{0.0f};
};

/// Scheduled or immediate fault descriptor.
struct FaultEvent {
    InstrumentFaultKind kind{InstrumentFaultKind::None};
    uint32_t            trigger_tick{0U};
    uint32_t            duration_ticks{1U};
    uint32_t            remaining_ticks{0U};
    uint32_t            param_u32{0U};
    float               param_f32{0.0f};
    bool                armed{false};
    bool                active{false};
};

/// Zero-heap deterministic fault schedule & active anomaly engine.
template <size_t MaxFaults = 16U>
class FaultInjector {
public:
    [[nodiscard]] Status schedule(InstrumentFaultKind kind, uint32_t trigger_tick,
                                  uint32_t duration_ticks = 1U, uint32_t param_u32 = 0U,
                                  float param_f32 = 0.0f) noexcept {
        for (auto& f : faults_) {
            if (!f.armed && !f.active) {
                f = FaultEvent{
                    .kind            = kind,
                    .trigger_tick    = trigger_tick,
                    .duration_ticks  = (duration_ticks == 0U) ? 1U : duration_ticks,
                    .remaining_ticks = 0U,
                    .param_u32       = param_u32,
                    .param_f32       = param_f32,
                    .armed           = true,
                    .active          = false,
                };
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] Status inject_now(InstrumentFaultKind kind, uint32_t current_tick,
                                    uint32_t duration_ticks = 1U, uint32_t param_u32 = 0U,
                                    float param_f32 = 0.0f) noexcept {
        return schedule(kind, current_tick, duration_ticks, param_u32, param_f32);
    }

    void clear_all() noexcept {
        for (auto& f : faults_) {
            f = FaultEvent{};
        }
    }

    void clear_kind(InstrumentFaultKind kind) noexcept {
        for (auto& f : faults_) {
            if (f.kind == kind) {
                f = FaultEvent{};
            }
        }
    }

    void advance_tick(uint32_t current_tick) noexcept {
        for (auto& f : faults_) {
            if (f.active) {
                if (f.remaining_ticks > 0U) {
                    --f.remaining_ticks;
                }
                if (f.remaining_ticks == 0U) {
                    f.active = false;
                }
            }
            if (f.armed && current_tick >= f.trigger_tick) {
                f.armed           = false;
                f.active          = true;
                f.remaining_ticks = f.duration_ticks;
            }
        }
    }

    [[nodiscard]] bool is_active(InstrumentFaultKind kind) const noexcept {
        for (const auto& f : faults_) {
            if (f.active && f.kind == kind) return true;
        }
        return false;
    }

    [[nodiscard]] float sum_active_f32(InstrumentFaultKind kind) const noexcept {
        float acc = 0.0f;
        for (const auto& f : faults_) {
            if (f.active && f.kind == kind) acc += f.param_f32;
        }
        return acc;
    }

    /// Consume any newly active one-shot RegisterSeuBitFlip param_u32 (returns true if one fired).
    [[nodiscard]] bool consume_seu_param(uint32_t& out_param) noexcept {
        for (auto& f : faults_) {
            if (f.active && f.kind == InstrumentFaultKind::RegisterSeuBitFlip) {
                out_param         = f.param_u32;
                f.active          = false;
                f.remaining_ticks = 0U;
                return true;
            }
        }
        return false;
    }

private:
    std::array<FaultEvent, MaxFaults> faults_{};
};

/// SCSR-compatible MMIO register bank for any mock instrument (satisfies strata::RegisterTransport).
class InstrumentRegisterBlock {
public:
    static constexpr size_t kSizeBytes = 256U;
    static constexpr size_t kNumWords  = kSizeBytes / 4U;

    explicit InstrumentRegisterBlock(uint64_t manifest_hash = 0ULL) noexcept {
        reset(manifest_hash);
    }

    void reset(uint64_t manifest_hash) noexcept {
        words_.fill(0U);
        rw1c_mask_.fill(false);
        ro_mask_.fill(false);

        mark_ro(reg::kMagic);
        mark_ro(reg::kVersion);
        mark_ro(reg::kManifestHashLo);
        mark_ro(reg::kManifestHashHi);
        mark_ro(reg::kStatus);
        mark_ro(reg::kTempMilliK);
        mark_ro(reg::kCurrentMilliA);
        mark_ro(reg::kBusVoltageMv);
        mark_ro(reg::kFrameCount);
        mark_ro(reg::kHeartbeat);
        mark_ro(reg::kCrcErrorCount);
        mark_rw1c(reg::kFaultFlags);

        force_word(reg::kMagic, 0x5354'494DU); // "STIM"
        force_word(reg::kVersion, 0x0001'0000U);
        force_word(reg::kManifestHashLo, static_cast<uint32_t>(manifest_hash & 0xFFFF'FFFFULL));
        force_word(reg::kManifestHashHi, static_cast<uint32_t>((manifest_hash >> 32U) & 0xFFFF'FFFFULL));
        force_word(reg::kGain, 1U);
    }

    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept {
        const size_t idx = offset / 4U;
        return (idx < kNumWords) ? words_[idx] : 0U;
    }

    void write32(uint32_t offset, uint32_t value) noexcept {
        const size_t idx = offset / 4U;
        if (idx >= kNumWords || ro_mask_[idx]) {
            return;
        }
        if (rw1c_mask_[idx]) {
            words_[idx] &= ~value;
        } else {
            words_[idx] = value;
        }
        ++write_count_;
    }

    void force_word(uint32_t offset, uint32_t value) noexcept {
        const size_t idx = offset / 4U;
        if (idx < kNumWords) words_[idx] = value;
    }

    void latch_fault_bits(uint32_t bits) noexcept {
        words_[reg::kFaultFlags / 4U] |= bits;
    }

    /// Hardware backdoor SEU injection: flips bit `bit_pos` in register at `offset`.
    void flip_bit_seu(uint32_t offset, uint8_t bit_pos) noexcept {
        const size_t idx = offset / 4U;
        if (idx < kNumWords && bit_pos < 32U) {
            words_[idx] ^= (1U << bit_pos);
            latch_fault_bits(fault_bit::kRegisterSeu);
        }
    }

    [[nodiscard]] uint32_t write_count() const noexcept { return write_count_; }

private:
    void mark_ro(uint32_t offset) noexcept { ro_mask_[offset / 4U] = true; }
    void mark_rw1c(uint32_t offset) noexcept { rw1c_mask_[offset / 4U] = true; }

    std::array<uint32_t, kNumWords> words_{};
    std::array<bool, kNumWords>     rw1c_mask_{};
    std::array<bool, kNumWords>     ro_mask_{};
    uint32_t                        write_count_{0U};
};
static_assert(RegisterTransport<InstrumentRegisterBlock>);

/// Packed housekeeping telemetry snapshot produced by every mock instrument.
struct InstrumentHousekeeping {
    uint16_t       instrument_id{0U};
    InstrumentMode mode{InstrumentMode::Off};
    ScenePattern   scene{ScenePattern::DarkFrame};
    uint32_t       tick_count{0U};
    uint32_t       frame_count{0U};
    uint32_t       crc_error_count{0U};
    uint32_t       heartbeat{0U};
    uint32_t       fault_flags{0U};
    uint32_t       integration_us{2000U};
    uint16_t       heater_pwm_permille{0U};
    uint16_t       gain{1U};
    float          detector_temp_k{265.0f};
    float          bus_voltage_v{28.0f};
    float          bus_current_a{0.0f};
    uint64_t       last_tai_ns{0ULL};
    bool           sbit_passed{true};
};

/// Header written at the start of every synthesized zero-copy science frame (24 bytes).
struct ScienceFrameHeader {
    static constexpr uint32_t kMagic = 0x5346'524DU; // "SFRM"
    uint32_t                  magic{kMagic};
    uint16_t                  instrument_id{0U};
    uint8_t                   mode{0U};
    uint8_t                   scene{0U};
    uint32_t                  frame_seq{0U};
    uint32_t                  integration_us{0U};
    uint64_t                  tai_ns{0ULL};
};
static_assert(sizeof(ScienceFrameHeader) == 24U);

/// ============================================================================
/// CRTP Base Class for All StrataForge Mock Flight Instruments (STRATA-SIM-001)
/// Subclasses only need to implement:
///   void synthesize_science_payload(std::span<std::byte> payload, uint32_t frame_seq,
///                                   ScenePattern scene, DeterministicRng& rng) noexcept;
/// ============================================================================
template <typename Derived, size_t FrameBytes = 256U, size_t MaxFaults = 16U>
    requires(FrameBytes >= sizeof(ScienceFrameHeader) + sizeof(uint32_t) + 16U)
class MockInstrumentBase {
public:
    static constexpr size_t kFrameBytes   = FrameBytes;
    static constexpr size_t kHeaderBytes  = sizeof(ScienceFrameHeader);
    static constexpr size_t kTrailerBytes = sizeof(uint32_t); // CRC-32C
    static constexpr size_t kPayloadBytes = FrameBytes - kHeaderBytes - kTrailerBytes;

    struct Config {
        uint16_t           instrument_id{1U};
        uint64_t           manifest_hash{0ULL};
        uint64_t           rng_seed{0x57A7'A001ULL};
        uint32_t           default_integration_us{2000U};
        uint32_t           frame_cadence_ticks{1U};
        ThermalPowerConfig thermal{};
    };

    explicit MockInstrumentBase(const Config& cfg) noexcept
        : cfg_(cfg),
          thermal_(cfg.thermal),
          regs_(cfg.manifest_hash),
          rng_(cfg.rng_seed),
          integration_us_(cfg.default_integration_us) {
        regs_.force_word(reg::kIntTimeUs, integration_us_);
        sync_registers();
    }

    /// Power on & boot the mock instrument into Standby mode.
    void power_on() noexcept {
        mode_ = InstrumentMode::Boot;
        sbit_passed_ = run_sbit();
        mode_ = sbit_passed_ ? InstrumentMode::Standby : InstrumentMode::Fault;
        regs_.force_word(reg::kMode, static_cast<uint32_t>(mode_));
        sync_registers();
    }

    [[nodiscard]] bool run_sbit() noexcept {
        sbit_passed_ = (regs_.read32(reg::kMagic) == 0x5354'494DU) &&
                       !faults_.is_active(InstrumentFaultKind::PowerBrownout);
        return sbit_passed_;
    }

    [[nodiscard]] Status set_mode(InstrumentMode next) noexcept {
        if (mode_ == InstrumentMode::Off && next != InstrumentMode::Boot &&
            next != InstrumentMode::Standby && next != InstrumentMode::Off) {
            return Status::PreconditionFailed;
        }
        mode_ = next;
        regs_.force_word(reg::kMode, static_cast<uint32_t>(mode_));
        sync_registers();
        return Status::Ok;
    }

    void set_integration_us(uint32_t us) noexcept {
        integration_us_ = (us < 100U) ? 100U : ((us > 1'000'000U) ? 1'000'000U : us);
        regs_.force_word(reg::kIntTimeUs, integration_us_);
    }

    void set_heater_pwm(uint16_t permille) noexcept {
        heater_pwm_permille_ = (permille > 1000U) ? 1000U : permille;
        regs_.force_word(reg::kHeaterPwm, heater_pwm_permille_);
    }

    void set_scene(ScenePattern scene) noexcept {
        scene_ = scene;
        regs_.force_word(reg::kSceneMode, static_cast<uint32_t>(scene_));
    }

    void set_gain(uint16_t gain) noexcept {
        gain_ = (gain == 0U) ? 1U : ((gain > 16U) ? 16U : gain);
        regs_.force_word(reg::kGain, gain_);
    }

    /// Handle standard instrument commands (used by CommandDispatcher / PUS-C / GroundTestSession).
    [[nodiscard]] Status handle_command(uint16_t opcode, uint64_t arg) noexcept {
        switch (opcode) {
            case cmd::kNoop:
                return Status::Ok;
            case cmd::kSetMode:
                if (arg > static_cast<uint64_t>(InstrumentMode::Fault)) return Status::InvalidArg;
                return set_mode(static_cast<InstrumentMode>(arg));
            case cmd::kSetIntegration:
                set_integration_us(static_cast<uint32_t>(arg));
                return Status::Ok;
            case cmd::kSetHeaterPwm:
                set_heater_pwm(static_cast<uint16_t>(arg));
                return Status::Ok;
            case cmd::kSetScene:
                if (arg > static_cast<uint64_t>(ScenePattern::CosmicRayBurst)) return Status::InvalidArg;
                set_scene(static_cast<ScenePattern>(arg));
                return Status::Ok;
            case cmd::kSetGain:
                set_gain(static_cast<uint16_t>(arg));
                return Status::Ok;
            case cmd::kClearFaults:
                faults_.clear_all();
                regs_.write32(reg::kFaultFlags, 0xFFFF'FFFFU);
                if (mode_ == InstrumentMode::Fault) {
                    mode_ = InstrumentMode::Standby;
                    regs_.force_word(reg::kMode, static_cast<uint32_t>(mode_));
                }
                sync_registers();
                return Status::Ok;
            case cmd::kInjectFault: {
                // Packed arg: [bit0..7: fault_kind][bit8..23: duration_ticks][bit24..39: magnitude]
                const auto kind     = static_cast<InstrumentFaultKind>(arg & 0xFFU);
                const auto duration = static_cast<uint32_t>((arg >> 8U) & 0xFFFFU);
                const auto mag      = static_cast<uint32_t>((arg >> 24U) & 0xFFFFU);
                return faults_.inject_now(kind, tick_count_, duration == 0U ? 10U : duration,
                                          mag, static_cast<float>(mag));
            }
            case cmd::kSelfTest:
                return run_sbit() ? Status::Ok : Status::ValidationFailed;
            default:
                return Status::InvalidArg;
        }
    }

    /// Advance physics, fault schedule, and register mirror by one simulation tick.
    /// Returns true if a new science frame is ready to be emitted on this tick.
    bool step(TaiTimestamp now, float dt_s = 0.1f) noexcept {
        last_tai_ns_ = now.ns();
        ++tick_count_;

        // Pick up any host/driver writes to RW SCSR registers
        pull_mmio_controls();

        // Advance scheduled/active faults
        faults_.advance_tick(tick_count_);

        // Apply one-shot Register SEU bit flips
        uint32_t seu_param = 0U;
        if (faults_.consume_seu_param(seu_param)) {
            const uint32_t reg_off = ((seu_param >> 8U) & 0xFFU) & ~0x3U;
            const uint8_t  bit_pos = static_cast<uint8_t>(seu_param & 0x1FU);
            regs_.flip_bit_seu(reg_off, bit_pos);
        }

        const float extra_heat_w = faults_.sum_active_f32(InstrumentFaultKind::ThermalRunaway);
        const bool  brownout     = faults_.is_active(InstrumentFaultKind::PowerBrownout);
        const float extra_curr_a = faults_.sum_active_f32(InstrumentFaultKind::OverCurrent);

        thermal_.step(mode_, heater_pwm_permille_, dt_s, extra_heat_w, brownout, extra_curr_a);

        // Latch hardware fault flags
        if (thermal_.temp_k() >= thermal_.config().over_temp_limit_k) {
            regs_.latch_fault_bits(fault_bit::kOverTemp);
        }
        if (thermal_.temp_k() <= thermal_.config().under_temp_limit_k) {
            regs_.latch_fault_bits(fault_bit::kUnderTemp);
        }
        if (brownout) {
            regs_.latch_fault_bits(fault_bit::kPowerBrownout);
        }
        if (thermal_.current_a() >= thermal_.config().max_current_a) {
            regs_.latch_fault_bits(fault_bit::kOverCurrent);
        }
        if (faults_.is_active(InstrumentFaultKind::DmaStall)) {
            regs_.latch_fault_bits(fault_bit::kDmaStall);
        }
        if (faults_.is_active(InstrumentFaultKind::WatchdogFreeze)) {
            regs_.latch_fault_bits(fault_bit::kWatchdogFreeze);
        } else if (mode_ != InstrumentMode::Off) {
            ++heartbeat_;
        }

        const bool producing_mode =
            (mode_ == InstrumentMode::Science || mode_ == InstrumentMode::Calibration);
        const uint32_t cadence = (cfg_.frame_cadence_ticks == 0U) ? 1U : cfg_.frame_cadence_ticks;
        const bool frame_ready = producing_mode && !faults_.is_active(InstrumentFaultKind::DmaStall) &&
                                 ((tick_count_ % cadence) == 0U);

        sync_registers();
        return frame_ready;
    }

    /// Synthesize a complete science frame (Header + Payload + CRC-32C trailer) into caller buffer.
    [[nodiscard]] Status produce_frame(std::span<std::byte> out_buf, TaiTimestamp now) noexcept {
        if (out_buf.size() < kFrameBytes) {
            return Status::PayloadSizeError;
        }
        if (faults_.is_active(InstrumentFaultKind::DmaStall)) {
            return Status::Timeout;
        }

        const uint32_t seq = ++frame_count_;
        ScienceFrameHeader hdr{
            .magic          = ScienceFrameHeader::kMagic,
            .instrument_id  = cfg_.instrument_id,
            .mode           = static_cast<uint8_t>(mode_),
            .scene          = static_cast<uint8_t>(scene_),
            .frame_seq      = seq,
            .integration_us = integration_us_,
            .tai_ns         = now.ns(),
        };
        std::memcpy(out_buf.data(), &hdr, kHeaderBytes);

        auto payload = out_buf.subspan(kHeaderBytes, kPayloadBytes);
        if (faults_.is_active(InstrumentFaultKind::SensorSaturation)) {
            std::memset(payload.data(), 0xFF, payload.size());
        } else {
            static_cast<Derived*>(this)->synthesize_science_payload(payload, seq, scene_, rng_);
        }

        uint32_t crc = Crc32c::compute(out_buf.subspan(0U, kHeaderBytes + kPayloadBytes));
        if (faults_.is_active(InstrumentFaultKind::FrameCrcCorrupt)) {
            crc ^= 0xDEAD'BEEFU;
            ++crc_error_count_;
            regs_.latch_fault_bits(fault_bit::kFrameCrcCorrupt);
        }
        std::memcpy(out_buf.data() + kHeaderBytes + kPayloadBytes, &crc, kTrailerBytes);
        sync_registers();
        return Status::Ok;
    }

    /// Emit a synthesized science frame directly into a zero-copy FramePool slot.
    template <size_t SlotCount, size_t SlotBytes>
        requires(SlotBytes >= FrameBytes)
    [[nodiscard]] Expected<uint16_t, Status>
    produce_to_frame_pool(FramePool<SlotCount, SlotBytes>& pool, GenId producer_gen,
                          uint32_t subscriber_mask, TaiTimestamp now) noexcept {
        auto slot_res = pool.allocate_slot(producer_gen);
        if (!slot_res.has_value()) {
            return Unexpected(slot_res.error());
        }
        const uint16_t slot_idx = slot_res.value();
        auto           buf      = pool.writable_slot(slot_idx);
        const Status   st       = produce_frame(buf, now);
        if (st != Status::Ok) {
            pool.release_producer_slot(slot_idx);
            return Unexpected(st);
        }
        pool.transfer_to_subscribers(slot_idx, subscriber_mask, static_cast<uint32_t>(kFrameBytes), now);
        return slot_idx;
    }

    /// Verify CRC-32C trailer of a produced science frame.
    [[nodiscard]] static bool verify_frame_crc(std::span<const std::byte> frame) noexcept {
        if (frame.size() < kFrameBytes) return false;
        const uint32_t expected = Crc32c::compute(frame.subspan(0U, kHeaderBytes + kPayloadBytes));
        uint32_t       stored   = 0U;
        std::memcpy(&stored, frame.data() + kHeaderBytes + kPayloadBytes, kTrailerBytes);
        return expected == stored;
    }

    [[nodiscard]] InstrumentHousekeeping sample_telemetry() const noexcept {
        return InstrumentHousekeeping{
            .instrument_id       = cfg_.instrument_id,
            .mode                = mode_,
            .scene               = scene_,
            .tick_count          = tick_count_,
            .frame_count         = frame_count_,
            .crc_error_count     = crc_error_count_,
            .heartbeat           = heartbeat_,
            .fault_flags         = regs_.read32(reg::kFaultFlags),
            .integration_us      = integration_us_,
            .heater_pwm_permille = heater_pwm_permille_,
            .gain                = gain_,
            .detector_temp_k     = thermal_.temp_k(),
            .bus_voltage_v       = thermal_.bus_v(),
            .bus_current_a       = thermal_.current_a(),
            .last_tai_ns         = last_tai_ns_,
            .sbit_passed         = sbit_passed_,
        };
    }

    [[nodiscard]] InstrumentRegisterBlock& registers() noexcept { return regs_; }
    [[nodiscard]] const InstrumentRegisterBlock& registers() const noexcept { return regs_; }
    [[nodiscard]] FaultInjector<MaxFaults>& fault_injector() noexcept { return faults_; }
    [[nodiscard]] const ThermalPowerModel& thermal_model() const noexcept { return thermal_; }
    [[nodiscard]] InstrumentMode mode() const noexcept { return mode_; }
    [[nodiscard]] uint32_t tick_count() const noexcept { return tick_count_; }
    [[nodiscard]] uint32_t frame_count() const noexcept { return frame_count_; }
    [[nodiscard]] uint32_t integration_us() const noexcept { return integration_us_; }
    [[nodiscard]] uint16_t gain() const noexcept { return gain_; }

protected:
    void pull_mmio_controls() noexcept {
        const uint32_t ctrl = regs_.read32(reg::kCtrl);
        if ((ctrl & 0x02U) != 0U) { // Soft reset bit
            regs_.force_word(reg::kCtrl, ctrl & ~0x02U);
            faults_.clear_all();
            thermal_.reset();
            mode_ = InstrumentMode::Standby;
            regs_.force_word(reg::kMode, static_cast<uint32_t>(mode_));
        }
        if ((ctrl & 0x04U) != 0U) { // Trigger SBIT bit
            regs_.force_word(reg::kCtrl, ctrl & ~0x04U);
            (void)run_sbit();
        }
        const uint32_t reg_mode = regs_.read32(reg::kMode);
        if (reg_mode <= static_cast<uint32_t>(InstrumentMode::Fault)) {
            mode_ = static_cast<InstrumentMode>(reg_mode);
        }
        const uint32_t reg_int = regs_.read32(reg::kIntTimeUs);
        if (reg_int >= 100U && reg_int <= 1'000'000U) {
            integration_us_ = reg_int;
        }
        const uint32_t reg_pwm = regs_.read32(reg::kHeaterPwm);
        if (reg_pwm <= 1000U) {
            heater_pwm_permille_ = static_cast<uint16_t>(reg_pwm);
        }
        const uint32_t reg_scene = regs_.read32(reg::kSceneMode);
        if (reg_scene <= static_cast<uint32_t>(ScenePattern::CosmicRayBurst)) {
            scene_ = static_cast<ScenePattern>(reg_scene);
        }
        const uint32_t reg_gain = regs_.read32(reg::kGain);
        if (reg_gain >= 1U && reg_gain <= 16U) {
            gain_ = static_cast<uint16_t>(reg_gain);
        }
    }

    void sync_registers() noexcept {
        const uint32_t fault_flags = regs_.read32(reg::kFaultFlags);
        const uint32_t status =
            (mode_ != InstrumentMode::Off ? 1U : 0U) |
            (sbit_passed_ ? 2U : 0U) |
            (fault_flags != 0U ? 4U : 0U) |
            ((mode_ == InstrumentMode::Science || mode_ == InstrumentMode::Calibration) ? 8U : 0U);
        regs_.force_word(reg::kStatus, status);
        regs_.force_word(reg::kTempMilliK, static_cast<uint32_t>(thermal_.temp_k() * 1000.0f));
        regs_.force_word(reg::kCurrentMilliA, static_cast<uint32_t>(thermal_.current_a() * 1000.0f));
        regs_.force_word(reg::kBusVoltageMv, static_cast<uint32_t>(thermal_.bus_v() * 1000.0f));
        regs_.force_word(reg::kFrameCount, frame_count_);
        regs_.force_word(reg::kHeartbeat, heartbeat_);
        regs_.force_word(reg::kCrcErrorCount, crc_error_count_);
    }

    Config                   cfg_;
    ThermalPowerModel        thermal_;
    InstrumentRegisterBlock  regs_;
    DeterministicRng         rng_;
    FaultInjector<MaxFaults> faults_{};
    InstrumentMode           mode_{InstrumentMode::Off};
    ScenePattern             scene_{ScenePattern::FlatField};
    uint32_t                 tick_count_{0U};
    uint32_t                 frame_count_{0U};
    uint32_t                 crc_error_count_{0U};
    uint32_t                 heartbeat_{0U};
    uint32_t                 integration_us_{2000U};
    uint16_t                 heater_pwm_permille_{250U};
    uint16_t                 gain_{1U};
    uint64_t                 last_tai_ns_{0ULL};
    bool                     sbit_passed_{true};
};

} // namespace strata::instruments
