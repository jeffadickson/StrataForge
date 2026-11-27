#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/event_ring.hpp>
#include <strata/services/comms/ccsds.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::comms {

/// ============================================================================
/// Limited ECSS-E-ST-70-41C (PUS-C) Wire-Protocol Adapter (STRATA-COM-011)
/// Zero-heap, zero-vtable, total-function PUS-C secondary header codec and
/// mission-essential service dispatcher mapped cleanly onto STRATA-20 native
/// services:
///   - ST[01] Request Verification (TM 1,1..1,8 gated by 4-bit TC ACK mask)
///   - ST[03] Housekeeping (TC 3,5 / 3,6 / 3,27 -> TM 3,25)
///   - ST[05] Event Reporting (TM 5,1..5,4 + TC 5,5 / 5,6 event filter)
///   - ST[06] Memory Management (TC 6,2 load / 6,5 dump -> TM 6,6 / 6,9 check -> TM 6,10)
///   - ST[08] Function Management (TC 8,1 perform function)
///   - ST[09] Time Management (TC 9,1 set rate -> TM 9,3 CUC TAI report)
///   - ST[11] Time-Based Scheduling (TC 11,1..11,4 enable/disable/reset/insert)
///   - ST[17] Connection Test (TC 17,1 Are-You-Alive -> TM 17,2)
/// ============================================================================

inline constexpr uint8_t kPusVersionC            = 2U;
inline constexpr size_t  kPusTcSecondaryHdrBytes = 5U;
inline constexpr size_t  kPusTmSecondaryHdrBytes = 15U;

/// 4-bit ECSS-E-ST-70-41C TC Verification ACK Flags.
inline constexpr uint8_t kPusAckAcceptance = 0x01U;
inline constexpr uint8_t kPusAckStart      = 0x02U;
inline constexpr uint8_t kPusAckProgress   = 0x04U;
inline constexpr uint8_t kPusAckCompletion = 0x08U;
inline constexpr uint8_t kPusAckAll        = 0x0FU;

/// Standard ECSS-E-ST-70-41C Service Type IDs supported in STRATA-20 profile.
enum class PusService : uint8_t {
    Verification = 1U,
    Housekeeping = 3U,
    Events       = 5U,
    Memory       = 6U,
    Functions    = 8U,
    Time         = 9U,
    Schedule     = 11U,
    Test         = 17U,
};

struct PusTcHeader {
    uint8_t  ack_flags{kPusAckAcceptance | kPusAckCompletion};
    uint8_t  service_type{0U};
    uint8_t  service_subtype{0U};
    uint16_t source_id{0U};
};

struct PusTcView {
    PusTcHeader                header{};
    std::span<const std::byte> app_data{};
};

struct PusTmHeader {
    uint8_t  time_ref_status{0U};
    uint8_t  service_type{0U};
    uint8_t  service_subtype{0U};
    uint16_t msg_type_counter{0U};
    uint16_t destination_id{0U};
    uint64_t cuc_tai_ns{0U};
};

struct PusTmView {
    PusTmHeader                header{};
    std::span<const std::byte> app_data{};
};

/// Total-function PUS-C TC Secondary Header Parser.
[[nodiscard]] constexpr Expected<PusTcView, WireErr>
parse_pus_tc(std::span<const std::byte> sp_payload) noexcept {
    if (sp_payload.size() < kPusTcSecondaryHdrBytes) {
        return Unexpected(WireErr::TruncatedHeader);
    }
    const uint8_t b0  = std::to_integer<uint8_t>(sp_payload[0]);
    const uint8_t ver = static_cast<uint8_t>((b0 >> 4U) & 0x0FU);
    if (ver != kPusVersionC) {
        return Unexpected(WireErr::InvalidVersion);
    }
    return PusTcView{
        .header =
            PusTcHeader{
                .ack_flags       = static_cast<uint8_t>(b0 & 0x0FU),
                .service_type    = std::to_integer<uint8_t>(sp_payload[1]),
                .service_subtype = std::to_integer<uint8_t>(sp_payload[2]),
                .source_id       = read_be16(sp_payload.subspan<3, 2>()),
            },
        .app_data = sp_payload.subspan(kPusTcSecondaryHdrBytes),
    };
}

