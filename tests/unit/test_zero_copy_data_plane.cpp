#include <strata/core/crc32c.hpp>
#include <strata/data/data_topic.hpp>
#include <strata/data/frame_lease.hpp>

#include <array>
#include <cstddef>
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

int test_linear_lease_auto_return() {
    using Pool = strata::FramePool<4U, 256U>;
    std::array<std::byte, 4U * 256U> ddr{};
    Pool pool(ddr, 0x8000'0000ULL);
    CHECK(pool.free_count() == 4U);

    {
        auto slot = pool.allocate_slot(strata::GenId{1U});
        CHECK(slot.has_value());
        CHECK(pool.free_count() == 3U);

        strata::FrameDesc desc{
            .phys_addr     = pool.slot_phys_addr(*slot),
            .tai_ns        = 1000U,
            .length_bytes  = 64U,
            .pool_slot_idx = *slot,
            .src_slot_id   = 0U,
            .producer_gen  = 1U,
        };
        strata::FrameLease<Pool> lease(pool, desc);
        // Scope exit without publish() MUST automatically return slot (zero leak!)
    }
    CHECK(pool.free_count() == 4U);
    return 0;
}

int test_zero_copy_fanout_and_crash_reclamation() {
    using Pool  = strata::FramePool<4U, 512U>;
    using Topic = strata::DataTopic<Pool, 4U, 4U>;

    std::array<std::byte, 4U * 512U> ddr{};
    Pool  pool(ddr, 0x0000'0008'0000'0000ULL);
    Topic topic(pool);

    // Sub 0: Class B/C Guaranteed Recorder (no lease expiry, lossless)
    auto sub_rec = topic.add_subscriber(strata::kGuaranteedControlQoS);
    // Sub 1: Class D Best-Effort ML Payload (50 ms lease timeout, DropOldest)
    auto sub_ml  = topic.add_subscriber(strata::kBestEffortScienceQoS);
    CHECK(sub_rec.has_value() && sub_ml.has_value());

    // Produce 1 frame directly in DDR4 pool slot
    auto slot_idx = pool.allocate_slot(strata::GenId{1U});
    CHECK(slot_idx.has_value());
    strata::FrameDesc desc{
        .phys_addr     = pool.slot_phys_addr(*slot_idx),
        .tai_ns        = (100_ms).ns(),
        .length_bytes  = 128U,
        .pool_slot_idx = *slot_idx,
        .src_slot_id   = 0U,
        .producer_gen  = 1U,
    };
    strata::FrameLease<Pool> lease(pool, desc);
    auto buf = lease.writable_bytes();
    for (size_t i = 0U; i < 128U; ++i) {
        buf[i] = static_cast<std::byte>(i ^ 0xA5U);
    }
    const std::byte* original_ptr = buf.data();
    lease.mutable_desc().crc32c   = strata::Crc32c::compute(buf.subspan(0U, 128U));

    CHECK(topic.publish(std::move(lease), strata::TaiTimestamp{(100_ms).ns()}) == strata::Status::Ok);
    CHECK(pool.free_count() == 3U);

    // Both subscribers receive TRUE ZERO-COPY Read-Only views pointing to the exact same DDR byte!
    {
        auto v_rec = topic.try_receive(*sub_rec);
        auto v_ml  = topic.try_receive(*sub_ml);
        CHECK(v_rec.has_value() && v_ml.has_value());
        CHECK(v_rec->bytes().data() == original_ptr);
        CHECK(v_ml->bytes().data() == original_ptr);
        CHECK(strata::Crc32c::compute(v_rec->bytes()) == v_rec->desc().crc32c);

        // Recorder finishes and drops its view -> slot STILL held by Class D ML subscriber
        v_rec->release();
        CHECK(pool.free_count() == 3U);

        // Simulate Class D ML subscriber hanging past its 50 ms lease_timeout!
        CHECK(topic.reclaim_expired(strata::TaiTimestamp{(140_ms).ns()}) == 0U);
        CHECK(topic.reclaim_expired(strata::TaiTimestamp{(155_ms).ns()}) == 1U);
        CHECK(pool.free_count() == 4U); // Reclaimed cleanly without leaking!

        // When hung/restarted Class D view later releases, idempotent bitmask prevents double-free!
        v_ml->release();
        CHECK(pool.free_count() == 4U);
    }

    // Now simulate Class D ML subscriber completely stalled (never pops its ring):
    // Class B/C Recorder MUST continue receiving all 6 frames with ZERO stalls/drops!
    for (uint32_t f = 0U; f < 6U; ++f) {
        auto s = pool.allocate_slot(strata::GenId{1U});
        CHECK(s.has_value());
        strata::FrameDesc d{
            .phys_addr     = pool.slot_phys_addr(*s),
            .tai_ns        = (200_ms).ns() + (f * (10_ms).ns()),
            .length_bytes  = 64U,
            .pool_slot_idx = *s,
            .src_slot_id   = 0U,
            .producer_gen  = 1U,
        };
        strata::FrameLease<Pool> l(pool, d);
        const strata::TaiTimestamp t{d.tai_ns};
        CHECK(topic.publish(std::move(l), t) == strata::Status::Ok);

        // Recorder consumes immediately
        auto vr = topic.try_receive(*sub_rec);
        CHECK(vr.has_value());
        vr->release();

        // FDIR periodically reclaims expired Class D leases
        topic.reclaim_expired(t + 60_ms);
    }

    CHECK(topic.subscriber_drops(*sub_rec) == 0U);
    CHECK(topic.subscriber_drops(*sub_ml) >= 2U); // BestEffortDropOldest shed safely
    CHECK(topic.reset_crashed_subscriber(*sub_ml, strata::GenId{2U}) <= 4U);
    CHECK(pool.free_count() == 4U);
    return 0;
}

} // namespace

int main() {
    if (test_linear_lease_auto_return() != 0) return 1;
    if (test_zero_copy_fanout_and_crash_reclamation() != 0) return 1;
    std::printf("PASS: strata.unit.zero_copy_data_plane\n");
    return 0;
}
