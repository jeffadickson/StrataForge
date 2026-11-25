#include <strata/middleware.hpp>
#include <strata/services/comms/ccsds.hpp>
#include <strata/services/fdir/fdir_engine.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using namespace strata::literals;

/// Deterministic splitmix64 PRNG for 100% reproducible STest-parity rule-based fuzzing.
class SeededRng {
public:
    explicit constexpr SeededRng(uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr uint64_t next() noexcept {
        uint64_t z = (state_ += 0x9E37'79B9'7F4A'7C15ULL);
        z          = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        z          = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] constexpr uint32_t range(uint32_t bound) noexcept {
        return static_cast<uint32_t>(next() % bound);
    }

private:
    uint64_t state_;
};

/// Rule-based stateful fuzz test (JPL F' STest parity):
/// Exercises 25,000 seeded random sequences of:
///  - Zero-copy DataTopic 1-to-N publishes across Guaranteed + BestEffort subscribers
///  - Random Class D consumer stalls, crashes, lease-timeout reclamations, and restarts
///  - Malformed random CCSDS wire packet fuzzing (zero crashes/UB invariant)
///  - Random SEU bit-flips in CriticalStore ping-pong slots (never returns corrupt data invariant)
int run_rule_fuzzer(uint64_t seed) {
    SeededRng rng(seed);

    using Pool  = strata::FramePool<8U, 128U>;
    using Topic = strata::DataTopic<Pool, 4U, 8U>;

    std::array<std::byte, 8U * 128U> ddr{};
    Pool  pool(ddr, 0x8000'0000ULL);
    Topic topic(pool);

    auto sub_b = topic.add_subscriber(strata::kGuaranteedControlQoS);
    auto sub_d = topic.add_subscriber(strata::kBestEffortScienceQoS);
    if (!sub_b || !sub_d) return 1;

    struct State {
        uint32_t epoch{0U};
        uint32_t magic{0x1234'5678U};
    };
    strata::CriticalStoreStorage<State> cds_mem{};
    strata::CriticalStore<State>        cds(cds_mem, 0xCAFE'BABE'DEAD'BEEFULL);
    uint32_t                            epoch_counter = 0U;

    uint64_t tai_ns  = 1'000'000ULL;
    uint16_t sub_gen = 1U;

    for (uint32_t step = 0U; step < 25'000U; ++step) {
        tai_ns += (rng.range(15U) + 1U) * (1_ms).ns();
        const strata::TaiTimestamp now{tai_ns};
        const uint32_t             rule = rng.range(6U);

        switch (rule) {
            case 0U: { // Rule 0: Allocate & publish zero-copy frame
                auto s = pool.allocate_slot(strata::GenId{1U});
                if (s) {
                    strata::FrameDesc d{
                        .phys_addr     = pool.slot_phys_addr(*s),
                        .tai_ns        = now.ns(),
                        .length_bytes  = 64U,
                        .pool_slot_idx = *s,
                        .src_slot_id   = 0U,
                        .producer_gen  = 1U,
                    };
                    strata::FrameLease<Pool> lease(pool, d);
                    (void)topic.publish(std::move(lease), now);
                }
                break;
            }
            case 1U: { // Rule 1: Class B Guaranteed subscriber drains
                if (auto v = topic.try_receive(*sub_b); v) {
                    if (v->bytes().size() != 64U) {
                        std::fprintf(stderr, "FAIL: invariant broken on Class B view size\n");
                        return 1;
                    }
                    v->release();
                }
                break;
            }
            case 2U: { // Rule 2: Class D BestEffort subscriber randomly drains (or stalls!)
                if (rng.range(3U) == 0U) {
                    if (auto v = topic.try_receive(*sub_d); v) {
                        v->release();
                    }
                }
                break;
            }
            case 3U: { // Rule 3: FDIR reclaims expired leases or restarts crashed Class D
                topic.reclaim_expired(now);
                if (rng.range(10U) == 0U) {
                    ++sub_gen;
                    topic.reset_crashed_subscriber(*sub_d, strata::GenId{sub_gen});
                }
                break;
            }
            case 4U: { // Rule 4: Fuzz CCSDS Space Packet & TC parsers with arbitrary random bytes
                std::array<std::byte, 32U> garbage{};
                const size_t               len = rng.range(33U);
                for (size_t i = 0U; i < len; ++i) {
                    garbage[i] = static_cast<std::byte>(rng.next() & 0xFFU);
                }
                auto sp = strata::services::comms::parse_space_packet(
                    std::span<const std::byte>(garbage.data(), len));
                auto tc = strata::services::comms::parse_tc_frame(
                    std::span<const std::byte>(garbage.data(), len));
                (void)sp;
                (void)tc;
                break;
            }
            case 5U: { // Rule 5: Checkpoint + random single-slot SEU corruption invariant
                ++epoch_counter;
                cds.checkpoint(State{.epoch = epoch_counter, .magic = 0x1234'5678U});
                if (rng.range(4U) == 0U) {
                    const size_t victim = rng.range(2U);
                    cds_mem.slots[victim].payload.epoch ^= 0x0000'0001U;
                }
                if (auto r = cds.restore(); r) {
                    if (r->magic != 0x1234'5678U || r->epoch > epoch_counter) {
                        std::fprintf(stderr, "FAIL: CriticalStore returned corrupted state!\n");
                        return 1;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // Final invariant: drain Class B + revoke Class D -> 100% of DDR4 pool slots MUST be free!
    while (auto v = topic.try_receive(*sub_b)) {
        v->release();
    }
    topic.reset_crashed_subscriber(*sub_d, strata::GenId{static_cast<uint16_t>(sub_gen + 1U)});
    if (pool.free_count() != Pool::kSlotCount) {
        std::fprintf(stderr, "FAIL: DDR pool leaked slots! free=%zu/%zu\n", pool.free_count(),
                     Pool::kSlotCount);
        return 1;
    }

    std::printf("PASS: strata.rule_fuzzer (seed=0x%016llX, 25000 steps, 0 leaks, 0 crashes)\n",
                static_cast<unsigned long long>(seed));
    return 0;
}

} // namespace

int main() {
    return run_rule_fuzzer(0x5354'5241'5441'2026ULL);
}
