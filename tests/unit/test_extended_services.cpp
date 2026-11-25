#include <strata/middleware.hpp>
#include <strata/services/autonomy/aot_loader.hpp>
#include <strata/services/cdh/telemetry_query.hpp>
#include <strata/services/cdh/time_and_sysmon.hpp>
#include <strata/services/comms/downlink_framers.hpp>
#include <strata/services/comms/link_adapters.hpp>
#include <strata/services/fdir/health_and_fault_router.hpp>
#include <strata/services/fdir/slot_manager.hpp>
#include <strata/services/storage/file_and_cfdp.hpp>
#include <strata/services/storage/pl_compression_stage.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

struct MockSlotBackend {
    uint64_t hw_hash{0x1122'3344'5566'7788ULL};
    bool     sig_ok{true};
    bool     sbit_ok{true};
    uint32_t scrub_pauses{0U};
    uint32_t scrub_reinits{0U};
    uint32_t loads{0U};

    [[nodiscard]] bool verify_signature(const strata::services::fdir::SignedSlotImage&) noexcept {
        return sig_ok;
    }
    [[nodiscard]] strata::Status quiesce_and_decouple(uint16_t, strata::services::fdir::SlotFabricKind) noexcept {
        return strata::Status::Ok;
    }
    [[nodiscard]] strata::Status pause_scrubber(uint16_t, strata::services::fdir::SlotFabricKind) noexcept {
        ++scrub_pauses;
        return strata::Status::Ok;
    }
    [[nodiscard]] strata::Status load_bitstream(uint16_t, strata::services::fdir::SlotFabricKind,
                                                const strata::services::fdir::SignedSlotImage&) noexcept {
        ++loads;
        return strata::Status::Ok;
    }
    [[nodiscard]] strata::Status reinit_scrubber(uint16_t, strata::services::fdir::SlotFabricKind) noexcept {
        ++scrub_reinits;
        return strata::Status::Ok;
    }
    [[nodiscard]] strata::Status release_decoupler(uint16_t, strata::services::fdir::SlotFabricKind) noexcept {
        return strata::Status::Ok;
    }
    [[nodiscard]] strata::Status run_sbit(uint16_t) noexcept {
        return sbit_ok ? strata::Status::Ok : strata::Status::SlotFaulted;
    }
    [[nodiscard]] uint64_t read_scsr_manifest_hash(uint16_t) noexcept { return hw_hash; }
    [[nodiscard]] strata::Status set_slot_active(uint16_t) noexcept { return strata::Status::Ok; }
};

struct MockPlCompressor {
    [[nodiscard]] strata::Expected<strata::FrameDesc, strata::Status>
    compress_dma(const strata::FrameDesc& src, strata::services::storage::PlCompressionAlg,
                 uint64_t dst_phys_addr) noexcept {
        strata::FrameDesc out = src;
        out.phys_addr         = dst_phys_addr;
        out.length_bytes      = src.length_bytes / 4U; // 4:1 hardware compression
        out.crc32c            = src.crc32c ^ 0xA5A5'5A5AU;
        return out;
    }
};

} // namespace

