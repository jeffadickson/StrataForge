#include <strata/services/comms/ccsds.hpp>
#include <strata/services/comms/com_pipeline.hpp>

#include <array>
#include <cstddef>
#include <cstdio>

namespace {

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                       \
        }                                                                                   \
    } while (false)

int test_ccsds_total_function_parsers() {
    using namespace strata::services::comms;

    // 1. Truncated 0..6 byte wire frames MUST return WireErr cleanly — never assert or abort.
    std::array<std::byte, 6U> tiny{};
    for (size_t len = 0U; len < 6U; ++len) {
        auto r = parse_space_packet(std::span<const std::byte>(tiny.data(), len));
        CHECK(!r.has_value());
        CHECK(r.error() == WireErr::TruncatedHeader);
    }
    // 6-byte header claiming 1 byte of payload with 0 bytes remaining -> LengthMismatch
    auto r6 = parse_space_packet(tiny);
    CHECK(!r6.has_value());
    CHECK(r6.error() == WireErr::LengthMismatch);

    // 2. Round-trip Space Packet inside TC Transfer Frame with CRC-16-CCITT FECF
    std::array<std::byte, 4U> payload{
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
    };
    std::array<std::byte, 64U> sp_buf{};
    auto sp_len = frame_space_packet(sp_buf, 0x0123U, 42U, true, true, payload);
    CHECK(sp_len.has_value());

    std::array<std::byte, 128U> tc_buf{};
    auto tc_len = frame_tc_frame(tc_buf, 0x02AAU, 3U, 9U, false, false,
                                 std::span<const std::byte>(sp_buf.data(), *sp_len));
    CHECK(tc_len.has_value());

    auto tc_parsed = parse_tc_frame(std::span<const std::byte>(tc_buf.data(), *tc_len));
    CHECK(tc_parsed.has_value());
    CHECK(tc_parsed->scid == 0x02AAU);
    CHECK(tc_parsed->vcid == 3U);
    CHECK(tc_parsed->frame_seq == 9U);

    auto sp_parsed = parse_space_packet(tc_parsed->data_field);
    CHECK(sp_parsed.has_value());
    CHECK(sp_parsed->apid == 0x0123U);
    CHECK(sp_parsed->seq_count == 42U);
    CHECK(sp_parsed->is_tc);
    CHECK(sp_parsed->payload.size() == 4U);
    CHECK(sp_parsed->payload[0] == std::byte{0xDE});

    // 3. Bit-flip fault injection in TC frame -> FECF CRC mismatch rejected cleanly
    tc_buf[6] = static_cast<std::byte>(std::to_integer<uint8_t>(tc_buf[6]) ^ 0x01U);
    auto tc_bad = parse_tc_frame(std::span<const std::byte>(tc_buf.data(), *tc_len));
    CHECK(!tc_bad.has_value());
    CHECK(tc_bad.error() == WireErr::FecfCrcMismatch);
    return 0;
}

int test_sdls_and_anti_replay() {
    using namespace strata::services::comms;

    SdlsEngine<> tx{};
    SdlsEngine<> rx{};
    std::array<std::byte, 32U> key{};
    for (size_t i = 0U; i < 32U; ++i) {
        key[i] = static_cast<std::byte>(i * 7U + 3U);
    }
    CHECK(tx.install_sa(0x0001U, key) == strata::Status::Ok);
    CHECK(rx.install_sa(0x0001U, key) == strata::Status::Ok);

    std::array<std::byte, 4U>  aad{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::array<std::byte, 8U>  pt{std::byte{'S'}, std::byte{'T'}, std::byte{'R'}, std::byte{'A'},
                                  std::byte{'T'}, std::byte{'A'}, std::byte{'2'}, std::byte{'0'}};
    std::array<std::byte, 64U> wire{};
    std::array<std::byte, 64U> dec{};

    auto enc_len = tx.protect(0x0001U, aad, pt, wire);
    CHECK(enc_len.has_value());

    // Valid decrypt succeeds
    auto dec_len = rx.unprotect(aad, std::span<const std::byte>(wire.data(), *enc_len), dec);
    CHECK(dec_len.has_value());
    CHECK(*dec_len == pt.size());
    CHECK(dec[0] == std::byte{'S'} && dec[7] == std::byte{'0'});

    // Exact replay rejected by 64-frame sliding anti-replay window!
    auto replay = rx.unprotect(aad, std::span<const std::byte>(wire.data(), *enc_len), dec);
    CHECK(!replay.has_value());
    CHECK(replay.error() == WireErr::ReplayDetected);

    // Fresh packet tampered in transit rejected with AuthTagMismatch (and does NOT poison window)
    auto enc_len2 = tx.protect(0x0001U, aad, pt, wire);
    CHECK(enc_len2.has_value());
    wire[12] = static_cast<std::byte>(std::to_integer<uint8_t>(wire[12]) ^ 0x80U);
    auto tamper = rx.unprotect(aad, std::span<const std::byte>(wire.data(), *enc_len2), dec);
    CHECK(!tamper.has_value());
    CHECK(tamper.error() == WireErr::AuthTagMismatch);

    // Restoring bit lets authentic packet pass since failed tamper did NOT consume sequence number
    wire[12] = static_cast<std::byte>(std::to_integer<uint8_t>(wire[12]) ^ 0x80U);
    auto ok2 = rx.unprotect(aad, std::span<const std::byte>(wire.data(), *enc_len2), dec);
    CHECK(ok2.has_value());
    return 0;
}

int test_com_queue_and_aggregator() {
    using namespace strata::services::comms;

    ComQueue<4U, 64U> q{};
    std::array<std::byte, 8U> bulk{std::byte{0xBB}};
    std::array<std::byte, 8U> hk{std::byte{0xAA}};
    CHECK(q.enqueue(ComPriority::BulkDp, bulk) == strata::Status::Ok);
    CHECK(q.enqueue(ComPriority::CriticalHk, hk) == strata::Status::Ok);

    // CriticalHk MUST dequeue before BulkDp regardless of arrival order
    std::array<std::byte, 64U> out{};
    auto d1 = q.dequeue_highest(out);
    CHECK(d1.has_value() && out[0] == std::byte{0xAA});
    auto d2 = q.dequeue_highest(out);
    CHECK(d2.has_value() && out[0] == std::byte{0xBB});

    // ComAggregator packs real packet + appends valid CCSDS Idle Packet (APID 0x7FF) to fill frame
    ComAggregator<64U> agg{};
    std::array<std::byte, 10U> pkt_payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                           std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
                                           std::byte{9}, std::byte{10}};
    std::array<std::byte, 32U> sp{};
    auto spl = frame_space_packet(sp, 0x0055U, 1U, false, false, pkt_payload);
    CHECK(spl.has_value());
    CHECK(agg.append(std::span<const std::byte>(sp.data(), *spl)) == strata::Status::Ok);

    auto frame_df = agg.flush_with_idle_fill();
    auto p1       = parse_space_packet(frame_df);
    CHECK(p1.has_value() && p1->apid == 0x0055U);
    auto p_idle = parse_space_packet(frame_df.subspan(*spl));
    CHECK(p_idle.has_value() && p_idle->apid == kIdleApid);
    CHECK(*spl + kSpacePacketHeaderBytes + p_idle->payload.size() == 64U);
    return 0;
}

} // namespace

int main() {
    if (test_ccsds_total_function_parsers() != 0) return 1;
    if (test_sdls_and_anti_replay() != 0) return 1;
    if (test_com_queue_and_aggregator() != 0) return 1;
    std::printf("PASS: strata.unit.comms_ccsds_sdls\n");
    return 0;
}
