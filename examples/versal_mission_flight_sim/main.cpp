// =============================================================================
// STRATA-20 End-to-End Multi-Partition Versal Mission Integration Simulator
// Consumes build-time auto-generated `strata_mission_icd.hpp` and
// `strata_hyp_tables.hpp` emitted directly from `versal_dual_instrument.toml`
// and `hyperspec_sensor_seds.xml`.
//
// Exercises the entire flight stack in concert:
//  1. Bare-metal Type-1 Hypervisor scheduling all 5 mission partitions with
//     static memory isolation and doorbell token-bucket rate limiting.
//  2. Ground Uplink -> ASM FrameAccumulator -> CCSDS TC -> SDLS Decrypt/Verify
//     -> 4-stage RBAC CmdService -> SEDS-generated HW register programming.
//  3. Zero-Copy Science Data Plane -> PL CCSDS-123 Compression -> 64-bit
//     DpCatalog -> CFDP Class 2 TX/RX -> ComQueue -> TM/USLP + ComTee Tap.
//  4. Live Fault Injection: Class D Data Abort Trap -> Hypervisor Halts Offender
//     -> FDIR FaultRouter WarmRestart preserving CriticalStore & bumping GenId.
//  5. Live CRAM SEU Attribution -> Authenticated SlotManager DFX Reload with
//     XilSEM scrubber pause/reinit & SCSR MANIFEST_HASH verification.
//  6. Cryptographically Signed AOT Autonomy Module -> AutonomySandbox reading
//     live ChannelQueryAdapter telemetry with strict command opcode allowlist.
// =============================================================================

#include "strata_hyp_tables.hpp"
#include "strata_mission_icd.hpp"

#include <host_sil/host_sil_backend.hpp>
#include <strata/middleware.hpp>
#include <strata/services/autonomy/aot_loader.hpp>
#include <strata/services/cdh/cmd_service.hpp>
#include <strata/services/cdh/telemetry_query.hpp>
#include <strata/services/cdh/time_and_sysmon.hpp>
#include <strata/services/comms/com_pipeline.hpp>
#include <strata/services/comms/downlink_framers.hpp>
#include <strata/services/comms/link_adapters.hpp>
#include <strata/services/fdir/fdir_engine.hpp>
#include <strata/services/fdir/health_and_fault_router.hpp>
#include <strata/services/fdir/slot_manager.hpp>
#include <strata/services/storage/data_products.hpp>
#include <strata/services/storage/file_and_cfdp.hpp>
#include <strata/services/storage/pl_compression_stage.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define MISSION_CHECK(cond)                                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "MISSION FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

using namespace strata;
using namespace strata::generated;

static_assert(kManifestHash == kHypManifestHash, "Generated C++ ICD and Hypervisor hashes must match");

/// Hardware register typed directly from CCSDS SEDS generated offset (0x1000).
using RegHsiIntegrationUs =
    mmio::Reg<seds::kReg_HyperspecImagerSensor_INTEGRATION_US, mmio::RW, uint32_t>;

struct SarCheckpoint {
    uint32_t completed_strips{0U};
    uint32_t last_mode{0U};
};

struct MissionPlCompressor {
    [[nodiscard]] Expected<FrameDesc, Status>
    compress_dma(const FrameDesc& src, services::storage::PlCompressionAlg,
                 uint64_t dst_phys) noexcept {
        FrameDesc out    = src;
        out.phys_addr    = dst_phys;
        out.length_bytes = src.length_bytes / 4U;
        out.crc32c       = src.crc32c ^ 0x5A5A'A5A5U;
        return out;
    }
};

struct MissionSlotBackend {
    uint64_t expected_hash{kManifestHash};
    uint32_t reloads{0U};
    uint32_t scrub_pauses{0U};
    uint32_t scrub_reinits{0U};