int main() {
    using namespace strata;
    using namespace strata::literals;

    // 1. STRATA-MW-016: Independent Composable Per-Partition Subtopology Bundles
    {
        constexpr runtime::CdhBundleConfig     kCdhCfg{.partition_id = 0U, .cmd_queue_depth = 16U, .event_ring_depth = 32U};
        constexpr runtime::CommsBundleConfig   kCommsCfg{.partition_id = 1U, .uplink_ring_depth = 8U, .downlink_ring_depth = 16U};
        constexpr runtime::StorageBundleConfig kStorCfg{.partition_id = 2U, .ingest_pool_slots = 8U, .ingest_slot_bytes = 256U, .max_subscribers = 2U};
        constexpr runtime::FdirBundleConfig    kFdirCfg{.partition_id = 3U, .fault_ring_depth = 16U};

        static std::array<std::byte, 8U * 256U> stor_backing{};
        runtime::CdhBundle<kCdhCfg>         cdh{};
        runtime::CommsBundle<kCommsCfg>     comms{};
        runtime::StorageBundle<kStorCfg>    stor{stor_backing, 0x8'0000'0000ULL};
        runtime::FdirBundle<kFdirCfg>       fdir{};

        CHECK(cdh.cmd_producer.try_push(runtime::CmdMessage{.opcode = 0x0101U}) == Status::Ok);
        CHECK(comms.uplink_producer.try_push(FrameDesc{.length_bytes = 128U}) == Status::Ok);
        CHECK(stor.pool.kSlotCount == 8U);
        CHECK(fdir.fault_producer.try_push(runtime::FaultNotification{.fault_code = 42U}) == Status::Ok);
    }

    // 2. STRATA-COM-004 & STRATA-COM-005: TM/AOS/USLP Framers + FrameAccumulator with ASM resync
    {
        using namespace strata::services::comms;
        std::array<std::byte, 128U> buf{};
        std::array<std::byte, 16U>  payload{};
        payload.fill(std::byte{0x5A});

        TmTransferFramer tm(0x123U, 2U, true, false);
        const auto tm_len = tm.frame(buf, payload, 0U);
        CHECK(tm_len.has_value() && tm_len.value() == 6U + 16U + 2U);
        CHECK(crc16_ccitt(std::span<const std::byte>{buf.data(), tm_len.value() - 2U}) ==
              read_be16(std::span<const std::byte>{buf.data() + tm_len.value() - 2U, 2U}.subspan<0, 2>()));

        AosTransferFramer aos(0x42U, 5U, true, false);
        const auto aos_len = aos.frame(buf, payload, 0U);
        CHECK(aos_len.has_value() && aos_len.value() == 8U + 16U + 2U);
        CHECK(aos.vcfc() == 1U);

        UslpTransferFramer uslp(0x1234U, 3U, 1U, true, false);
        const auto uslp_len = uslp.frame(buf, payload, 0U);
        CHECK(uslp_len.has_value() && uslp_len.value() == 12U + 16U + 2U);
        CHECK(uslp.vcfc() == 1U);

        // FrameAccumulator: feed noise + ASM + corrupted TC header + embedded ASM + valid TC frame
        std::array<std::byte, 64U> valid_tc{};
        const auto tc_len = frame_tc_frame(valid_tc, 0x101U, 1U, 9U, false, false, payload);
        CHECK(tc_len.has_value());

        std::array<std::byte, 128U> stream{};
        size_t pos = 0U;
        stream[pos++] = std::byte{0xFF};
        stream[pos++] = std::byte{0x00};
        for (auto b : FrameAccumulator<>::kDefaultCcsdsAsm) stream[pos++] = b;
        stream[pos++] = std::byte{0xC0}; // Invalid version 3
        stream[pos++] = std::byte{0x00};
        stream[pos++] = std::byte{0x00};
        stream[pos++] = std::byte{0x01};
        stream[pos++] = std::byte{0x00};
        for (auto b : FrameAccumulator<>::kDefaultCcsdsAsm) stream[pos++] = b;
        std::memcpy(stream.data() + pos, valid_tc.data(), tc_len.value());
        pos += tc_len.value();

        FrameAccumulator<256U, 4U> acc(FrameAccumulator<>::kDefaultCcsdsAsm, kTcFrameHeaderBytes,
                                       &FrameAccumulator<256U, 4U>::tc_frame_length_extractor);
        size_t valid_frames = 0U;
        const size_t emitted = acc.ingest(std::span<const std::byte>{stream.data(), pos},
                                          [&](std::span<const std::byte> f) noexcept {
                                              const auto parsed = parse_tc_frame(f);
                                              if (parsed.has_value() && parsed.value().frame_seq == 9U) {
                                                  ++valid_frames;
                                              }
                                          });
        CHECK(emitted == 1U && valid_frames == 1U && acc.resync_count() == 1U);
    }

    // 3. STRATA-COM-009 & STRATA-COM-010: ComRetry, ComTee, InterNodeBridge
    {
        using namespace strata::services::comms;
        ComRetry<8U> retry(/*max_retries=*/2U, /*backoff_ticks=*/1U);
        CHECK(retry.enqueue(FrameDesc{.length_bytes = 256U}) == Status::Ok);
        uint32_t calls = 0U;
        CHECK(retry.tick([&](const FrameDesc&) noexcept { ++calls; return Status::Busy; }) == 0U);
        CHECK(retry.tick([&](const FrameDesc&) noexcept { ++calls; return Status::Ok; }) == 0U);
        CHECK(retry.tick([&](const FrameDesc&) noexcept { ++calls; return Status::Ok; }) == 1U);
        CHECK(retry.sent_total() == 1U && retry.retries_total() == 1U && calls == 2U);

        SpscRingStorage<FrameDesc, 8U> primary_st{};
        SpscRingStorage<FrameDesc, 8U> tap_st{};
        primary_st.reset(GenId{1U}, GenId{1U});
        tap_st.reset(GenId{1U}, GenId{1U});
        ComTee<8U, 8U> tee(primary_st, tap_st);
        CHECK(tee.forward(FrameDesc{.phys_addr = 0x1000U, .length_bytes = 64U}) == Status::Ok);
        CHECK(tee.primary_count() == 1U && tee.tapped_count() == 1U);
        SpscConsumer<FrameDesc, 8U> primary_rx(primary_st);
        SpscConsumer<FrameDesc, 8U> tap_rx(tap_st);
        CHECK(primary_rx.try_pop().value().phys_addr == 0x1000U);
        CHECK(tap_rx.try_pop().value().phys_addr == 0x1000U);

        InterNodeBridge<128U> node_a(1U, BridgeLinkProfile::SpaceWire);
        InterNodeBridge<128U> node_b(2U, BridgeLinkProfile::SpaceWire);
        std::array<std::byte, 128U> wire{};
        const FrameDesc desc{.phys_addr = 0x8'0000'1000ULL, .length_bytes = 4096U, .src_slot_id = 3U};
        const auto wlen = node_a.encode_descriptor(2U, 4U, desc, wire);
        CHECK(wlen.has_value());
        const auto dec = node_b.decode(std::span<const std::byte>{wire.data(), wlen.value()});
        CHECK(dec.has_value() && dec.value().header.src_node == 1U && dec.value().payload.size() == sizeof(FrameDesc));
        wire[wlen.value() - 1U] ^= std::byte{0x01};
        CHECK(node_b.decode(std::span<const std::byte>{wire.data(), wlen.value()}).error() == Status::CrcMismatch);
    }

    // 4. STRATA-CDH-004, 011, 012, 013: ChannelQueryAdapter, TimeConverter, SysMonitor, TextLogSink
    {
        using namespace strata::services::cdh;
        BlackboardStorage<uint32_t> temp_bb{};
        BlackboardWriter<uint32_t>  temp_wr(temp_bb, GenId{1U});
        temp_wr.publish(295U, TaiTimestamp{1'000'000ULL});

        ChannelQueryAdapter<8U> qa{};
        CHECK(qa.register_channel(0x0010U, temp_bb) == Status::Ok);
        const auto qres = qa.query_typed<uint32_t>(0x0010U);
        CHECK(qres.has_value() && qres.value().value == 295U);

        TimeConverter<4U> tc(/*epoch_tai_sec=*/1000ULL, /*tai_utc=*/37, /*max_drift=*/10'000ULL);
        CHECK(tc.add_leap_second(2000ULL, 38) == Status::Ok);
        const auto sclk = tc.tai_to_sclk(TaiTimestamp{1010'500'000'000ULL});
        CHECK(sclk.coarse_sec == 10U);
        const auto back = tc.sclk_to_tai(sclk);
        CHECK(back.ns() >= 1010'499'999'000ULL && back.ns() <= 1010'500'001'000ULL);
        CHECK(tc.tai_to_utc(TaiTimestamp{2010'000'000'000ULL}).tai_minus_utc_sec == 38);
        CHECK(tc.observe_pps(TaiTimestamp{1'000'000'000ULL}) == Status::Ok);
        CHECK(tc.observe_pps(TaiTimestamp{2'000'002'000ULL}) == Status::Ok);
        CHECK(tc.observe_pps(TaiTimestamp{3'000'500'000ULL}) == Status::DeadlineMissed);
        CHECK(tc.drift_anomalies() == 1U);

        BuildManifestInfo bmi{.manifest_hash = 0xABCD'EF01'2345'6789ULL};
        SysMonitor<4U, 4U> sysmon(bmi);
        sysmon.record_partition_frame(0U, 1U, 2000U, 1200U, 150U);
        sysmon.record_pool_usage(0U, 32U, 12U);
        sysmon.record_fabric_and_seu(44U, 2U, 0U);
        CHECK(sysmon.verify_slot_identity(bmi.manifest_hash));
        CHECK(!sysmon.verify_slot_identity(0xDEADULL));
        sysmon.publish(TaiTimestamp{2'000'000ULL});
        BlackboardReader<SysMonitorSnapshot<4U, 4U>> sm_rd(sysmon.storage());
        const auto snap = sm_rd.read(TaiTimestamp{2'000'000ULL});
        CHECK(snap.has_value() && snap.value().value.partitions[0].min_headroom_us == 800U &&
              snap.value().value.pools[0].high_water_slots == 12U &&
              snap.value().value.scsr_hash_mismatch_count == 1U);

        EventRecord ev{.first_tai_ns = 12345U, .last_tai_ns = 12345U, .event_id = 0x0042U,
                       .arg0 = 0x10U, .arg1 = 0x20U, .repeat_count = 1U,
                       .severity = EventSeverity::ActivityLo, .partition_id = 1U};
        size_t lines_emitted = 0U;
        TextLogSink<true>  sink_on{};
        TextLogSink<false> sink_off{};
        sink_on.emit(ev, [&](std::span<const char> line) noexcept {
            if (line.size() > 10U) ++lines_emitted;
        });
        sink_off.emit(ev, [&](std::span<const char>) noexcept { ++lines_emitted; });
        CHECK(lines_emitted == 1U);
    }

    // 5. STRATA-FDIR-003, 004, 007: HealthMonitor, FaultRouter, SlotManager
    {
        using namespace strata::services::fdir;
        BlackboardStorage<runtime::PartitionHeartbeat> hb_bb{};
        BlackboardWriter<runtime::PartitionHeartbeat>  hb_wr(hb_bb, GenId{1U});
        hb_wr.publish(runtime::PartitionHeartbeat{.partition_id = 2U, .beat_count = 1U, .overrun_count = 0U},
                      TaiTimestamp{0U});

        HealthMonitor<4U, 4U> hm{};
        CHECK(hm.register_partition(2U, hb_bb, 10_ms) == Status::Ok);
        CHECK(hm.register_slot(0U, 5_ms, TaiTimestamp{0U}) == Status::Ok);

        std::array<HealthFinding, 8U> findings{};
        auto poll_ok = [](uint16_t) noexcept { return std::pair{SlotState::Active, 0U}; };
        CHECK(hm.evaluate(TaiTimestamp{(2_ms).ns()}, poll_ok, findings) == 0U);
        // Advance past both 5ms slot WDT and 10ms partition heartbeat timeout without updates
        const size_t n = hm.evaluate(TaiTimestamp{(20_ms).ns()}, poll_ok, findings);
        CHECK(n == 2U);

        FaultRouter<8U, 8U> fr(/*warm_limit=*/1U, /*cold_limit=*/1U);
        uint32_t ctl_calls = 0U;
        auto ctl = [&](uint8_t, hyp::PartCtlOp) noexcept { ++ctl_calls; };
        const CrashRecord d_crash{.partition_id = 4U, .criticality = Criticality::ClassD, .kind = TrapKind::DataAbort};
        CHECK(fr.handle_trap(d_crash, ctl) == FaultRoutingDecision::WarmRestartPartition);
        CHECK(fr.handle_trap(d_crash, ctl) == FaultRoutingDecision::ColdRestartPartition);
        // Class D exhausted -> halted in isolation, ZERO spacecraft safe-mode escalations!
        CHECK(fr.handle_trap(d_crash, ctl) == FaultRoutingDecision::HaltPartition);
        CHECK(fr.safe_mode_escalations() == 0U);
        // Unrecoverable Class B TCB fault -> escalates spacecraft safe mode
        const CrashRecord b_crash{.partition_id = 0U, .criticality = Criticality::ClassB, .kind = TrapKind::PrivilegeFault};
        CHECK(fr.handle_trap(b_crash, ctl) == FaultRoutingDecision::WarmRestartPartition);
        CHECK(fr.handle_trap(b_crash, ctl) == FaultRoutingDecision::ColdRestartPartition);
        CHECK(fr.handle_trap(b_crash, ctl) == FaultRoutingDecision::EscalateSpacecraftSafe);
        CHECK(fr.safe_mode_escalations() == 1U && ctl_calls == 6U);

        // SlotManager authenticated end-to-end Versal DFX reload
        MockSlotBackend sb{};
        SlotManager<MockSlotBackend, 4U> sm(sb);
        CHECK(sm.register_slot(1U, SlotFabricKind::VersalPlDfx) == Status::Ok);
        SignedSlotImage img{
            .image_id = 77U, .expected_manifest_hash = sb.hw_hash, .staged_in_verified_vol = true};
        CHECK(sm.load_and_verify(1U, img) == Status::Ok);
        CHECK(sm.inspect(1U).value().state == SlotState::Standby);
        CHECK(sm.inspect(1U).value().gen.value == 1U);
        CHECK(sb.scrub_pauses == 1U && sb.scrub_reinits == 1U && sb.loads == 1U);
        CHECK(sm.activate(1U) == Status::Ok);
        CHECK(sm.inspect(1U).value().state == SlotState::Active);
        // Tampered signature rejected
        sb.sig_ok = false;
        CHECK(sm.load_and_verify(1U, img) == Status::PermissionDenied);
    }

    // 6. STRATA-STOR-003, 004, 005, 006 & STRATA-AUTO-002
    {
        using namespace strata::services::storage;
        MockPlCompressor comp_hw{};
        PlCompressionStage<MockPlCompressor> stage(comp_hw, PlCompressionAlg::Ccsds123B2);
        DpHeader hdr{};
        const auto cdesc = stage.process_frame(FrameDesc{.phys_addr = 0x1000U, .length_bytes = 4096U, .crc32c = 0x1234U},
                                               0x2000U, hdr);
        CHECK(cdesc.has_value() && cdesc.value().length_bytes == 1024U && hdr.compression_alg == 1U);

        // FileService full ops: commit, list, verify_crc32c, move, delete
        FileService<4U, 8U> fs{};
        CHECK(fs.register_volume(StorageVolume{0U, 2U, 10'000U, 0U, false, true}) == Status::Ok);
        CHECK(fs.register_volume(StorageVolume{1U, 2U, 10'000U, 0U, true, true}) == Status::Ok);
        std::array<std::byte, 4U> fdata{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        const uint32_t fcrc = Crc32c::compute(fdata);
        const auto fid = fs.commit_file(2U, 0U, 4096U, fcrc);
        CHECK(fid.has_value());
        CHECK(fs.verify_crc32c(fid.value(), fdata) == Status::Ok);
        std::array<FileService<4U, 8U>::FileEntry, 4U> list_out{};
        CHECK(fs.list_directory(2U, 0U, list_out).value() == 1U);
        CHECK(fs.move_file(2U, fid.value(), 1U) == Status::Ok);
        CHECK(fs.volume_used_bytes(0U).value() == 0U && fs.volume_used_bytes(1U).value() == 4096U);
        CHECK(fs.authenticate_dfx_stage(fid.value(), fcrc) == Status::Ok);
        CHECK(fs.delete_file(2U, fid.value()) == Status::Ok);
        CHECK(fs.volume_used_bytes(1U).value() == 0U);

        // CfdpTxTransaction Class 2 NAK selective retransmit + suspend/resume checkpoint
        CfdpTxTransaction<8U> tx{};
        tx.start(99U, 3000U, 1000U, true);
        const auto s0 = tx.next_segment();
        CHECK(s0.has_value() && s0.value().offset == 0U && s0.value().length == 1000U);
        const auto cp = tx.checkpoint();
        CfdpTxTransaction<8U> tx_resumed{};
        tx_resumed.restore(cp, 1000U);
        std::array<ByteExtent, 1U> nak{ByteExtent{0U, 500U}};
        CHECK(tx_resumed.on_nak(nak) == Status::Ok);
        const auto retx = tx_resumed.next_segment();
        CHECK(retx.has_value() && retx.value().is_retransmit && retx.value().offset == 0U &&
              retx.value().length == 500U);
        CHECK(tx_resumed.next_segment().value().offset == 1000U);
        CHECK(tx_resumed.next_segment().value().offset == 2000U);
        CHECK(tx_resumed.next_segment().value().is_eof);
        CHECK(!tx_resumed.is_complete());
        tx_resumed.on_finished_ack();
        CHECK(tx_resumed.is_complete());

        // FastFileChunker + inter-file cooldown
        FastFileChunker<64U> chunker(5_ms);
        std::array<std::byte, 128U> cwire{};
        CHECK(chunker.begin_file(10U, 1U, TaiTimestamp{0U}) == Status::Ok);
        const auto cw = chunker.encode_chunk(fdata, cwire, TaiTimestamp{(1_ms).ns()});
        CHECK(cw.has_value());
        const auto cdec = FastFileChunker<64U>::decode_chunk(std::span<const std::byte>{cwire.data(), cw.value()});
        CHECK(cdec.has_value() && cdec.value().header.file_id == 10U);
        // Immediate next file blocked by 5ms cooldown, succeeds at 7ms
        CHECK(chunker.begin_file(11U, 1U, TaiTimestamp{(2_ms).ns()}) == Status::Busy);
        CHECK(chunker.begin_file(11U, 1U, TaiTimestamp{(7_ms).ns()}) == Status::Ok);

        // STRATA-AUTO-002: SignedAotModuleLoader + AutonomySandbox execution
        using namespace strata::services::autonomy;
        std::array<SandboxInstr, 3U> prog{
            SandboxInstr{SandboxOp::ReadTlmGt, 0U, 0U, 100},
            SandboxInstr{SandboxOp::EmitCmdIf, 0U, 0x0201U, 500},
            SandboxInstr{SandboxOp::Halt, 0U, 0U, 0},
        };
        const auto code_span = std::as_bytes(std::span<const SandboxInstr>{prog});
        SignedAotModuleHeader mhdr{
            .abi_version       = 1U,
            .instruction_count = 3U,
            .module_id         = 7U,
            .max_steps_bound   = 32U,
            .manifest_hash     = 0xFEED'FACE'CAFE'BEEFULL,
            .code_crc32c       = Crc32c::compute(code_span),
        };
        ReferenceAotVerifier verifier{};
        ReferenceAotVerifier::compute_tag(verifier.trusted_key_seed, mhdr, code_span, mhdr.signature_tag);

        std::array<std::byte, sizeof(mhdr) + sizeof(prog)> img_blob{};
        std::memcpy(img_blob.data(), &mhdr, sizeof(mhdr));
        std::memcpy(img_blob.data() + sizeof(mhdr), code_span.data(), code_span.size());

        SignedAotModuleLoader<> loader(0xFEED'FACE'CAFE'BEEFULL, verifier);
        CHECK(loader.load_from_image(img_blob) == Status::Ok);
        CHECK(loader.is_loaded());

        AutonomySandbox<> sb{};
        CHECK(sb.allow_opcode(0x0201U) == Status::Ok);
        std::array<int64_t, 1U> tlm{250};
        uint32_t cmds = 0U;
        const auto run = sb.execute(loader.loaded_program(), tlm, loader.header().max_steps_bound,
                                    [&](uint16_t, int64_t) noexcept { ++cmds; });
        CHECK(run.halted_cleanly && run.cmds_forwarded == 1U && cmds == 1U);

        // Tamper signature -> rejected
        img_blob[sizeof(mhdr) - 1U] ^= std::byte{0xFF};
        CHECK(loader.load_from_image(img_blob) == Status::PermissionDenied);
    }

    std::printf("PASS: test_extended_services (MW-016, COM-004/005/009/010, CDH-004/011/012/013, FDIR-003/004/007, STOR-003..006, AUTO-002)\n");
    return 0;
}
