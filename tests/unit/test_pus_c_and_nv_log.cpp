#include <strata/ipc/nv_fault_log.hpp>
#include <strata/services/comms/pus_c.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

using namespace strata;
using namespace strata::services::comms;

void test_secded_exhaustive_72bit() {
    constexpr std::array<uint64_t, 5U> kPatterns = {
        0x0000'0000'0000'0000ULL,
        0xFFFF'FFFF'FFFF'FFFFULL,
        0x5555'AAAA'1234'5678ULL,
        0xDEAD'BEEF'CAFE'BABEULL,
        0x0123'4567'89AB'CDEFULL,
    };

    for (const uint64_t val : kPatterns) {
        const EccWord64 clean = Secded64::encode(val);

        // Clean decode
        EccWord64 w = clean;
        CHECK(Secded64::decode_and_heal(w) == EccStatus::Clean);
        CHECK(w.data == val);

        // Flip EVERY single data bit (0..63) -> must recover exact original value!
        for (uint8_t b = 0U; b < 64U; ++b) {
            EccWord64 corrupted = clean;
            corrupted.data ^= (1ULL << b);
            CHECK(Secded64::decode_and_heal(corrupted) == EccStatus::SingleBitCorrected);
            CHECK(corrupted.data == val && corrupted.ecc == clean.ecc);
        }

        // Flip EVERY single ECC bit (0..7) -> must heal ECC cleanly!
        for (uint8_t eb = 0U; eb < 8U; ++eb) {
            EccWord64 corrupted = clean;
            corrupted.ecc ^= static_cast<uint8_t>(1U << eb);
            CHECK(Secded64::decode_and_heal(corrupted) == EccStatus::SingleBitCorrected);
            CHECK(corrupted.data == val && corrupted.ecc == clean.ecc);
        }

        // Flip 2 bits -> must detect as uncorrectable DoubleBitDetected!
        for (uint8_t b = 0U; b < 32U; ++b) {
            EccWord64 dbl = clean;
            dbl.data ^= (1ULL << b) | (1ULL << (b + 17U));
            CHECK(Secded64::decode_and_heal(dbl) == EccStatus::DoubleBitDetected);
        }
    }
}

