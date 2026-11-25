#include <strata/services/cdh/cmd_service.hpp>
#include <strata/services/cdh/flight_ops.hpp>
#include <strata/services/cdh/sequencer.hpp>
#include <strata/services/cdh/telemetry.hpp>

#include <array>
#include <cmath>
#include <cstdio>

namespace {

using namespace strata::literals;

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                       \
        }                                                                                   \
    } while (false)

struct DemoHk {
    uint32_t voltage_mv{3300U};
    int32_t  temp_mdeg{25'000};
};

int test_cmd_service_st01() {
    using namespace strata::services::cdh;

    strata::SpscRingStorage<CmdVerificationReport, 16U> ring{};
    ring.reset(strata::GenId{1U}, strata::GenId{1U});
    CmdService<8U, 8U, 16U> cmd_svc(ring);
    strata::SpscConsumer<CmdVerificationReport, 16U> rx(ring, strata::GenId{0U});

    // Ground (source=0) allowed 0x0100..0x01FF only when SDLS authenticated
    CHECK(cmd_svc.add_auth_rule(PartitionAuthEntry{
              .source_partition  = 0U,
              .min_opcode        = 0x0100U,
              .max_opcode        = 0x01FFU,
              .require_sdls_auth = true,
          }) == strata::Status::Ok);

    // Unauthenticated telecommand rejected
    auto denied = cmd_svc.accept_command(0U, 2U, 0x0105U, false, strata::TaiTimestamp{10U});
    CHECK(!denied.has_value() && denied.error() == strata::CmdResult::Denied);

    // Full 4-stage PUS ST[01] lifecycle: Accepted -> Started -> Progress(50%) -> Completed
    auto seq = cmd_svc.accept_command(0U, 2U, 0x0105U, true, strata::TaiTimestamp{20U});
    CHECK(seq.has_value());
    CHECK(cmd_svc.report_started(*seq, strata::TaiTimestamp{30U}) == strata::Status::Ok);
    CHECK(cmd_svc.report_progress(*seq, 1U, 50U, strata::TaiTimestamp{40U}) == strata::Status::Ok);
    CHECK(cmd_svc.report_completed(*seq, strata::CmdResult::Ok, strata::TaiTimestamp{50U}) ==
          strata::Status::Ok);

    CHECK(cmd_svc.accepted_count() == 1U);
    CHECK(cmd_svc.rejected_count() == 1U);
    CHECK(cmd_svc.completed_count() == 1U);
    CHECK(rx.available() == 5U); // 1 Denied + 4 ST[01] stages
    return 0;
}

int test_telemetry_packetizer_and_stats() {
    using namespace strata::services::cdh;

    strata::BlackboardStorage<DemoHk> bb_mem{};
    strata::BlackboardWriter<DemoHk>  writer(bb_mem);
    strata::BlackboardReader<DemoHk>  reader(bb_mem);
    writer.publish(DemoHk{.voltage_mv = 3312U, .temp_mdeg = 26'500},
                   strata::TaiTimestamp{(100_ms).ns()});

    using HkPkt = TlmPacketizer<0x0042U, DemoHk>;
    HkPkt                      pktzr{};
    std::array<std::byte, 64U> wire{};
    auto len = pktzr.sample_and_pack(reader, strata::TaiTimestamp{(105_ms).ns()}, wire);
    CHECK(len.has_value() && *len == HkPkt::kPacketBytes);

    auto parsed = strata::services::comms::parse_space_packet(
        std::span<const std::byte>(wire.data(), *len));
    CHECK(parsed.has_value() && parsed->apid == 0x0042U);

    // PUS ST[04] Welford online stats
    StatsCollector stats{};
    stats.observe(10.0);
    stats.observe(20.0);
    stats.observe(30.0);
    const auto s = stats.snapshot();
    CHECK(s.sample_count == 3U);
    CHECK(std::fabs(s.min_val - 10.0) < 1e-9);
    CHECK(std::fabs(s.max_val - 30.0) < 1e-9);
    CHECK(std::fabs(s.mean - 20.0) < 1e-9);
    CHECK(std::fabs(s.variance - 100.0) < 1e-9);

    // PUS ST[14] Forwarding Control
    ForwardingControl<> fwd{};
    CHECK(fwd.set_mode(0x0042U, ForwardMode::StoreOnly) == strata::Status::Ok);
    CHECK(!fwd.should_send_rf(0x0042U) && fwd.should_store(0x0042U));
    return 0;
}

int test_sequencer_and_flight_mem_ops() {
    using namespace strata::services::cdh;

    // Multi-slot Sequencer with CRC verification & relative timing
    std::array<SeqStep, 2U> steps{{
        {SeqTimeMode::Immediate, 1U, 0x0101U, 10U, 0U},
        {SeqTimeMode::Relative, 1U, 0x0102U, 20U, (50_ms).ns()},
    }};
    const uint32_t crc = strata::Crc32c::compute(std::as_bytes(std::span<const SeqStep>(steps)));

    SeqEngine<> seq{};
    CHECK(seq.load(0U, steps, crc ^ 1U, strata::TaiTimestamp{0U}) == strata::Status::CrcMismatch);
    CHECK(seq.load(0U, steps, crc, strata::TaiTimestamp{0U}) == strata::Status::Ok);

    uint32_t executed_opcodes = 0U;
    auto dispatch_cb = [&](const SeqStep& st) noexcept {
        executed_opcodes += st.opcode;
        return strata::CmdResult::Ok;
    };
    CHECK(seq.tick(strata::TaiTimestamp{(10_ms).ns()}, dispatch_cb) == 1U);
    CHECK(seq.tick(strata::TaiTimestamp{(40_ms).ns()}, dispatch_cb) == 0U); // Relative delay not met
    CHECK(seq.tick(strata::TaiTimestamp{(65_ms).ns()}, dispatch_cb) == 1U);
    CHECK(executed_opcodes == (0x0101U + 0x0102U));
    CHECK(seq.completed_count() == 1U);

    // cFS MD / MM / CS FlightMemoryOps
    std::array<std::byte, 64U> ro_seg{};
    std::array<std::byte, 64U> rw_ram{};
    ro_seg[0] = std::byte{0x42};
    rw_ram[0] = std::byte{0x11};

    FlightMemoryOps<> mem{};
    CHECK(mem.allow_region(reinterpret_cast<uintptr_t>(ro_seg.data()), ro_seg.size(), false) ==
          strata::Status::Ok);
    CHECK(mem.allow_region(reinterpret_cast<uintptr_t>(rw_ram.data()), rw_ram.size(), true) ==
          strata::Status::Ok);

    // MD dwell read allowed on ro_seg, denied outside allowlist
    std::array<std::byte, 8U> dwell_out{};
    auto d_ok = mem.dwell_read(reinterpret_cast<uintptr_t>(ro_seg.data()), 4U, dwell_out);
    CHECK(d_ok.has_value() && dwell_out[0] == std::byte{0x42});
    auto d_bad = mem.dwell_read(0x0000'1000U, 4U, dwell_out);
    CHECK(!d_bad.has_value() && d_bad.error() == strata::Status::PermissionDenied);

    // MM two-step Arm -> Commit denied on RO segment, allowed on RW segment
    std::array<std::byte, 4U> patch_data{
        std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
    };
    const uint32_t pcrc = strata::Crc32c::compute(patch_data);
    CHECK(mem.arm_patch(reinterpret_cast<uintptr_t>(ro_seg.data()), patch_data, pcrc) ==
          strata::Status::PermissionDenied);
    CHECK(mem.arm_patch(reinterpret_cast<uintptr_t>(rw_ram.data()), patch_data, pcrc) ==
          strata::Status::Ok);
    CHECK(mem.commit_patch(pcrc) == strata::Status::Ok);
    CHECK(rw_ram[0] == std::byte{0xAA} && rw_ram[3] == std::byte{0xDD});

    // CS background software checksum catches simulated SEU bit-flip in ro_seg!
    CHECK(mem.register_checksum_segment(7U, ro_seg) == strata::Status::Ok);
    CHECK(mem.scrub_next_segment().has_value());
    ro_seg[15] = std::byte{0x01}; // Inject SEU bit-flip
    auto scrub = mem.scrub_next_segment();
    CHECK(!scrub.has_value() && scrub.error() == 7U);
    CHECK(mem.scrub_mismatches() == 1U);
    return 0;
}

} // namespace

int main() {
    if (test_cmd_service_st01() != 0) return 1;
    if (test_telemetry_packetizer_and_stats() != 0) return 1;
    if (test_sequencer_and_flight_mem_ops() != 0) return 1;
    std::printf("PASS: strata.unit.cdh_services\n");
    return 0;
}