    [[nodiscard]] bool verify_signature(const services::fdir::SignedSlotImage& img) noexcept {
        return img.expected_manifest_hash == expected_hash;
    }
    [[nodiscard]] Status quiesce_and_decouple(uint16_t, services::fdir::SlotFabricKind) noexcept {
        return Status::Ok;
    }
    [[nodiscard]] Status pause_scrubber(uint16_t, services::fdir::SlotFabricKind) noexcept {
        ++scrub_pauses;
        return Status::Ok;
    }
    [[nodiscard]] Status load_bitstream(uint16_t, services::fdir::SlotFabricKind,
                                        const services::fdir::SignedSlotImage&) noexcept {
        ++reloads;
        return Status::Ok;
    }
    [[nodiscard]] Status reinit_scrubber(uint16_t, services::fdir::SlotFabricKind) noexcept {
        ++scrub_reinits;
        return Status::Ok;
    }
    [[nodiscard]] Status release_decoupler(uint16_t, services::fdir::SlotFabricKind) noexcept {
        return Status::Ok;
    }
    [[nodiscard]] Status run_sbit(uint16_t) noexcept { return Status::Ok; }
    [[nodiscard]] uint64_t read_scsr_manifest_hash(uint16_t) noexcept { return expected_hash; }
    [[nodiscard]] Status set_slot_active(uint16_t) noexcept { return Status::Ok; }
};

/// Static backing storage (zero heap, keeps main() stack frame small for flight stack-usage gate).
struct MissionStaticArena {
    hyp::arch::HostSilBackend<>                      hyp_backend{};
    mmio::MockRegisterBank<8192U>                    hsi_scsr_bank{};
    std::array<std::byte, 16U * 1024U>               hsi_pool_ddr{};
    BlackboardStorage<uint32_t>                      hsi_temp_bb{};
    CriticalStoreStorage<SarCheckpoint>              sar_cds_storage{};
    SpscRingStorage<FrameDesc, 16U>                  downlink_primary_ring{};
    SpscRingStorage<FrameDesc, 16U>                  downlink_recorder_ring{};
    services::comms::FrameAccumulator<256U, 4U>      uplink_acc{
        services::comms::FrameAccumulator<>::kDefaultCcsdsAsm,
        services::comms::kTcFrameHeaderBytes,
        &services::comms::FrameAccumulator<256U, 4U>::tc_frame_length_extractor};
    services::comms::SdlsEngine<>                    sdls{};
    services::comms::ComQueue<8U, 128U>              com_queue{};
    services::comms::ComAggregator<128U>             com_agg{};
    SpscRingStorage<services::cdh::CmdVerificationReport, 16U> cmd_report_ring{};
    services::cdh::CmdService<16U, 8U, 16U>          cmd_svc{cmd_report_ring};
    services::cdh::ChannelQueryAdapter<16U>          tlm_query{};
    services::cdh::SysMonitor<8U, 4U>                sysmon{services::cdh::BuildManifestInfo{
        .manifest_hash = kManifestHash}};
    services::fdir::FdirEngine<16U, 8U, 8U>          fdir_engine{};
    services::fdir::FaultRouter<16U, 8U>             fault_router{/*warm=*/2U, /*cold=*/1U};
    services::storage::DpCatalog<16U>                dp_catalog{1024ULL * 1024ULL};
    services::storage::FileService<4U, 8U>           file_svc{};
    services::storage::CfdpTxTransaction<8U>         cfdp_tx{};
    services::storage::CfdpRxTransaction<8U>         cfdp_rx{};
    services::autonomy::SignedAotModuleLoader<>      aot_loader{kManifestHash};
    services::autonomy::AutonomySandbox<8U>          sandbox{};
};

MissionStaticArena g_arena{};

} // namespace