void test_nv_fault_log_reboot_tmr_and_scrub() {
    constexpr uint64_t kHash = 0x14A6'F9D1'9243'B827ULL;

    // Simulated refresh-free MRAM/FRAM block retained across cold reboot
    static NvFaultLogStorage<16U> mram_backing{};

    // --- Boot Epoch 1 (Initial Cold Power-On) ---
    {
        NvFaultLog<16U> log(mram_backing);
        CHECK(log.mount_on_boot(kHash, ResetReason::PowerOnCold, TaiTimestamp{1'000ULL}, true) ==
              Status::Ok);
        CHECK(log.header_snapshot().boot_epoch == 1U);
        CHECK(log.valid_entry_count() == 1U); // Automatic RebootMarker at slot 0

        CHECK(log.append(NvRecordKind::CrashTrap, /*src=*/4U, /*esr=*/0x9200'0047U,
                         TaiTimestamp{5'000ULL}, 0x1400'2210ULL, 0x1000'0000ULL, 0xCAFEULL) ==
              Status::Ok);
        CHECK(log.append(NvRecordKind::CriticalEvent, /*src=*/1U, /*ev=*/0x0099U,
                         TaiTimestamp{9'000ULL}, 42U, 99U, 0U) == Status::Ok);
        CHECK(log.valid_entry_count() == 3U);

        // Simulate sudden brownout mid-write on next slot (slot 3 has partial payload, NO commit fence)
        mram_backing.slots[3].words[0]     = Secded64::encode(0xBAD0'BAD0'BAD0'BAD0ULL);
        mram_backing.slots[3].commit_fence = Secded64::encode(0ULL);
    }

    // --- Inject Radiation Upsets While Powered Off (No DRAM Refresh!) ---
    // 1. Wipe out ENTIRE TMR header replica 1 with 0xFF!
    std::memset(static_cast<void*>(&mram_backing.header.replicas[1]), 0xFF,
                sizeof(mram_backing.header.replicas[1]));
    // 2. Flip a single bit in slot 1 (CrashTrap record) data word 4 (aux0 = PC)!
    mram_backing.slots[1].words[4].data ^= (1ULL << 19U);
    // 3. Flip a single bit in slot 2 (CriticalEvent) ECC parity byte!
    mram_backing.slots[2].words[2].ecc ^= 0x04U;

    // --- Boot Epoch 2 (Brownout / Cold Reboot Recovery) ---
    {
        NvFaultLog<16U> log_after_reboot(mram_backing);
        CHECK(log_after_reboot.mount_on_boot(kHash, ResetReason::BrownoutRecover,
                                             TaiTimestamp{20'000ULL}) == Status::Ok);

        // TMR 2-of-3 majority vote healed wiped replica 1 and incremented boot_epoch to 2!
        const auto hdr = log_after_reboot.header_snapshot();
        CHECK(hdr.boot_epoch == 2U);
        CHECK(hdr.tmr_repairs >= 1U);
        // Slot 0 (Epoch 1 Reboot), Slot 1 (CrashTrap), Slot 2 (CriticalEvent), Slot 3 (Epoch 2 Reboot)
        CHECK(log_after_reboot.valid_entry_count() == 4U);

        // Background scrub heals single-bit upsets in slots 1 & 2 automatically
        uint32_t total_ce = 0U;
        for (size_t i = 0U; i < 16U; ++i) {
            const auto rep = log_after_reboot.scrub_step();
            total_ce += rep.ecc_single_bit_corrected;
            CHECK(rep.ecc_double_bit_detected == 0U);
        }
        CHECK(total_ce == 2U);

        // Read back CrashTrap from BEFORE reboot -> bit-exact recovery!
        const auto trap_rec = log_after_reboot.read_slot(1U);
        CHECK(trap_rec.has_value());
        CHECK(trap_rec.value().boot_epoch == 1U);
        CHECK(trap_rec.value().kind == NvRecordKind::CrashTrap);
        CHECK(trap_rec.value().aux0 == 0x1400'2210ULL);
        CHECK(trap_rec.value().aux1 == 0x1000'0000ULL);

        // Verify Epoch 2 RebootMarker cleanly replaced the torn brownout slot 3
        const auto reboot2 = log_after_reboot.read_slot(3U);
        CHECK(reboot2.has_value() && reboot2.value().boot_epoch == 2U &&
              reboot2.value().kind == NvRecordKind::RebootMarker &&
              reboot2.value().code == static_cast<uint32_t>(ResetReason::BrownoutRecover));

        // Inject a 2-bit uncorrectable upset into slot 2 -> MUST be detected & rejected!
        mram_backing.slots[2].words[1].data ^= (1ULL << 3U) | (1ULL << 27U);
        CHECK(log_after_reboot.read_slot(2U).error() == Status::CrcMismatch);
    }
}

void test_pus_c_flight_profile() {
    PusCAdapter<8U, 8U, 8U> pus(0x0105U);
    CHECK(pus.register_sid(10U, 1000U, true) == Status::Ok);

    // Helper to wrap a PUS-C TC into a CCSDS Space Packet
    auto make_tc_sp = [](std::span<std::byte> out, const PusTcHeader& hdr,
                         std::span<const std::byte> app, uint16_t seq) -> size_t {
        std::array<std::byte, 128U> body{};
        const auto                  blen = encode_pus_tc(body, hdr, app);
        CHECK(blen.has_value());
        const auto slen = frame_space_packet(out, 0x0105U, seq, /*is_tc=*/true, /*sec_hdr=*/true,
                                             std::span<const std::byte>{body.data(), blen.value()});
        CHECK(slen.has_value());
        return slen.value();
    };

    std::array<std::byte, 160U> tc_sp{};
    std::array<uint8_t, 16U>    seen_svc{};
    std::array<uint8_t, 16U>    seen_sub{};
    size_t                      tm_cnt = 0U;

    auto on_tm = [&](std::span<const std::byte> sp_raw) noexcept {
        const auto sp = parse_space_packet(sp_raw);
        CHECK(sp.has_value() && !sp.value().is_tc && sp.value().has_sec_hdr);
        const auto tm = parse_pus_tm(sp.value().payload);
        CHECK(tm.has_value());
        if (tm_cnt < seen_svc.size()) {
            seen_svc[tm_cnt] = tm.value().header.service_type;
            seen_sub[tm_cnt] = tm.value().header.service_subtype;
            ++tm_cnt;
        }
    };

    uint16_t last_fn_id  = 0U;
    uint32_t last_fn_arg = 0U;
    auto     on_fn       = [&](uint16_t fn_id, uint32_t arg) noexcept {
        last_fn_id  = fn_id;
        last_fn_arg = arg;
        return (fn_id == 0xFFFFU) ? CmdResult::Denied : CmdResult::Ok;
    };

    std::array<std::byte, 32U> sim_mem{};
    auto on_mem = [&](uint8_t sub, uint32_t addr, uint32_t len, std::span<const std::byte> wr,
                      std::span<std::byte> reply, size_t& reply_len) noexcept -> CmdResult {
        if (addr + len > sim_mem.size()) return CmdResult::InvalidArg;
        if (sub == 2U) { // TC(6,2) Load
            if (wr.size() < len) return CmdResult::InvalidArg;
            std::memcpy(sim_mem.data() + addr, wr.data(), len);
            return CmdResult::Ok;
        }
        if (sub == 5U) { // TC(6,5) Dump -> TM(6,6)
            std::memcpy(reply.data(), sim_mem.data() + addr, len);
            reply_len = len;
            return CmdResult::Ok;
        }
        if (sub == 9U) { // TC(6,9) Check -> TM(6,10) CRC-32C
            const uint32_t crc = Crc32c::compute(std::span<const std::byte>{sim_mem.data() + addr, len});
            write_be32(std::span<std::byte, 4U>{reply.data(), 4U}, crc);
            reply_len = 4U;
            return CmdResult::Ok;
        }
        return CmdResult::UnknownOpcode;
    };

    // 1. ST[17] Connection Test TC(17,1) with Acceptance + Completion ACK (Start suppressed!)
    {
        tm_cnt = 0U;
        const size_t n = make_tc_sp(
            tc_sp,
            PusTcHeader{.ack_flags = kPusAckAcceptance | kPusAckCompletion, .service_type = 17U, .service_subtype = 1U, .source_id = 1U},
            {}, 1U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), n}, TaiTimestamp{1000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);
        // Expect TM(1,1) Acceptance, TM(17,2) Connection Test, TM(1,7) Completion (NO TM 1,3!)
        CHECK(tm_cnt == 3U);
        CHECK(seen_svc[0] == 1U && seen_sub[0] == 1U);
        CHECK(seen_svc[1] == 17U && seen_sub[1] == 2U);
        CHECK(seen_svc[2] == 1U && seen_sub[2] == 7U);
    }

    // 2. ST[08] Function Management TC(8,1) success AND failure (TM 1,8 failure always emitted even if ACK=0)
    {
        tm_cnt = 0U;
        std::array<std::byte, 6U> app{};
        write_be16(std::span<std::byte, 2U>{app.data(), 2U}, 0x0201U);
        write_be32(std::span<std::byte, 4U>{app.data() + 2U, 4U}, 3500U);
        const size_t n = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = kPusAckAll, .service_type = 8U, .service_subtype = 1U},
            app, 2U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), n}, TaiTimestamp{2000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);
        CHECK(last_fn_id == 0x0201U && last_fn_arg == 3500U && tm_cnt == 3U);

        // Failure with ack_flags=0 still emits TM(1,8) failure report!
        tm_cnt = 0U;
        write_be16(std::span<std::byte, 2U>{app.data(), 2U}, 0xFFFFU);
        const size_t nf = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 8U, .service_subtype = 1U}, app, 3U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), nf}, TaiTimestamp{2100ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Denied);
        CHECK(tm_cnt == 1U && seen_svc[0] == 1U && seen_sub[0] == 8U);
    }

    // 3. ST[03] Housekeeping TC(3,6) disable SID + TM(3,25) report
    {
        std::array<std::byte, 2U> sid_app{};
        write_be16(sid_app, 10U);
        const size_t n = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 3U, .service_subtype = 6U}, sid_app, 4U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), n}, TaiTimestamp{3000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);
        CHECK(!pus.is_sid_enabled(10U));

        std::array<std::byte, 64U> hk_sp{};
        std::array<std::byte, 4U>  hk_vals{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        const auto hk_len = pus.build_st03_hk_report(hk_sp, 10U, hk_vals, 1U, TaiTimestamp{3100ULL});
        CHECK(hk_len.has_value());
        const auto sp = parse_space_packet(std::span<const std::byte>{hk_sp.data(), hk_len.value()});
        const auto tm = parse_pus_tm(sp.value().payload);
        CHECK(tm.has_value() && tm.value().header.service_type == 3U && tm.value().header.service_subtype == 25U);
    }

    // 4. ST[05] Event Reporting + TC(5,6) suppression (Fatal never suppressed!)
    {
        std::array<std::byte, 4U> ev_app{};
        write_be32(ev_app, 0x0042U);
        const size_t n = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 5U, .service_subtype = 6U}, ev_app, 5U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), n}, TaiTimestamp{4000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);

        std::array<std::byte, 64U> ev_sp{};
        EventRecord warn_ev{.event_id = 0x0042U, .severity = EventSeverity::WarningLo};
        CHECK(pus.build_st05_event_report(ev_sp, warn_ev, 1U, TaiTimestamp{4100ULL}).value() == 0U);
        EventRecord fatal_ev{.event_id = 0x0042U, .severity = EventSeverity::Fatal};
        const auto  flen = pus.build_st05_event_report(ev_sp, fatal_ev, 1U, TaiTimestamp{4200ULL});
        CHECK(flen.has_value() && flen.value() > 0U);
        const auto tm = parse_pus_tm(
            parse_space_packet(std::span<const std::byte>{ev_sp.data(), flen.value()}).value().payload);
        CHECK(tm.value().header.service_type == 5U && tm.value().header.service_subtype == 4U);
    }

    // 5. ST[06] Memory Load/Dump/Check -> TM(6,6) & TM(6,10)
    {
        std::array<std::byte, 12U> load_app{};
        write_be32(std::span<std::byte, 4U>{load_app.data(), 4U}, 0U);
        write_be32(std::span<std::byte, 4U>{load_app.data() + 4U, 4U}, 4U);
        load_app[8] = std::byte{0xDE}; load_app[9] = std::byte{0xAD};
        load_app[10] = std::byte{0xBE}; load_app[11] = std::byte{0xEF};
        const size_t nl = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 6U, .service_subtype = 2U}, load_app, 6U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), nl}, TaiTimestamp{5000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);

        tm_cnt = 0U;
        const size_t nc = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 6U, .service_subtype = 9U},
            std::span<const std::byte>{load_app.data(), 8U}, 7U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), nc}, TaiTimestamp{5100ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);
        CHECK(tm_cnt == 1U && seen_svc[0] == 6U && seen_sub[0] == 10U);
    }

    // 6. ST[09] Time Management & ST[11] Time-Based Scheduling
    {
        std::array<std::byte, 2U> rate_app{};
        write_be16(rate_app, 10U);
        const size_t n9 = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 9U, .service_subtype = 1U}, rate_app, 8U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), n9}, TaiTimestamp{6000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);
        CHECK(pus.time_report_rate_div() == 10U);

        std::array<std::byte, 64U> t9_sp{};
        CHECK(pus.build_st09_time_report(t9_sp, 1U, TaiTimestamp{6100ULL}).has_value());

        // Insert ST[11] scheduled activity for TAI = 10_000 ns
        std::array<std::byte, 14U> sch_app{};
        write_be32(std::span<std::byte, 4U>{sch_app.data(), 4U}, 0U);
        write_be32(std::span<std::byte, 4U>{sch_app.data() + 4U, 4U}, 10'000U);
        write_be16(std::span<std::byte, 2U>{sch_app.data() + 8U, 2U}, 0x0777U);
        write_be32(std::span<std::byte, 4U>{sch_app.data() + 10U, 4U}, 1234U);
        const size_t n11 = make_tc_sp(
            tc_sp, PusTcHeader{.ack_flags = 0U, .service_type = 11U, .service_subtype = 4U}, sch_app, 9U);
        CHECK(pus.dispatch_tc(std::span<const std::byte>{tc_sp.data(), n11}, TaiTimestamp{7000ULL},
                              on_tm, on_fn, on_mem) == CmdResult::Ok);
        CHECK(pus.tick_schedule(TaiTimestamp{9'000ULL}, on_fn) == 0U);
        CHECK(pus.tick_schedule(TaiTimestamp{10'000ULL}, on_fn) == 1U);
        CHECK(last_fn_id == 0x0777U && last_fn_arg == 1234U);
    }
}

} // namespace

int main() {
    test_secded_exhaustive_72bit();
    test_nv_fault_log_reboot_tmr_and_scrub();
    test_pus_c_flight_profile();
    std::printf("PASS: test_pus_c_and_nv_log (exhaustive 72-bit SECDED + TMR reboot MRAM log + ECSS-E-ST-70-41C PUS-C)\n");
    return 0;
}
