#include <strata/core/types.hpp>
#include <strata/hyp/abi.hpp>

#include <cstddef>
#include <cstdio>

namespace {

constexpr bool verify_wire_layouts() noexcept {
    static_assert(sizeof(strata::hyp::SharedTimePage) == 64U);
    static_assert(sizeof(strata::FrameDesc) == 32U);
    static_assert(alignof(strata::FrameDesc) == 32U);
    static_assert(sizeof(strata::SpscHeader) == 128U);
    static_assert(offsetof(strata::SpscHeader, tail) == 64U,
                  "Producer and consumer indices must live on separate 64B cache lines");
    static_assert(static_cast<uint32_t>(strata::hyp::HypCall::DiagTrace) == 7U,
                  "Hypervisor ABI must remain bounded to 8 auditable hypercalls");
    return true;
}

static_assert(verify_wire_layouts());

} // namespace

int main() {
    const strata::TaiTimestamp t0{1000U};
    const strata::TaiTimestamp t1{2500U};
    if (!(t0 < t1)) {
        std::fprintf(stderr, "FAIL: TaiTimestamp ordering\n");
        return 1;
    }

    const strata::GenId g0{1U};
    const strata::GenId g1{2U};
    if (!(g0 < g1)) {
        std::fprintf(stderr, "FAIL: GenId ordering\n");
        return 1;
    }

    std::printf("PASS: strata.unit.abi_and_types\n");
    return 0;
}