int main() {
    using namespace strata;
    using namespace strata::generated;

    auto& a = g_arena;

    // -------------------------------------------------------------------------
    // 1. Boot Type-1 Hypervisor from auto-generated kPartitionTable
    // -------------------------------------------------------------------------
    static hyp::HypervisorCore<hyp::arch::HostSilBackend<>> hypervisor(
        a.hyp_backend, kMinorFrameUs, kMajorFrameUs,
        static_cast<uint8_t>(PartitionId::fdir_slot_mgr_b));

    for (const auto& pcfg : kPartitionTable) {
        MISSION_CHECK(hypervisor.register_partition(pcfg) == Status::Ok);
    }
    MISSION_CHECK(hypervisor.register_doorbell(hyp::DoorbellChannelConfig{
                      .channel_id             = 1U,
                      .src_partition          = static_cast<uint8_t>(PartitionId::hyperspec_d),
                      .dst_partition          = static_cast<uint8_t>(PartitionId::storage_c),
                      .max_tokens             = 4U,
                      .refill_per_minor_frame = 2U}) == Status::Ok);

    // Initialize simulated SCSR hardware bank for Slot 0 stamped with generated kManifestHash
    a.hsi_scsr_bank.force_word(0x000U, hw::scsr::kMagic);
    a.hsi_scsr_bank.force_word(0x004U, static_cast<uint32_t>(kManifestHash & 0xFFFF'FFFFULL));
    a.hsi_scsr_bank.force_word(0x008U, static_cast<uint32_t>(kManifestHash >> 32U));
    a.hsi_scsr_bank.force_word(0x010U, static_cast<uint32_t>(SlotState::Standby));
    hw::SlotHandle<mmio::MockRegisterBank<8192U>> hsi_slot(a.hsi_scsr_bank, kHwSlots[0].slot_id);
    MISSION_CHECK(hsi_slot.verify_manifest_hash(kManifestHash));
    MISSION_CHECK(a.sysmon.verify_slot_identity(kManifestHash));

    // -------------------------------------------------------------------------
    // 2. Uplink Pipeline: ASM Stream -> FrameAccumulator -> TC -> SDLS -> CmdService -> SEDS Reg
    // -------------------------------------------------------------------------
    std::array<std::byte, 32U> sa_key{};
    for (size_t i = 0U; i < sa_key.size(); ++i) sa_key[i] = static_cast<std::byte>(0xA0U + i);
    MISSION_CHECK(a.sdls.install_sa(0x0042U, sa_key) == Status::Ok);

    // Register RBAC rule for HSI_SET_INTEGRATION (0x0201) from cdh_b
    MISSION_CHECK(a.cmd_svc.add_auth_rule(services::cdh::PartitionAuthEntry{
                      .source_partition  = static_cast<uint8_t>(PartitionId::cdh_b),
                      .min_opcode        = 0x0100U,
                      .max_opcode        = 0x03FFU,
                      .require_sdls_auth = true,
                      .enabled           = true}) == Status::Ok);

    // Build ground TC command payload: [opcode: 2B][integration_us: 4B]
    std::array<std::byte, 6U> cmd_pt{};
    services::comms::write_be16(std::span<std::byte, 2U>{cmd_pt.data(), 2U}, 0x0201U);
    services::comms::write_be32(std::span<std::byte, 4U>{cmd_pt.data() + 2U, 4U}, 4500U);

    std::array<std::byte, 64U> sdls_buf{};
    const auto sdls_len = a.sdls.protect(0x0042U, {}, cmd_pt, sdls_buf);
    MISSION_CHECK(sdls_len.has_value());

    std::array<std::byte, 96U> tc_buf{};
    const auto tc_len = services::comms::frame_tc_frame(
        tc_buf, 0x0123U, 1U, 1U, false, false,
        std::span<const std::byte>{sdls_buf.data(), sdls_len.value()});
    MISSION_CHECK(tc_len.has_value());

    // Stream noise + ASM + TC frame into FrameAccumulator
    std::array<std::byte, 128U> rx_wire{};
    size_t wire_pos = 0U;
    rx_wire[wire_pos++] = std::byte{0xEE};
    for (auto b : services::comms::FrameAccumulator<>::kDefaultCcsdsAsm) rx_wire[wire_pos++] = b;
    std::memcpy(rx_wire.data() + wire_pos, tc_buf.data(), tc_len.value());
    wire_pos += tc_len.value();

    uint32_t applied_integration_us = 0U;
    const size_t rx_frames = a.uplink_acc.ingest(
        std::span<const std::byte>{rx_wire.data(), wire_pos},
        [&](std::span<const std::byte> frame) noexcept {
            const auto tc = services::comms::parse_tc_frame(frame);
            MISSION_CHECK(tc.has_value());
            std::array<std::byte, 32U> pt{};
            const auto pt_len = a.sdls.unprotect({}, tc.value().data_field, pt);
            MISSION_CHECK(pt_len.has_value() && pt_len.value() == 6U);

            const uint16_t opcode = services::comms::read_be16(std::span<const std::byte, 2U>{pt.data(), 2U});
            const uint32_t integ  = services::comms::read_be32(std::span<const std::byte, 4U>{pt.data() + 2U, 4U});
            const auto seq = a.cmd_svc.accept_command(
                static_cast<uint8_t>(PartitionId::cdh_b),
                static_cast<uint8_t>(PartitionId::hyperspec_d), opcode, true, TaiTimestamp{10'000'000ULL});
            MISSION_CHECK(seq.has_value());
            MISSION_CHECK(a.cmd_svc.report_started(seq.value(), TaiTimestamp{10'100'000ULL}) == Status::Ok);
            hsi_slot.write_reg<RegHsiIntegrationUs>(integ);
            applied_integration_us = hsi_slot.read_reg<RegHsiIntegrationUs>();
            MISSION_CHECK(a.cmd_svc.report_completed(seq.value(), CmdResult::Ok,
                                                     TaiTimestamp{10'200'000ULL}) == Status::Ok);
        });
    MISSION_CHECK(rx_frames == 1U && applied_integration_us == 4500U);

    // -------------------------------------------------------------------------
    // 3. Zero-Copy Science Data Plane -> PL Compression -> DpCatalog -> CFDP -> TM/USLP + ComTee
    // -------------------------------------------------------------------------
    static FramePool<16U, 1024U>                     hsi_pool(a.hsi_pool_ddr, 0x0000'0008'0000'0000ULL);
    static DataTopic<FramePool<16U, 1024U>, 4U, 16U> hsi_topic(hsi_pool);
    const auto stor_sub = hsi_topic.add_subscriber(kGuaranteedControlQoS);
    MISSION_CHECK(stor_sub.has_value());

    const auto slot_idx = hsi_pool.allocate_slot(GenId{1U});
    MISSION_CHECK(slot_idx.has_value());
    FrameDesc raw_desc{
        .phys_addr     = hsi_pool.slot_phys_addr(slot_idx.value()),
        .tai_ns        = 20'000'000ULL,
        .length_bytes  = 1024U,
        .pool_slot_idx = slot_idx.value(),
        .src_slot_id   = kHwSlots[0].slot_id,
        .producer_gen  = 1U,
        .flags         = 0x05U,
        .crc32c        = 0x1234'5678U,
    };
    FrameLease<FramePool<16U, 1024U>> lease(hsi_pool, raw_desc);
    MISSION_CHECK(hsi_topic.publish(std::move(lease), TaiTimestamp{20'000'000ULL}) == Status::Ok);
    // Notify storage_c via hypervisor-rate-limited doorbell
    MISSION_CHECK(hypervisor.handle_hypcall(static_cast<uint8_t>(PartitionId::hyperspec_d),
                                            hyp::HypCall::Notify, 1U)
                      .has_value());

    const auto rx_view = hsi_topic.try_receive(stor_sub.value());
    MISSION_CHECK(rx_view.has_value());

    MissionPlCompressor                                       pl_comp_hw{};
    services::storage::PlCompressionStage<MissionPlCompressor> pl_stage(
        pl_comp_hw, services::storage::PlCompressionAlg::Ccsds123B2);
    services::storage::DpHeader dp_hdr{};
    const auto comp_desc = pl_stage.process_frame(rx_view.value().desc(),
                                                  0x0000'0008'0100'0000ULL, dp_hdr);
    MISSION_CHECK(comp_desc.has_value() && comp_desc.value().length_bytes == 256U);

    const auto dp_id = a.dp_catalog.register_product(
        0x0010U, /*priority=*/10U, dp_hdr.compression_alg, TaiTimestamp{21'000'000ULL},
        comp_desc.value().length_bytes, 0x1000ULL, comp_desc.value().crc32c);
    MISSION_CHECK(dp_id.has_value());

    // Transfer Data Product via 64-bit CCSDS CFDP Class 2 TX -> RX with simulated lost chunk & NAK!
    a.cfdp_tx.start(1001U, 256U, 128U, true);
    a.cfdp_rx.start(1001U, 256U, true);
    const auto seg0 = a.cfdp_tx.next_segment(); // [0, 128) -> simulate drop!
    const auto seg1 = a.cfdp_tx.next_segment(); // [128, 256) -> arrives!
    const auto eof  = a.cfdp_tx.next_segment();
    MISSION_CHECK(seg0.has_value() && seg1.has_value() && eof.has_value() && eof.value().is_eof);
    MISSION_CHECK(a.cfdp_rx.on_file_data(seg1.value().offset, seg1.value().length) == Status::Ok);
    a.cfdp_rx.on_eof();
    std::array<services::storage::ByteExtent, 4U> missing{};
    const size_t gap_n = a.cfdp_rx.collect_missing_extents(missing);
    MISSION_CHECK(gap_n == 1U && missing[0].start == 0U && missing[0].end == 128U);
    MISSION_CHECK(a.cfdp_tx.on_nak(std::span<const services::storage::ByteExtent>{missing.data(), gap_n}) ==
                  Status::Ok);
    const auto retx = a.cfdp_tx.next_segment();
    MISSION_CHECK(retx.has_value() && retx.value().is_retransmit);
    MISSION_CHECK(a.cfdp_rx.on_file_data(retx.value().offset, retx.value().length) == Status::Ok);
    MISSION_CHECK(a.cfdp_rx.is_complete());
    a.cfdp_tx.on_finished_ack();
    MISSION_CHECK(a.cfdp_tx.is_complete());

    // Downlink framing via TM + USLP + zero-copy ComTee flight recorder tap
    a.downlink_primary_ring.reset(GenId{1U}, GenId{1U});
    a.downlink_recorder_ring.reset(GenId{1U}, GenId{1U});
    services::comms::ComTee<16U, 16U> com_tee(a.downlink_primary_ring, a.downlink_recorder_ring);
    MISSION_CHECK(com_tee.forward(comp_desc.value()) == Status::Ok);
    MISSION_CHECK(com_tee.primary_count() == 1U && com_tee.tapped_count() == 1U);

    // -------------------------------------------------------------------------
    // 4. Live Class D Hardware Data Abort Trap -> Isolated WarmRestart Preserving CriticalStore
    // -------------------------------------------------------------------------
    const uint8_t sar_pid  = static_cast<uint8_t>(PartitionId::sar_dfx_d);
    const uint8_t fdir_pid = static_cast<uint8_t>(PartitionId::fdir_slot_mgr_b);
    CriticalStore<SarCheckpoint> sar_cds(a.sar_cds_storage, kManifestHash);
    sar_cds.checkpoint(SarCheckpoint{.completed_strips = 17U, .last_mode = 2U});
    a.hyp_backend.partition_ram(sar_pid).bss_and_stack[0]  = std::byte{0xDE};
    a.hyp_backend.partition_ram(sar_pid).critical_store[0] = std::byte{0x77};

    hypervisor.on_hardware_trap(hyp::TrapContext{
        .partition_id = sar_pid,
        .trap_kind    = 1U, // DataAbort
        .gen_id       = hypervisor.partition(sar_pid).gen.value,
        .pc           = 0x1400'2210ULL,
        .fault_addr   = 0x1000'0000ULL, // Attempted write to cdh_b Class B RAM!
        .tai_ns       = 30'000'000ULL,
    });
    MISSION_CHECK(hypervisor.partition(sar_pid).halted);
    const auto trap = hypervisor.pop_trap();
    MISSION_CHECK(trap.has_value());

    const auto decision = a.fault_router.handle_trap(
        services::fdir::CrashRecord{
            .partition_id = trap.value().partition_id,
            .criticality  = Criticality::ClassD,
            .kind         = services::fdir::TrapKind::DataAbort,
            .gen_id       = trap.value().gen_id,
            .pc           = trap.value().pc,
            .fault_addr   = trap.value().fault_addr,
            .tai_ns       = trap.value().tai_ns,
        },
        [&](uint8_t target_pid, hyp::PartCtlOp op) noexcept {
            const auto r = hypervisor.handle_hypcall(fdir_pid, hyp::HypCall::PartCtl, target_pid,
                                                     static_cast<uint64_t>(op));
            MISSION_CHECK(r.has_value());
        });
    MISSION_CHECK(decision == services::fdir::FaultRoutingDecision::WarmRestartPartition);
    MISSION_CHECK(a.fault_router.safe_mode_escalations() == 0U);
    MISSION_CHECK(hypervisor.partition(sar_pid).gen.value == 2U);
    MISSION_CHECK(a.hyp_backend.partition_ram(sar_pid).bss_and_stack[0] == std::byte{0x00});
    MISSION_CHECK(a.hyp_backend.partition_ram(sar_pid).critical_store[0] == std::byte{0x77});
    const auto restored_sar = sar_cds.restore();
    MISSION_CHECK(restored_sar.has_value() && restored_sar.value().completed_strips == 17U);

    // -------------------------------------------------------------------------
    // 5. Live CRAM SEU Attribution -> Authenticated SlotManager DFX Reload
    // -------------------------------------------------------------------------
    MISSION_CHECK(a.fdir_engine.register_cram_range(services::fdir::CramFrameRange{
                      .frame_addr_start = 0x0004'0000U,
                      .frame_addr_end   = 0x0004'0FFFU,
                      .slot_id          = kHwSlots[1].slot_id,
                      .owner_partition  = sar_pid,
                      .enabled          = true}) == Status::Ok);
    const auto dir = a.fdir_engine.handle_uncorrectable_cram_upset(0x0004'0120U);
    MISSION_CHECK(dir.level == services::fdir::RecoveryLevel::QuiesceAndReloadSlot &&
                  dir.primary_slot_id == kHwSlots[1].slot_id);

    MissionSlotBackend                                 slot_backend{};
    services::fdir::SlotManager<MissionSlotBackend, 4U> slot_mgr(slot_backend);
    MISSION_CHECK(slot_mgr.register_slot(kHwSlots[1].slot_id,
                                         services::fdir::SlotFabricKind::VersalPlDfx) == Status::Ok);
    services::fdir::SignedSlotImage sar_pdi{
        .image_id               = 201U,
        .expected_manifest_hash = kManifestHash,
        .staged_in_verified_vol = true,
    };
    MISSION_CHECK(slot_mgr.load_and_verify(kHwSlots[1].slot_id, sar_pdi) == Status::Ok);
    MISSION_CHECK(slot_mgr.activate(kHwSlots[1].slot_id) == Status::Ok);
    MISSION_CHECK(slot_backend.reloads == 1U && slot_backend.scrub_pauses == 1U &&
                  slot_backend.scrub_reinits == 1U);

    // -------------------------------------------------------------------------
    // 6. Signed AOT Autonomy Module -> ChannelQueryAdapter Telemetry -> Allowlist Gate
    // -------------------------------------------------------------------------
    BlackboardWriter<uint32_t> temp_writer(a.hsi_temp_bb, GenId{1U});
    temp_writer.publish(310U, TaiTimestamp{40'000'000ULL});
    MISSION_CHECK(a.tlm_query.register_channel(kRomTelemetry[1].channel_id, a.hsi_temp_bb) == Status::Ok);
    const auto temp_snap = a.tlm_query.query_typed<uint32_t>(kRomTelemetry[1].channel_id);
    MISSION_CHECK(temp_snap.has_value());

    std::array<services::autonomy::SandboxInstr, 4U> auto_prog{
        services::autonomy::SandboxInstr{services::autonomy::SandboxOp::ReadTlmGt, 0U, 0U, 300},
        services::autonomy::SandboxInstr{services::autonomy::SandboxOp::EmitCmdIf, 0U, 0x0201U, 2000}, // Allowed
        services::autonomy::SandboxInstr{services::autonomy::SandboxOp::EmitCmdIf, 0U, 0x0301U, 1},    // Blocked!
        services::autonomy::SandboxInstr{services::autonomy::SandboxOp::Halt, 0U, 0U, 0},
    };
    const auto code_bytes = std::as_bytes(std::span<const services::autonomy::SandboxInstr>{auto_prog});
    services::autonomy::SignedAotModuleHeader aot_hdr{
        .abi_version       = 1U,
        .instruction_count = 4U,
        .module_id         = 55U,
        .max_steps_bound   = 64U,
        .manifest_hash     = kManifestHash,
        .code_crc32c       = Crc32c::compute(code_bytes),
    };
    services::autonomy::ReferenceAotVerifier aot_verifier{};
    services::autonomy::ReferenceAotVerifier::compute_tag(aot_verifier.trusted_key_seed, aot_hdr,
                                                          code_bytes, aot_hdr.signature_tag);
    std::array<std::byte, sizeof(aot_hdr) + sizeof(auto_prog)> aot_blob{};
    std::memcpy(aot_blob.data(), &aot_hdr, sizeof(aot_hdr));
    std::memcpy(aot_blob.data() + sizeof(aot_hdr), code_bytes.data(), code_bytes.size());

    MISSION_CHECK(a.aot_loader.load_from_image(aot_blob) == Status::Ok);
    MISSION_CHECK(a.sandbox.allow_opcode(0x0201U) == Status::Ok); // Allow exposure adjust, NOT DFX reload!
    std::array<int64_t, 1U> ro_tlm{static_cast<int64_t>(temp_snap.value().value)};
    uint32_t auto_cmds_ok = 0U;
    const auto auto_sum   = a.sandbox.execute(a.aot_loader.loaded_program(), ro_tlm,
                                              a.aot_loader.header().max_steps_bound,
                                              [&](uint16_t op, int64_t) noexcept {
                                                  if (op == 0x0201U) ++auto_cmds_ok;
                                              });
    MISSION_CHECK(auto_sum.halted_cleanly && auto_sum.cmds_forwarded == 1U &&
                  auto_sum.cmds_blocked == 1U && auto_cmds_ok == 1U);

    std::printf(
        "PASS: versal_mission_flight_sim | HASH=0x%016llX | parts=%zu | integ=%u us | "
        "dfx_reloads=%u | sar_gen=%u | auto_ok=%u | auto_blocked=%u\n",
        static_cast<unsigned long long>(kManifestHash), kPartitionTable.size(),
        applied_integration_us, slot_backend.reloads, hypervisor.partition(sar_pid).gen.value,
        auto_sum.cmds_forwarded, auto_sum.cmds_blocked);
    return 0;
}
