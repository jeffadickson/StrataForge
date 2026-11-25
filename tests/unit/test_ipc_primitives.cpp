#include <strata/ipc/blackboard.hpp>
#include <strata/ipc/critical_store.hpp>
#include <strata/ipc/event_ring.hpp>
#include <strata/ipc/param_bank.hpp>
#include <strata/ipc/spsc_channel.hpp>

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

struct NavHk {
    int32_t  temp_mdeg{0};
    uint32_t frames_ok{0U};
};

struct MockNotifier {
    uint32_t rings{0U};
    uint32_t last_channel{0U};
    void notify(uint32_t ch) noexcept {
        ++rings;
        last_channel = ch;
    }
};

struct CalibParams {
    uint32_t exposure_us{1000U};
    uint16_t gain{2U};
    uint16_t pad{0U};
};

struct CheckpointState {
    uint32_t scan_line{0U};
    uint32_t mode{0U};
};

int test_blackboard() {
    strata::BlackboardStorage<NavHk> storage{};

    constexpr strata::QoSConfig kNavQoS{
        .durability    = strata::Durability::TransientLocal,
        .reliability   = strata::Reliability::BestEffortDropOldest,
        .deadline      = 20_ms,
        .liveliness    = strata::DurationNs{0U},
        .lease_timeout = strata::DurationNs{0U},
        .history_depth = 1U,
    };

    strata::BlackboardWriter<NavHk, kNavQoS> writer(storage, strata::GenId{3U});
    strata::BlackboardReader<NavHk, kNavQoS> reader(storage);

    // No publish yet
    auto empty = reader.read(strata::TaiTimestamp{(10_ms).ns()});
    CHECK(!empty.has_value());
    CHECK(empty.error() == strata::Status::NoDataYet);

    writer.publish(NavHk{.temp_mdeg = 24'500, .frames_ok = 77U}, strata::TaiTimestamp{(100_ms).ns()});

    // Fresh read within 20 ms deadline
    auto fresh = reader.read(strata::TaiTimestamp{(115_ms).ns()});
    CHECK(fresh.has_value());
    CHECK(fresh->value.temp_mdeg == 24'500);
    CHECK(fresh->value.frames_ok == 77U);
    CHECK(fresh->writer_gen == strata::GenId{3U});

    // Stale read past 20 ms deadline fails explicitly with DeadlineMissed
    auto stale = reader.read(strata::TaiTimestamp{(125_ms).ns()});
    CHECK(!stale.has_value());
    CHECK(stale.error() == strata::Status::DeadlineMissed);

    // Restarted TransientLocal reader immediately sees last valid sample
    strata::BlackboardReader<NavHk, kNavQoS> restarted(storage);
    auto caught_up = restarted.read(strata::TaiTimestamp{(110_ms).ns()});
    CHECK(caught_up.has_value());
    CHECK(caught_up->value.frames_ok == 77U);

    // Volatile reader ignores pre-attach sample until a fresh publish arrives
    constexpr strata::QoSConfig kVolQoS{.durability = strata::Durability::Volatile};
    strata::BlackboardReader<NavHk, kVolQoS> vol_reader(storage);
    auto vol_none = vol_reader.read(strata::TaiTimestamp{(110_ms).ns()});
    CHECK(!vol_none.has_value());
    CHECK(vol_none.error() == strata::Status::NoDataYet);
    writer.publish(NavHk{.temp_mdeg = 25'000, .frames_ok = 78U}, strata::TaiTimestamp{(112_ms).ns()});
    auto vol_ok = vol_reader.read(strata::TaiTimestamp{(112_ms).ns()});
    CHECK(vol_ok.has_value());
    CHECK(vol_ok->value.frames_ok == 78U);
    return 0;
}

int test_spsc_channel() {
    strata::SpscRingStorage<uint32_t, 4U> storage{};
    storage.reset(strata::GenId{1U}, strata::GenId{1U});

    MockNotifier notifier{};

    // Use value notifier and test coalesced doorbell via reference wrapper
    struct RefNotifier {
        MockNotifier* target;
        void notify(uint32_t ch) noexcept { target->notify(ch); }
    };

    strata::SpscProducer<uint32_t, 4U, strata::kGuaranteedControlQoS, RefNotifier> prod(
        storage, 9U, RefNotifier{&notifier}, strata::GenId{1U});
    strata::SpscConsumer<uint32_t, 4U, strata::kGuaranteedControlQoS> cons(storage,
                                                                           strata::GenId{1U});

    cons.arm_notify();
    CHECK(prod.try_push(10U) == strata::Status::Ok);
    CHECK(prod.try_push(20U) == strata::Status::Ok);
    CHECK(prod.try_push(30U) == strata::Status::Ok);
    CHECK(prod.try_push(40U) == strata::Status::Ok);
    CHECK(prod.try_push(50U) == strata::Status::Full);
    // Interrupt coalescing: 4 pushes produced exactly 1 doorbell ring!
    CHECK(notifier.rings == 1U);
    CHECK(notifier.last_channel == 9U);

    // Bounded WCET drain (max 2 items)
    uint32_t sum = 0U;
    const uint32_t drained = cons.drain_up_to(2U, [&](uint32_t v) noexcept { sum += v; });
    CHECK(drained == 2U);
    CHECK(sum == 30U);
    CHECK(cons.available() == 2U);

    // Partition generation restart: consumer restarts -> producer gets StaleGeneration until acked
    cons.advance_consumer_generation(strata::GenId{2U});
    CHECK(prod.try_push(60U) == strata::Status::StaleGeneration);
    prod.acknowledge_consumer_generation(strata::GenId{2U});
    CHECK(prod.try_push(60U) == strata::Status::Ok);
    return 0;
}

int test_event_ring() {
    strata::SpscRingStorage<strata::EventRecord, 8U> storage{};
    storage.reset(strata::GenId{1U}, strata::GenId{1U});

    strata::EventWriter<8U> writer(storage, 2U, 100_ms);
    strata::SpscConsumer<strata::EventRecord, 8U> reader(storage, strata::GenId{0U});

    // Burst of 5 identical warnings within 100 ms cooldown -> only 1 emitted immediately
    CHECK(writer.emit(42U, strata::EventSeverity::WarningHi, strata::TaiTimestamp{(10_ms).ns()}) ==
          strata::Status::Ok);
    CHECK(writer.emit(42U, strata::EventSeverity::WarningHi, strata::TaiTimestamp{(20_ms).ns()}) ==
          strata::Status::Ok);
    CHECK(writer.emit(42U, strata::EventSeverity::WarningHi, strata::TaiTimestamp{(30_ms).ns()}) ==
          strata::Status::Ok);
    CHECK(writer.suppressed_count() == 2U);
    CHECK(reader.available() == 1U);

    // Next emit after cooldown expires carries repeat_count = 3 (1 new + 2 suppressed)
    CHECK(writer.emit(42U, strata::EventSeverity::WarningHi, strata::TaiTimestamp{(120_ms).ns()}) ==
          strata::Status::Ok);
    auto r1 = reader.try_pop();
    auto r2 = reader.try_pop();
    CHECK(r1.has_value() && r2.has_value());
    CHECK(r1->repeat_count == 1U);
    CHECK(r2->repeat_count == 3U);

    // Severity filtering: disable Diagnostic, verify Fatal still bypasses filter & cooldown
    writer.set_severity_mask(0x00U);
    CHECK(writer.emit(99U, strata::EventSeverity::Diagnostic, strata::TaiTimestamp{(200_ms).ns()}) ==
          strata::Status::Ok);
    CHECK(writer.filtered_count() == 1U);
    CHECK(writer.emit(100U, strata::EventSeverity::Fatal, strata::TaiTimestamp{(201_ms).ns()}) ==
          strata::Status::Ok);
    CHECK(reader.available() == 1U);
    return 0;
}

int test_param_bank() {
    strata::ParamBank<CalibParams> bank(CalibParams{.exposure_us = 1000U, .gain = 2U});
    CHECK(bank.active().exposure_us == 1000U);
    CHECK(bank.version() == 1U);
    CHECK(bank.verify_active());

    // Invalid candidate rejected before touching active bank
    auto bad = bank.stage(CalibParams{.exposure_us = 999'999U, .gain = 2U},
                          [](const CalibParams& p) noexcept { return p.exposure_us <= 50'000U; });
    CHECK(bad == strata::Status::ValidationFailed);
    CHECK(bank.active().exposure_us == 1000U);

    // Stage valid update -> still inactive until atomic commit
    CHECK(bank.stage(CalibParams{.exposure_us = 4000U, .gain = 8U}) == strata::Status::Ok);
    CHECK(bank.active().exposure_us == 1000U);
    CHECK(bank.commit() == strata::Status::Ok);
    CHECK(bank.active().exposure_us == 4000U);
    CHECK(bank.active().gain == 8U);
    CHECK(bank.version() == 2U);

    // Rollback restores version 1 cleanly
    CHECK(bank.rollback() == strata::Status::Ok);
    CHECK(bank.active().exposure_us == 1000U);
    CHECK(bank.version() == 1U);
    return 0;
}

int test_critical_store() {
    strata::CriticalStoreStorage<CheckpointState> raw_mem{};
    constexpr uint64_t kHash = 0x1122'3344'5566'7788ULL;

    strata::CriticalStore<CheckpointState> cds(raw_mem, kHash);
    CHECK(!cds.restore().has_value());

    cds.checkpoint(CheckpointState{.scan_line = 100U, .mode = 1U});
    cds.checkpoint(CheckpointState{.scan_line = 250U, .mode = 2U});
    CHECK(cds.latest_sequence() == 2U);

    auto r_latest = cds.restore();
    CHECK(r_latest.has_value());
    CHECK(r_latest->scan_line == 250U);

    // Fault injection: corrupt the newest ping-pong slot (slot 1) as if an SEU hit mid-write!
    raw_mem.slots[1].payload.scan_line ^= 0x0001'0000U;

    // WarmStart recovery automatically falls back to intact prior CRC-valid checkpoint (slot 0)!
    auto r_recovered = cds.restore();
    CHECK(r_recovered.has_value());
    CHECK(r_recovered->scan_line == 100U);
    CHECK(r_recovered->mode == 1U);

    // Manifest hash mismatch rejects stale checkpoint from a previous flight image
    strata::CriticalStore<CheckpointState> wrong_hash_cds(raw_mem, kHash ^ 1ULL);
    CHECK(!wrong_hash_cds.restore().has_value());
    return 0;
}

} // namespace

int main() {
    if (test_blackboard() != 0) return 1;
    if (test_spsc_channel() != 0) return 1;
    if (test_event_ring() != 0) return 1;
    if (test_param_bank() != 0) return 1;
    if (test_critical_store() != 0) return 1;
    std::printf("PASS: strata.unit.ipc_primitives\n");
    return 0;
}
