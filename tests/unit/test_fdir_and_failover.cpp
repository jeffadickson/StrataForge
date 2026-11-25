#include <strata/services/fdir/fdir_engine.hpp>

#include <cstdio>

namespace {

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                       \
        }                                                                                   \
    } while (false)

int test_watchpoint_actionpoint_and_failover_escalation() {
    using namespace strata::services::fdir;

    FdirEngine<> fdir{};

    // WP 0: Focal plane temp > 85,000 mdegC for 2 consecutive ticks
    CHECK(fdir.add_watchpoint(WatchpointDef{
              .wp_id          = 0U,
              .channel_id     = 10U,
              .op             = CompareOp::GreaterThan,
              .high_limit     = 85'000,
              .debounce_ticks = 2U,
          }) == strata::Status::Ok);

    // WP 1: Current > 4,000 mA for 1 tick
    CHECK(fdir.add_watchpoint(WatchpointDef{
              .wp_id          = 1U,
              .channel_id     = 11U,
              .op             = CompareOp::GreaterThan,
              .high_limit     = 4'000,
              .debounce_ticks = 1U,
          }) == strata::Status::Ok);

    // AP 0: (WP0 && WP1) -> Quiesce & Reload Slot 1; escalate to spare Slot 2 after 2 reloads
    CHECK(fdir.add_actionpoint(ActionpointDef{
              .ap_id                            = 100U,
              .required_wp_mask                 = (1U << 0U) | (1U << 1U),
              .require_all                      = true,
              .level                            = RecoveryLevel::QuiesceAndReloadSlot,
              .target_partition                 = 4U,
              .target_slot_id                   = 1U,
              .max_slot_reloads_before_failover = 2U,
          }) == strata::Status::Ok);

    // Pre-verified spare-slot failover: Slot 1 -> Spare DFX Slot 2 (personality 0x22)
    CHECK(fdir.register_slot_failover(SlotFailoverEntry{
              .primary_slot_id   = 1U,
              .spare_slot_id     = 2U,
              .personality_id    = 0x0022U,
              .standby_partition = 5U,
          }) == strata::Status::Ok);

    // Tick 1: Only 1 over-temp sample (debounce = 2) -> must NOT trip yet
    fdir.evaluate_sample(10U, 90'000);
    fdir.evaluate_sample(11U, 4'500);
    RecoveryDirective last{};
    CHECK(fdir.evaluate_actionpoints([&](const RecoveryDirective& d) noexcept { last = d; }) == 0U);

    // Tick 2: 2nd consecutive over-temp sample -> trips WP0 && WP1 -> Reload #1
    fdir.evaluate_sample(10U, 91'000);
    CHECK(fdir.evaluate_actionpoints([&](const RecoveryDirective& d) noexcept { last = d; }) == 1U);
    CHECK(last.level == RecoveryLevel::QuiesceAndReloadSlot);
    CHECK(last.primary_slot_id == 1U);

    // Reload #2 still tries surgical slot reload
    CHECK(fdir.evaluate_actionpoints([&](const RecoveryDirective& d) noexcept { last = d; }) == 1U);
    CHECK(last.level == RecoveryLevel::QuiesceAndReloadSlot);

    // Fault #3 exceeds max_slot_reloads_before_failover -> automatic failover to spare Slot 2
    CHECK(fdir.evaluate_actionpoints([&](const RecoveryDirective& d) noexcept { last = d; }) == 1U);
    CHECK(last.level == RecoveryLevel::FailoverSpareSlot);
    CHECK(last.primary_slot_id == 1U);
    CHECK(last.failover_slot_id == 2U);
    CHECK(last.personality_id == 0x0022U);
    CHECK(last.target_partition == 5U);
    return 0;
}

int test_cram_frame_attribution() {
    using namespace strata::services::fdir;

    FdirEngine<> fdir{};
    CHECK(fdir.register_cram_range(CramFrameRange{
              .frame_addr_start = 0x0010'0000U,
              .frame_addr_end   = 0x001F'FFFFU,
              .slot_id          = 1U,
              .owner_partition  = 4U,
          }) == strata::Status::Ok);
    CHECK(fdir.register_slot_failover(SlotFailoverEntry{
              .primary_slot_id   = 1U,
              .spare_slot_id     = 2U,
              .personality_id    = 0x0022U,
              .standby_partition = 5U,
          }) == strata::Status::Ok);

    // Upset inside Slot 1 CRAM range -> surgical single-slot reload (siblings untouched)
    const auto d1 = fdir.handle_uncorrectable_cram_upset(0x0012'3456U, 1U);
    CHECK(d1.level == RecoveryLevel::QuiesceAndReloadSlot);
    CHECK(d1.primary_slot_id == 1U);

    // Repeated hard CRAM failure in Slot 1 -> spare-slot failover to Slot 2
    const auto d2 = fdir.handle_uncorrectable_cram_upset(0x0012'3456U, 1U);
    CHECK(d2.level == RecoveryLevel::FailoverSpareSlot);
    CHECK(d2.failover_slot_id == 2U);

    // Uncorrectable upset in static shell outside mapped dynamic slots -> System Safe Mode
    const auto d_shell = fdir.handle_uncorrectable_cram_upset(0x0000'0040U, 1U);
    CHECK(d_shell.level == RecoveryLevel::SystemSafeMode);
    return 0;
}

} // namespace

int main() {
    if (test_watchpoint_actionpoint_and_failover_escalation() != 0) return 1;
    if (test_cram_frame_attribution() != 0) return 1;
    std::printf("PASS: strata.unit.fdir_and_failover\n");
    return 0;
}