/// Zero-copy PUS-C TC Secondary Header + Application Data Encoder.
[[nodiscard]] inline Expected<size_t, WireErr>
encode_pus_tc(std::span<std::byte> out, const PusTcHeader& hdr,
              std::span<const std::byte> app_data) noexcept {
    const size_t total = kPusTcSecondaryHdrBytes + app_data.size();
    if (out.size() < total) return Unexpected(WireErr::BufferTooSmall);

    out[0] = static_cast<std::byte>((kPusVersionC << 4U) | (hdr.ack_flags & 0x0FU));
    out[1] = static_cast<std::byte>(hdr.service_type);
    out[2] = static_cast<std::byte>(hdr.service_subtype);
    write_be16(out.subspan<3, 2>(), hdr.source_id);
    if (!app_data.empty()) {
        std::memcpy(out.data() + kPusTcSecondaryHdrBytes, app_data.data(), app_data.size());
    }
    return total;
}

/// Total-function PUS-C TM Secondary Header Parser.
[[nodiscard]] constexpr Expected<PusTmView, WireErr>
parse_pus_tm(std::span<const std::byte> sp_payload) noexcept {
    if (sp_payload.size() < kPusTmSecondaryHdrBytes) {
        return Unexpected(WireErr::TruncatedHeader);
    }
    const uint8_t b0  = std::to_integer<uint8_t>(sp_payload[0]);
    const uint8_t ver = static_cast<uint8_t>((b0 >> 4U) & 0x0FU);
    if (ver != kPusVersionC) {
        return Unexpected(WireErr::InvalidVersion);
    }
    const uint64_t hi = read_be32(sp_payload.subspan<7, 4>());
    const uint64_t lo = read_be32(sp_payload.subspan<11, 4>());
    return PusTmView{
        .header =
            PusTmHeader{
                .time_ref_status  = static_cast<uint8_t>(b0 & 0x0FU),
                .service_type     = std::to_integer<uint8_t>(sp_payload[1]),
                .service_subtype  = std::to_integer<uint8_t>(sp_payload[2]),
                .msg_type_counter = read_be16(sp_payload.subspan<3, 2>()),
                .destination_id   = read_be16(sp_payload.subspan<5, 2>()),
                .cuc_tai_ns       = (hi << 32U) | lo,
            },
        .app_data = sp_payload.subspan(kPusTmSecondaryHdrBytes),
    };
}

