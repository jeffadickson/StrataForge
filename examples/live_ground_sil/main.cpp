// =============================================================================
// STRATA-20 Live Flight-to-Ground UDP SIL Bridge (STRATA-VER-007)
// Runs real zero-heap C++20 flight software + HyperspectralImagerMock + PUS-C +
// AES-256-GCM SDLS + FDIR + NvFaultLog over live loopback UDP connected to
// StrataGround (`TelemetryBroker`, `LimitMonitor`, `GroundTestSession`, Web Console).
// =============================================================================

#include "strata_mission_icd.hpp"

#include <strata/instruments/reference_instruments.hpp>
#include <strata/ipc/event_ring.hpp>
#include <strata/ipc/nv_fault_log.hpp>
#include <strata/services/comms/ccsds.hpp>
#include <strata/services/comms/pus_c.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace {

using namespace strata;
using namespace strata::instruments;
using namespace strata::services::comms;

// Static .bss flight allocations (zero heap, Class B <= 4096 B stack budget)
HyperspectralImagerMock<512U> g_hsi{
    strata::generated::kManifestHash, /*inst_id=*/1U, /*rng_seed=*/0x57A7'A202'6001ULL};
std::array<std::byte, 4U * 512U> g_frame_backing{};
FramePool<4U, 512U>              g_frame_pool{g_frame_backing, 0x0800'0000ULL};
NvFaultLogStorage<16U>           g_mram_storage{};
NvFaultLog<16U>                  g_nv_log{g_mram_storage};
PusCAdapter<16U, 16U>            g_pus_adapter{0x105U};
SdlsEngine<Aes256GcmSdlsBackend> g_sdls{};

constexpr uint16_t kChCdhUptimeMs       = 0x0001U;
constexpr uint16_t kChHsiDetectorTempK  = 0x0010U;
constexpr uint16_t kCmdNoop             = 0x0101U;
constexpr uint16_t kCmdHsiSetInt        = 0x0201U;
constexpr uint16_t kCmdSlotReloadDfx    = 0x0301U;

// PUS-C ST[08,1] custom function IDs for live I&T fault injection & control
constexpr uint16_t kFnInjectThermalRunaway = 0x8001U;
constexpr uint16_t kFnClearFaultsAndResume = 0x8002U;
constexpr uint16_t kFnStopSim              = 0x80FFU;

// Event IDs emitted over STEV + PUS-C ST[05]
constexpr uint16_t kEvrBootOk          = 0x0100U;
constexpr uint16_t kEvrCmdApplied      = 0x0101U;
constexpr uint16_t kEvrFdirOverTemp    = 0x0201U;
constexpr uint16_t kEvrFdirRecovered   = 0x0202U;

struct UdpLink {
    int         rx_fd{-1};
    int         tx_fd{-1};
    sockaddr_in tx_addr{};

    void send_raw(std::span<const std::byte> data) const noexcept {
        if (tx_fd < 0 || data.empty()) return;
        (void)::sendto(tx_fd, data.data(), data.size(), 0,
                       reinterpret_cast<const sockaddr*>(&tx_addr), sizeof(tx_addr));
    }

    void send_tlm(uint16_t channel_id, uint64_t raw_val) const noexcept {
        std::array<std::byte, 14U> pkt{};
        pkt[0] = std::byte{'S'};
        pkt[1] = std::byte{'T'};
        pkt[2] = std::byte{'T'};
        pkt[3] = std::byte{'M'};
        write_be16(std::span<std::byte, 2U>{pkt.data() + 4U, 2U}, channel_id);
        write_be32(std::span<std::byte, 4U>{pkt.data() + 6U, 4U},
                   static_cast<uint32_t>(raw_val >> 32U));
        write_be32(std::span<std::byte, 4U>{pkt.data() + 10U, 4U},
                   static_cast<uint32_t>(raw_val & 0xFFFF'FFFFULL));
        send_raw(pkt);
    }

    void send_evr(const EventRecord& ev) const noexcept {
        std::array<std::byte, 36U> pkt{};
        pkt[0] = std::byte{'S'};
        pkt[1] = std::byte{'T'};
        pkt[2] = std::byte{'E'};
        pkt[3] = std::byte{'V'};
        std::memcpy(pkt.data() + 4U, &ev, sizeof(ev));
        send_raw(pkt);
    }
};

uint32_t parse_u32_arg(const char* s, uint32_t fallback) noexcept {
    if (s == nullptr) return fallback;
    char*               end = nullptr;
    const unsigned long v   = std::strtoul(s, &end, 10);
    return (end != s) ? static_cast<uint32_t>(v) : fallback;
}

void emit_event(const UdpLink& link, uint32_t& ev_seq, EventSeverity sev, uint16_t ev_id,
                uint32_t arg0, uint32_t arg1, TaiTimestamp now) noexcept {
    ++ev_seq;
    const EventRecord ev{
        .first_tai_ns = now.ns(),
        .last_tai_ns  = now.ns(),
        .event_id     = ev_id,
        .arg0         = arg0,
        .arg1         = arg1,
        .repeat_count = 1U,
        .severity     = sev,
        .partition_id = 3U,
    };
    link.send_evr(ev);

    std::array<std::byte, 128U> pus_buf{};
    const auto n = g_pus_adapter.build_st05_event_report(pus_buf, ev, 1U, now);
    if (n.has_value() && n.value() > 0U) {
        link.send_raw(std::span<const std::byte>{pus_buf.data(), n.value()});
    }
}

void process_uplink_packet(std::span<const std::byte> pkt, const UdpLink& link, uint32_t& ev_seq,
                           TaiTimestamp now, bool& stop_requested) noexcept {
    if (pkt.size() < 6U) return;

    // 1. Check for PUS-C Space Packet TC (version=0, is_tc=1, has_sec_hdr=1 -> (b0 & 0xF8) == 0x18)
    const uint8_t b0 = std::to_integer<uint8_t>(pkt[0]);
    if ((b0 & 0xF8U) == 0x18U && pkt.size() >= 11U) {
        auto on_tm = [&](std::span<const std::byte> tm_bytes) noexcept {
            link.send_raw(tm_bytes);
        };
        auto on_func = [&](uint16_t func_id, uint32_t arg) noexcept -> CmdResult {
            if (func_id == kFnInjectThermalRunaway) {
                const uint32_t watts = (arg == 0U) ? 240U : arg;
                (void)g_hsi.fault_injector().inject_now(
                    InstrumentFaultKind::ThermalRunaway, g_hsi.tick_count(), 60U, watts,
                    static_cast<float>(watts));
                emit_event(link, ev_seq, EventSeverity::Command, kEvrCmdApplied, func_id, watts, now);
                return CmdResult::Ok;
            }
            if (func_id == kFnClearFaultsAndResume) {
                (void)g_hsi.handle_command(cmd::kClearFaults, 0U);
                (void)g_hsi.set_mode(InstrumentMode::Science);
                emit_event(link, ev_seq, EventSeverity::ActivityHi, kEvrFdirRecovered,
                           static_cast<uint32_t>(g_hsi.mode()), 0U, now);
                return CmdResult::Ok;
            }
            if (func_id == kFnStopSim) {
                stop_requested = true;
                return CmdResult::Ok;
            }
            return CmdResult::UnknownOpcode;
        };
        auto on_mem = [&](uint8_t, uint32_t, uint32_t, std::span<const std::byte>,
                          std::span<std::byte>, size_t& reply_len) noexcept -> CmdResult {
            reply_len = 0U;
            return CmdResult::Denied;
        };
        (void)g_pus_adapter.dispatch_tc(pkt, now, on_tm, on_func, on_mem);
        return;
    }

    // 2. Otherwise parse as CCSDS TC Transfer Frame (with CRC-16 FECF validation)
    const auto tc = parse_tc_frame(pkt);
    if (!tc.has_value() || tc.value().data_field.size() < 2U) {
        return;
    }
    const auto     df     = tc.value().data_field;
    const uint16_t opcode = read_be16(df.subspan<0, 2>());
    if (opcode == kCmdNoop) {
        emit_event(link, ev_seq, EventSeverity::Command, kEvrCmdApplied, opcode, 0U, now);
    } else if (opcode == kCmdHsiSetInt && df.size() >= 6U) {
        const uint32_t int_us = read_be32(df.subspan(2U).subspan<0, 4>());
        (void)g_hsi.handle_command(cmd::kSetIntegration, int_us);
        emit_event(link, ev_seq, EventSeverity::Command, kEvrCmdApplied, opcode, int_us, now);
    } else if (opcode == kCmdSlotReloadDfx && df.size() >= 10U) {
        const uint32_t slot_id = read_be32(df.subspan(2U).subspan<0, 4>());
        (void)g_hsi.handle_command(cmd::kClearFaults, 0U);
        (void)g_hsi.set_mode(InstrumentMode::Science);
        emit_event(link, ev_seq, EventSeverity::ActivityHi, kEvrFdirRecovered, opcode, slot_id, now);
    }
}

} // namespace

int main(int argc, char** argv) {
    uint16_t rx_port    = 9101U;
    uint16_t tx_port    = 9100U;
    uint32_t max_ticks  = 400U;
    uint32_t tick_ms    = 20U;

    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--rx-port") == 0) {
            rx_port = static_cast<uint16_t>(parse_u32_arg(argv[i + 1], rx_port));
        } else if (std::strcmp(argv[i], "--tx-port") == 0) {
            tx_port = static_cast<uint16_t>(parse_u32_arg(argv[i + 1], tx_port));
        } else if (std::strcmp(argv[i], "--ticks") == 0) {
            max_ticks = parse_u32_arg(argv[i + 1], max_ticks);
        } else if (std::strcmp(argv[i], "--tick-ms") == 0) {
            tick_ms = parse_u32_arg(argv[i + 1], tick_ms);
        }
    }

    UdpLink link{};
    link.rx_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    link.tx_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (link.rx_fd < 0 || link.tx_fd < 0) {
        std::fprintf(stderr, "ERROR: failed to create UDP sockets\n");
        return 1;
    }

    int reuse = 1;
    (void)::setsockopt(link.rx_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int flags = ::fcntl(link.rx_fd, F_GETFL, 0);
    (void)::fcntl(link.rx_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in rx_addr{};
    rx_addr.sin_family      = AF_INET;
    rx_addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    rx_addr.sin_port        = ::htons(rx_port);
    if (::bind(link.rx_fd, reinterpret_cast<const sockaddr*>(&rx_addr), sizeof(rx_addr)) != 0) {
        std::fprintf(stderr, "ERROR: failed to bind UDP rx port %u\n", static_cast<unsigned>(rx_port));
        return 1;
    }

    link.tx_addr.sin_family      = AF_INET;
    link.tx_addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    link.tx_addr.sin_port        = ::htons(tx_port);

    // Mount refresh-free NvFaultLog & initialize HyperspectralImagerMock in Science mode
    (void)g_nv_log.mount_on_boot(strata::generated::kManifestHash, ResetReason::PowerOnCold,
                                 TaiTimestamp{1'000'000ULL});
    g_hsi.power_on();
    (void)g_hsi.set_mode(InstrumentMode::Science);
    g_hsi.set_scene(ScenePattern::PointTarget);

    uint32_t ev_seq       = 0U;
    bool     fdir_tripped = false;
    bool     stop_req     = false;
    emit_event(link, ev_seq, EventSeverity::ActivityHi, kEvrBootOk, 1U, 0U, TaiTimestamp{1'000'000ULL});

    std::array<std::byte, 512U> rx_buf{};
    timespec                    sleep_ts{};
    sleep_ts.tv_sec  = static_cast<time_t>(tick_ms / 1000U);
    sleep_ts.tv_nsec = static_cast<long>((tick_ms % 1000U) * 1'000'000UL);

    for (uint32_t tick = 1U; tick <= max_ticks && !stop_req; ++tick) {
        const TaiTimestamp now{static_cast<uint64_t>(tick) * 10'000'000ULL};

        // Drain all pending inbound CCSDS TC / PUS-C commands from StrataGround
        for (uint32_t burst = 0U; burst < 16U; ++burst) {
            const ssize_t n = ::recvfrom(link.rx_fd, rx_buf.data(), rx_buf.size(), 0, nullptr, nullptr);
            if (n <= 0) break;
            process_uplink_packet(std::span<const std::byte>{rx_buf.data(), static_cast<size_t>(n)},
                                  link, ev_seq, now, stop_req);
        }

        // Advance Mock Instrument physics & produce zero-copy science frame
        if (g_hsi.step(now, 0.5f)) {
            const auto slot = g_hsi.produce_to_frame_pool(g_frame_pool, GenId{1U}, 0x01U, now);
            if (slot.has_value()) {
                g_frame_pool.release_subscriber_slot(slot.value(), 0U);
            }
        }

        const auto hk = g_hsi.sample_telemetry();

        // Autonomous Flight FDIR: if over-temp fault latches, safe instrument & persist NvFaultLog!
        if (!fdir_tripped && (hk.fault_flags & fault_bit::kOverTemp) != 0U) {
            fdir_tripped = true;
            g_hsi.fault_injector().clear_kind(InstrumentFaultKind::ThermalRunaway);
            g_hsi.set_heater_pwm(0U);
            (void)g_hsi.set_mode(InstrumentMode::Safe);
            (void)g_nv_log.append(NvRecordKind::FdirAction, 3U, kEvrFdirOverTemp, now,
                                  static_cast<uint64_t>(hk.detector_temp_k * 1000.0f),
                                  hk.fault_flags, hk.frame_count);
            emit_event(link, ev_seq, EventSeverity::WarningHi, kEvrFdirOverTemp,
                       static_cast<uint32_t>(hk.detector_temp_k * 1000.0f), hk.fault_flags, now);
        }

        // Downlink calibrated channels matching versal_dual_instrument.toml:
        //   CDH_UPTIME_MS       = 0x0001 (raw = ms)
        //   HSI_DETECTOR_TEMP_K = 0x0010 (eng = 180.0 + 0.05 * raw)
        link.send_tlm(kChCdhUptimeMs, static_cast<uint64_t>(tick) * tick_ms);
        const float    clamped_t = (hk.detector_temp_k < 180.0f) ? 180.0f : hk.detector_temp_k;
        const uint64_t raw_temp  = static_cast<uint64_t>((clamped_t - 180.0f) / 0.05f);
        link.send_tlm(kChHsiDetectorTempK, raw_temp);

        if (tick_ms > 0U) {
            (void)::nanosleep(&sleep_ts, nullptr);
        }
    }

    ::close(link.rx_fd);
    ::close(link.tx_fd);
    std::printf(
        "PASS: live_ground_sil completed | frames=%u | nv_log_records=%zu | final_temp_k=%.2f\n",
        g_hsi.frame_count(), g_nv_log.valid_entry_count(),
        static_cast<double>(g_hsi.sample_telemetry().detector_temp_k));
    return 0;
}
