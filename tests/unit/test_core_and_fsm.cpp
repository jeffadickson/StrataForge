#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/fsm.hpp>
#include <strata/core/qos.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstdio>
#include <cstring>

namespace {

using namespace strata::literals;

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                       \
        }                                                                                   \
    } while (false)

// Standard CRC-32C check vector: CRC-32C("123456789") == 0xE3069283
constexpr uint32_t kExpectedCrc32cCheck = 0xE306'9283U;

constexpr uint32_t compute_check_vector() noexcept {
    constexpr std::array<std::byte, 9U> msg{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
        std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'},
    };
    return strata::Crc32c::compute(msg);
}

static_assert(compute_check_vector() == kExpectedCrc32cCheck,
              "Compile-time CRC-32C check vector mismatch!");

enum class InstState : uint8_t { Standby, Armed, Active, Faulted };
enum class InstEvent : uint8_t { Arm, Start, Stop, Fault };

struct InstCtx {
    bool     sbit_passed{false};
    uint32_t starts{0U};
    uint32_t stops{0U};
};

bool guard_sbit(const InstCtx& c) noexcept { return c.sbit_passed; }
void act_start(InstCtx& c) noexcept { ++c.starts; }
void act_stop(InstCtx& c) noexcept { ++c.stops; }

int test_expected_and_qos() {
    strata::Expected<int, strata::Status> ok_val(42);
    CHECK(ok_val.has_value());
    CHECK(*ok_val == 42);
    CHECK(ok_val.value_or(0) == 42);

    strata::Expected<int, strata::Status> err_val{strata::Unexpected(strata::Status::Timeout)};
    CHECK(!err_val.has_value());
    CHECK(err_val.error() == strata::Status::Timeout);
    CHECK(err_val.value_or(99) == 99);

    strata::Expected<void, strata::Status> void_ok{};
    CHECK(void_ok.has_value());
    strata::Expected<void, strata::Status> void_err{strata::Unexpected(strata::Status::Busy)};
    CHECK(!void_err.has_value());
    CHECK(void_err.error() == strata::Status::Busy);

    constexpr auto t0 = strata::TaiTimestamp{100U} + 5_ms;
    CHECK(t0.ns() == 5'000'100ULL);
    CHECK(strata::elapsed_since(t0, strata::TaiTimestamp{100U}) == 5_ms);
    return 0;
}

int test_fsm() {
    using T = strata::Transition<InstCtx, InstState, InstEvent>;
    constexpr std::array<T, 4U> kTable{{
        {InstState::Standby, InstEvent::Arm, InstState::Armed, &guard_sbit, nullptr},
        {InstState::Armed, InstEvent::Start, InstState::Active, nullptr, &act_start},
        {InstState::Active, InstEvent::Stop, InstState::Standby, nullptr, &act_stop},
        {InstState::Active, InstEvent::Fault, InstState::Faulted, nullptr, nullptr},
    }};

    strata::Fsm<InstCtx, InstState, InstEvent, 4U> fsm(InstState::Standby, kTable);
    InstCtx ctx{};

    // Guard blocks Arm until SBIT passes
    CHECK(fsm.dispatch(ctx, InstEvent::Arm) == strata::Status::GuardRejected);
    CHECK(fsm.state() == InstState::Standby);

    // Unhandled event rejected cleanly
    CHECK(fsm.dispatch(ctx, InstEvent::Stop) == strata::Status::UnhandledEvent);

    ctx.sbit_passed = true;
    CHECK(fsm.dispatch(ctx, InstEvent::Arm) == strata::Status::Ok);
    CHECK(fsm.state() == InstState::Armed);
    CHECK(fsm.dispatch(ctx, InstEvent::Start) == strata::Status::Ok);
    CHECK(fsm.state() == InstState::Active);
    CHECK(ctx.starts == 1U);
    CHECK(fsm.dispatch(ctx, InstEvent::Stop) == strata::Status::Ok);
    CHECK(fsm.state() == InstState::Standby);
    CHECK(ctx.stops == 1U);
    CHECK(fsm.transition_count() == 3U);
    return 0;
}

} // namespace

int main() {
    if (test_expected_and_qos() != 0) return 1;
    if (test_fsm() != 0) return 1;
    std::printf("PASS: strata.unit.core_and_fsm\n");
    return 0;
}