/// Zero-copy PUS-C TM Secondary Header + Application Data Encoder.
[[nodiscard]] inline Expected<size_t, WireErr>
encode_pus_tm(std::span<std::byte> out, const PusTmHeader& hdr,
              std::span<const std::byte> app_data) noexcept {
    const size_t total = kPusTmSecondaryHdrBytes + app_data.size();
    if (out.size() < total) return Unexpected(WireErr::BufferTooSmall);

    out[0] = static_cast<std::byte>((kPusVersionC << 4U) | (hdr.time_ref_status & 0x0FU));
    out[1] = static_cast<std::byte>(hdr.service_type);
    out[2] = static_cast<std::byte>(hdr.service_subtype);
    write_be16(out.subspan<3, 2>(), hdr.msg_type_counter);
    write_be16(out.subspan<5, 2>(), hdr.destination_id);
    write_be32(out.subspan<7, 4>(), static_cast<uint32_t>(hdr.cuc_tai_ns >> 32U));
    write_be32(out.subspan<11, 4>(), static_cast<uint32_t>(hdr.cuc_tai_ns & 0xFFFF'FFFFULL));
    if (!app_data.empty()) {
        std::memcpy(out.data() + kPusTmSecondaryHdrBytes, app_data.data(), app_data.size());
    }
    return total;
}

/// ============================================================================
/// PUS-C Flight Profile Adapter (ST[01], ST[03], ST[05], ST[06], ST[08],
/// ST[09], ST[11], ST[17])
/// ============================================================================
struct PusScheduledActivity {
    uint64_t release_tai_ns{0U};
    uint16_t function_id{0U};
    uint32_t arg{0U};
    bool     active{false};
};

struct PusSidEntry {
    uint16_t sid{0U};
    uint32_t period_ms{1000U};
    bool     enabled{false};
    bool     registered{false};
};

template <size_t MaxSids = 16U, size_t MaxSchedule = 16U, size_t MaxDisabledEvents = 16U>
class PusCAdapter {
public:
    explicit PusCAdapter(uint16_t apid = 0x0100U) noexcept : apid_(apid) {}

    /// Register a Housekeeping Structure ID (SID) for ST[03].
    [[nodiscard]] Status register_sid(uint16_t sid, uint32_t period_ms, bool enabled = true) noexcept {
        for (auto& s : sids_) {
            if (!s.registered) {
                s = PusSidEntry{sid, period_ms, enabled, true};
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] bool is_sid_enabled(uint16_t sid) const noexcept {
        for (const auto& s : sids_) {
            if (s.registered && s.sid == sid) return s.enabled;
        }
        return false;
    }

    /// ST[01] Request Verification Report encoder: maps CmdStage + CmdResult to TM(1,1..1,8).
    /// Respects the originating TC's 4-bit `ack_flags` for success reports; failure reports
    /// (1,2 / 1,4 / 1,6 / 1,8) are always emitted unconditionally per ECSS-E-ST-70-41C.
    [[nodiscard]] Expected<size_t, WireErr>
    build_st01_report(std::span<std::byte> out_sp, uint16_t tc_req_id, uint16_t tc_seq_ctrl,
                      uint8_t tc_ack_flags, CmdStage stage, CmdResult result,
                      uint16_t dest_id, TaiTimestamp now) noexcept {
        const bool ok = (result == CmdResult::Ok || result == CmdResult::InProgress);
        uint8_t    req_mask = 0U;
        uint8_t    subtype  = 0U;
        switch (stage) {
            case CmdStage::Accepted:
                req_mask = kPusAckAcceptance;
                subtype  = ok ? 1U : 2U;
                break;
            case CmdStage::Started:
                req_mask = kPusAckStart;
                subtype  = ok ? 3U : 4U;
                break;
            case CmdStage::Progress:
                req_mask = kPusAckProgress;
                subtype  = ok ? 5U : 6U;
                break;
            case CmdStage::Completed:
                req_mask = kPusAckCompletion;
                subtype  = ok ? 7U : 8U;
                break;
        }
        if (ok && ((tc_ack_flags & req_mask) == 0U)) {
            return 0U; // Suppressed by TC ACK mask
        }

        // App data: [tc_req_id: 2B][tc_seq_ctrl: 2B][error_code: 2B if failure]
        std::array<std::byte, 6U> app{};
        write_be16(std::span<std::byte, 2U>{app.data(), 2U}, tc_req_id);
        write_be16(std::span<std::byte, 2U>{app.data() + 2U, 2U}, tc_seq_ctrl);
        size_t app_len = 4U;
        if (!ok) {
            write_be16(std::span<std::byte, 2U>{app.data() + 4U, 2U},
                       static_cast<uint16_t>(result));
            app_len = 6U;
        }
        return wrap_tm_space_packet(out_sp, static_cast<uint8_t>(PusService::Verification),
                                    subtype, dest_id, now,
                                    std::span<const std::byte>{app.data(), app_len});
    }

    /// ST[03] TM(3,25) Housekeeping Parameter Report encoder.
    [[nodiscard]] Expected<size_t, WireErr>
    build_st03_hk_report(std::span<std::byte> out_sp, uint16_t sid,
                         std::span<const std::byte> hk_params, uint16_t dest_id,
                         TaiTimestamp now) noexcept {
        std::array<std::byte, 128U> app{};
        if (2U + hk_params.size() > app.size()) return Unexpected(WireErr::BufferTooSmall);
        write_be16(std::span<std::byte, 2U>{app.data(), 2U}, sid);
        if (!hk_params.empty()) {
            std::memcpy(app.data() + 2U, hk_params.data(), hk_params.size());
        }
        return wrap_tm_space_packet(out_sp, static_cast<uint8_t>(PusService::Housekeeping), 25U,
                                    dest_id, now,
                                    std::span<const std::byte>{app.data(), 2U + hk_params.size()});
    }

    /// ST[05] Event Report encoder mapping strata::EventRecord -> TM(5,1..5,4).
    /// Returns 0 if suppressed by TC(5,6) disable filter (Fatal events never suppressed).
    [[nodiscard]] Expected<size_t, WireErr>
    build_st05_event_report(std::span<std::byte> out_sp, const EventRecord& ev, uint16_t dest_id,
                            TaiTimestamp now) noexcept {
        if (ev.severity != EventSeverity::Fatal && is_event_disabled(ev.event_id)) {
            return 0U;
        }
        uint8_t subtype = 1U; // Informative
        switch (ev.severity) {
            case EventSeverity::Diagnostic:
            case EventSeverity::Command:
            case EventSeverity::ActivityLo:
            case EventSeverity::ActivityHi: subtype = 1U; break; // TM(5,1) Informative
            case EventSeverity::WarningLo:  subtype = 2U; break; // TM(5,2) Low-severity anomaly
            case EventSeverity::WarningHi:  subtype = 3U; break; // TM(5,3) Medium-severity anomaly
            case EventSeverity::Fatal:      subtype = 4U; break; // TM(5,4) High-severity anomaly
        }
        std::array<std::byte, 12U> app{};
        write_be32(std::span<std::byte, 4U>{app.data(), 4U}, ev.event_id);
        write_be32(std::span<std::byte, 4U>{app.data() + 4U, 4U}, ev.arg0);
        write_be32(std::span<std::byte, 4U>{app.data() + 8U, 4U}, ev.arg1);
        return wrap_tm_space_packet(out_sp, static_cast<uint8_t>(PusService::Events), subtype,
                                    dest_id, now, app);
    }

    /// ST[09] TM(9,3) Spacecraft Time Report encoder.
    [[nodiscard]] Expected<size_t, WireErr>
    build_st09_time_report(std::span<std::byte> out_sp, uint16_t dest_id,
                           TaiTimestamp now) noexcept {
        std::array<std::byte, 10U> app{};
        write_be16(std::span<std::byte, 2U>{app.data(), 2U}, time_report_rate_div_);
        write_be32(std::span<std::byte, 4U>{app.data() + 2U, 4U},
                   static_cast<uint32_t>(now.ns() >> 32U));
        write_be32(std::span<std::byte, 4U>{app.data() + 6U, 4U},
                   static_cast<uint32_t>(now.ns() & 0xFFFF'FFFFULL));
        return wrap_tm_space_packet(out_sp, static_cast<uint8_t>(PusService::Time), 3U, dest_id,
                                    now, app);
    }

    /// Dispatch an incoming PUS-C Telecommand Space Packet across ST[03], ST[05], ST[06],
    /// ST[08], ST[09], ST[11], and ST[17]. Emits any direct TM reply (e.g. TM 17,2 / TM 6,6 /
    /// TM 6,10) into `on_tm_packet(span<const byte>)` plus ST[01] verification packets.
    template <typename OnTmFn, typename OnFuncFn, typename OnMemFn>
    [[nodiscard]] CmdResult dispatch_tc(std::span<const std::byte> raw_sp, TaiTimestamp now,
                                        OnTmFn&& on_tm_packet, OnFuncFn&& on_function,
                                        OnMemFn&& on_mem_op) noexcept {
        const auto sp = parse_space_packet(raw_sp);
        if (!sp.has_value() || !sp.value().is_tc || !sp.value().has_sec_hdr) {
            return CmdResult::InvalidArg;
        }
        const auto tc = parse_pus_tc(sp.value().payload);
        if (!tc.has_value()) {
            return CmdResult::InvalidArg;
        }

        const auto&    hdr  = tc.value().header;
        const auto     data = tc.value().app_data;
        const uint16_t req_id =
            static_cast<uint16_t>(0x1800U | (sp.value().apid & 0x07FFU));
        const uint16_t seq_ctrl =
            static_cast<uint16_t>(0xC000U | (sp.value().seq_count & 0x3FFFU));

        std::array<std::byte, 256U> tm_buf{};
        auto emit_verif = [&](CmdStage stage, CmdResult res) noexcept {
            const auto n = build_st01_report(tm_buf, req_id, seq_ctrl, hdr.ack_flags, stage, res,
                                             hdr.source_id, now);
            if (n.has_value() && n.value() > 0U) {
                on_tm_packet(std::span<const std::byte>{tm_buf.data(), n.value()});
            }
        };

        emit_verif(CmdStage::Accepted, CmdResult::Ok);
        emit_verif(CmdStage::Started, CmdResult::Ok);

        CmdResult exec_res = CmdResult::UnknownOpcode;

        switch (static_cast<PusService>(hdr.service_type)) {
            case PusService::Housekeeping: {
                if ((hdr.service_subtype == 5U || hdr.service_subtype == 6U) && data.size() >= 2U) {
                    const uint16_t sid = read_be16(data.subspan<0, 2>());
                    exec_res           = set_sid_enabled(sid, hdr.service_subtype == 5U)
                                             ? CmdResult::Ok
                                             : CmdResult::InvalidArg;
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            case PusService::Events: {
                if ((hdr.service_subtype == 5U || hdr.service_subtype == 6U) && data.size() >= 4U) {
                    const uint32_t ev_id = read_be32(data.subspan<0, 4>());
                    set_event_disabled(ev_id, hdr.service_subtype == 6U);
                    exec_res = CmdResult::Ok;
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            case PusService::Memory: {
                // TC(6,2) load, TC(6,5) dump -> TM(6,6), TC(6,9) check -> TM(6,10)
                if (data.size() >= 8U) {
                    const uint32_t addr = read_be32(data.subspan<0, 4>());
                    const uint32_t len  = read_be32(data.subspan<4, 4>());
                    std::array<std::byte, 128U> reply_payload{};
                    size_t                      reply_len = 0U;
                    exec_res = on_mem_op(hdr.service_subtype, addr, len, data.subspan(8U),
                                         reply_payload, reply_len);
                    if (exec_res == CmdResult::Ok &&
                        (hdr.service_subtype == 5U || hdr.service_subtype == 9U)) {
                        const uint8_t tm_sub = (hdr.service_subtype == 5U) ? 6U : 10U;
                        const auto    n      = wrap_tm_space_packet(
                            tm_buf, static_cast<uint8_t>(PusService::Memory), tm_sub,
                            hdr.source_id, now,
                            std::span<const std::byte>{reply_payload.data(), reply_len});
                        if (n.has_value()) {
                            on_tm_packet(std::span<const std::byte>{tm_buf.data(), n.value()});
                        }
                    }
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            case PusService::Functions: {
                // TC(8,1): [function_id: 2B][arg: 4B]
                if (hdr.service_subtype == 1U && data.size() >= 6U) {
                    const uint16_t fn_id = read_be16(data.subspan<0, 2>());
                    const uint32_t arg   = read_be32(data.subspan<2, 4>());
                    exec_res             = on_function(fn_id, arg);
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            case PusService::Time: {
                // TC(9,1): [rate_divider: 2B]
                if (hdr.service_subtype == 1U && data.size() >= 2U) {
                    time_report_rate_div_ = read_be16(data.subspan<0, 2>());
                    exec_res              = CmdResult::Ok;
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            case PusService::Schedule: {
                if (hdr.service_subtype == 1U) {
                    schedule_enabled_ = true;
                    exec_res          = CmdResult::Ok;
                } else if (hdr.service_subtype == 2U) {
                    schedule_enabled_ = false;
                    exec_res          = CmdResult::Ok;
                } else if (hdr.service_subtype == 3U) {
                    for (auto& act : schedule_) act.active = false;
                    exec_res = CmdResult::Ok;
                } else if (hdr.service_subtype == 4U && data.size() >= 14U) {
                    const uint64_t hi = read_be32(data.subspan<0, 4>());
                    const uint64_t lo = read_be32(data.subspan<4, 4>());
                    const uint16_t fn = read_be16(data.subspan<8, 2>());
                    const uint32_t ar = read_be32(data.subspan<10, 4>());
                    exec_res          = insert_scheduled((hi << 32U) | lo, fn, ar)
                                            ? CmdResult::Ok
                                            : CmdResult::Busy;
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            case PusService::Test: {
                // TC(17,1) Are-You-Alive -> TM(17,2) Connection Test Report
                if (hdr.service_subtype == 1U) {
                    const auto n = wrap_tm_space_packet(
                        tm_buf, static_cast<uint8_t>(PusService::Test), 2U, hdr.source_id, now, {});
                    if (n.has_value()) {
                        on_tm_packet(std::span<const std::byte>{tm_buf.data(), n.value()});
                    }
                    exec_res = CmdResult::Ok;
                } else {
                    exec_res = CmdResult::InvalidArg;
                }
                break;
            }

            default:
                exec_res = CmdResult::UnknownOpcode;
                break;
        }

        emit_verif(CmdStage::Completed, exec_res);
        return exec_res;
    }

    /// Execute any due ST[11] time-tagged activities whose `release_tai_ns <= now.ns()`.
    template <typename OnFuncFn>
    size_t tick_schedule(TaiTimestamp now, OnFuncFn&& on_function) noexcept {
        if (!schedule_enabled_) return 0U;
        size_t executed = 0U;
        for (auto& act : schedule_) {
            if (act.active && act.release_tai_ns <= now.ns()) {
                act.active = false;
                (void)on_function(act.function_id, act.arg);
                ++executed;
            }
        }
        return executed;
    }

    [[nodiscard]] uint16_t time_report_rate_div() const noexcept { return time_report_rate_div_; }
    [[nodiscard]] bool     schedule_enabled() const noexcept { return schedule_enabled_; }

private:
    [[nodiscard]] Expected<size_t, WireErr>
    wrap_tm_space_packet(std::span<std::byte> out_sp, uint8_t svc, uint8_t sub, uint16_t dest_id,
                         TaiTimestamp now, std::span<const std::byte> app_data) noexcept {
        std::array<std::byte, 240U> pus_payload{};
        const PusTmHeader           hdr{
                      .time_ref_status  = 0U,
                      .service_type     = svc,
                      .service_subtype  = sub,
                      .msg_type_counter = msg_counter_++,
                      .destination_id   = dest_id,
                      .cuc_tai_ns       = now.ns(),
        };
        const auto plen = encode_pus_tm(pus_payload, hdr, app_data);
        if (!plen.has_value()) return Unexpected(plen.error());
        return frame_space_packet(out_sp, apid_, sp_seq_++, /*is_tc=*/false, /*has_sec_hdr=*/true,
                                  std::span<const std::byte>{pus_payload.data(), plen.value()});
    }

    bool set_sid_enabled(uint16_t sid, bool en) noexcept {
        for (auto& s : sids_) {
            if (s.registered && s.sid == sid) {
                s.enabled = en;
                return true;
            }
        }
        return false;
    }

    void set_event_disabled(uint32_t event_id, bool disable) noexcept {
        if (disable) {
            for (auto& e : disabled_events_) {
                if (e.active && e.event_id == event_id) return;
            }
            for (auto& e : disabled_events_) {
                if (!e.active) {
                    e = DisabledEv{event_id, true};
                    return;
                }
            }
        } else {
            for (auto& e : disabled_events_) {
                if (e.active && e.event_id == event_id) {
                    e.active = false;
                }
            }
        }
    }

    [[nodiscard]] bool is_event_disabled(uint32_t event_id) const noexcept {
        for (const auto& e : disabled_events_) {
            if (e.active && e.event_id == event_id) return true;
        }
        return false;
    }

    bool insert_scheduled(uint64_t release_tai_ns, uint16_t fn_id, uint32_t arg) noexcept {
        for (auto& act : schedule_) {
            if (!act.active) {
                act = PusScheduledActivity{release_tai_ns, fn_id, arg, true};
                return true;
            }
        }
        return false;
    }

    struct DisabledEv {
        uint32_t event_id{0U};
        bool     active{false};
    };

    uint16_t                                       apid_{0x0100U};
    uint16_t                                       sp_seq_{0U};
    uint16_t                                       msg_counter_{0U};
    uint16_t                                       time_report_rate_div_{1U};
    bool                                           schedule_enabled_{true};
    std::array<PusSidEntry, MaxSids>               sids_{};
    std::array<PusScheduledActivity, MaxSchedule>  schedule_{};
    std::array<DisabledEv, MaxDisabledEvents>      disabled_events_{};
};

} // namespace strata::services::comms
