#include <strata/services/autonomy/sandbox_runner.hpp>
#include <strata/services/storage/data_products.hpp>
#include <strata/services/storage/file_and_cfdp.hpp>

#include <array>
#include <cstdio>

namespace {

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                       \
        }                                                                                   \
    } while (false)

int test_dp_catalog_priority_and_eviction() {
    using namespace strata::services::storage;

    // 10 GiB solid-state recorder (64-bit sizes, well beyond F' 4 GiB limit)
    constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
    DpCatalog<16U> cat(10ULL * kGiB);

    // Fill with 6 GiB low-priority diagnostic DP (priority=200) and 4 GiB medium DP (priority=50)
    auto p_low = cat.register_product(1U, 200U, 1U, strata::TaiTimestamp{10U}, 6ULL * kGiB, 0U, 1U);
    auto p_med = cat.register_product(2U, 50U, 1U, strata::TaiTimestamp{20U}, 4ULL * kGiB, 6ULL * kGiB, 2U);
    CHECK(p_low.has_value() && p_med.has_value());
    CHECK(cat.used_bytes() == 10ULL * kGiB);

    // New 5 GiB high-priority science DP (priority=10) automatically evicts low-priority 6 GiB DP!
    auto p_hi = cat.register_product(3U, 10U, 1U, strata::TaiTimestamp{30U}, 5ULL * kGiB, 0U, 3U);
    CHECK(p_hi.has_value());
    CHECK(cat.evicted_count() == 1U);
    CHECK(cat.used_bytes() == 9ULL * kGiB);

    // Downlink selector MUST pick priority=10 product first
    auto next = cat.select_next_downlink();
    CHECK(next.has_value() && next->product_id == *p_hi);
    CHECK(cat.advance_downlink(*p_hi, 5ULL * kGiB) == strata::Status::Ok);

    // Next downlink picks priority=50 product
    auto next2 = cat.select_next_downlink();
    CHECK(next2.has_value() && next2->product_id == *p_med);
    return 0;
}

int test_file_service_and_cfdp_64bit() {
    using namespace strata::services::storage;

    constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
    FileService<> fs{};
    CHECK(fs.register_volume(StorageVolume{
              .volume_id         = 1U,
              .owner_partition   = 2U,
              .quota_bytes       = 12ULL * kGiB,
              .allow_dfx_staging = true,
          }) == strata::Status::Ok);

    // Cross-partition unauthorized access blocked by volume jail
    auto denied = fs.commit_file(9U, 1U, 1024U, 0x1234U);
    CHECK(!denied.has_value() && denied.error() == strata::Status::PermissionDenied);

    // 6 GiB (>4 GiB) file commit & authenticated DFX staging
    auto fid = fs.commit_file(2U, 1U, 6ULL * kGiB, 0xABCD'EF01U);
    CHECK(fid.has_value());
    CHECK(fs.authenticate_dfx_stage(*fid, 0x0000'0000U) == strata::Status::CrcMismatch);
    CHECK(!fs.is_verified_dfx_image(*fid));
    CHECK(fs.authenticate_dfx_stage(*fid, 0xABCD'EF01U) == strata::Status::Ok);
    CHECK(fs.is_verified_dfx_image(*fid));

    // 64-bit CCSDS CFDP Class 2 receiver with out-of-order packets & NAK gap computation
    CfdpRxTransaction<> cfdp{};
    constexpr uint64_t kFileLen = 5ULL * kGiB; // > 4 GiB!
    cfdp.start(42U, kFileLen, true);
    CHECK(cfdp.on_file_data(0ULL, 2ULL * kGiB) == strata::Status::Ok);
    CHECK(cfdp.on_file_data(3ULL * kGiB, 2ULL * kGiB) == strata::Status::Ok);
    cfdp.on_eof();
    CHECK(!cfdp.is_complete());

    // Exact missing extent [2 GiB, 3 GiB) reported for Class 2 NAK PDU
    std::array<ByteExtent, 4U> gaps{};
    const size_t n = cfdp.collect_missing_extents(gaps);
    CHECK(n == 1U);
    CHECK(gaps[0].start == 2ULL * kGiB && gaps[0].end == 3ULL * kGiB);

    // Retransmitted missing extent arrives -> transaction completes cleanly!
    CHECK(cfdp.on_file_data(2ULL * kGiB, 1ULL * kGiB) == strata::Status::Ok);
    CHECK(cfdp.is_complete());
    return 0;
}

int test_autonomy_sandbox() {
    using namespace strata::services::autonomy;

    AutonomySandbox<> sb{};
    CHECK(sb.allow_opcode(0x0201U) == strata::Status::Ok); // Allowed: Start Science Capture

    // Script 1: Attempts both an allowed science cmd (0x0201) and a FORBIDDEN pyro cmd (0x0999)
    std::array<SandboxInstr, 4U> prog1{{
        {SandboxOp::ReadTlmGt, 0U, 0U, 100},
        {SandboxOp::EmitCmdIf, 0U, 0x0201U, 1}, // Allowed
        {SandboxOp::EmitCmdIf, 0U, 0x0999U, 1}, // Forbidden -> MUST be blocked!
        {SandboxOp::Halt, 0U, 0U, 0},
    }};
    std::array<int64_t, 2U> tlm{150, 0};

    uint16_t last_cmd = 0U;
    auto r1 = sb.execute(prog1, tlm, 32U, [&](uint16_t op, int64_t) noexcept { last_cmd = op; });
    CHECK(r1.halted_cleanly && !r1.budget_preempted);
    CHECK(r1.cmds_forwarded == 1U && last_cmd == 0x0201U);
    CHECK(r1.cmds_blocked == 1U);

    // Script 2: Malicious infinite loop -> cleanly preempted at step budget!
    std::array<SandboxInstr, 2U> rogue_loop{{
        {SandboxOp::Nop, 0U, 0U, 0},
        {SandboxOp::Jump, 0U, 0U, 0}, // Infinite loop back to PC=0
    }};
    auto r2 = sb.execute(rogue_loop, tlm, 50U, [](uint16_t, int64_t) noexcept {});
    CHECK(r2.budget_preempted && !r2.halted_cleanly);
    CHECK(r2.steps_executed == 50U);
    return 0;
}

} // namespace

int main() {
    if (test_dp_catalog_priority_and_eviction() != 0) return 1;
    if (test_file_service_and_cfdp_64bit() != 0) return 1;
    if (test_autonomy_sandbox() != 0) return 1;
    std::printf("PASS: strata.unit.storage_and_autonomy\n");
    return 0;
}
