#include <strata/core/types.hpp>
#include <strata/hyp/abi.hpp>

#include <cstdio>

int main() {
    constexpr strata::FrameDesc desc{
        .phys_addr     = 0x0000'0008'0000'0000ULL,
        .tai_ns        = 1'700'000'000'000'000'000ULL,
        .length_bytes  = 4U * 1024U * 1024U,
        .pool_slot_idx = 7U,
        .src_slot_id   = 0U,
        .producer_gen  = 1U,
        .flags         = 0x0001U,
        .crc32c        = 0xCAFE'BABEU,
    };

    std::printf(
        "StrataForge SIL ready | ABI v%u.%u | FrameDesc=%zuB | SpscHeader=%zuB | slot=%u len=%u\n",
        static_cast<unsigned>(strata::hyp::kAbiMajor),
        static_cast<unsigned>(strata::hyp::kAbiMinor),
        sizeof(strata::FrameDesc),
        sizeof(strata::SpscHeader),
        static_cast<unsigned>(desc.pool_slot_idx),
        static_cast<unsigned>(desc.length_bytes));
    return 0;
}
