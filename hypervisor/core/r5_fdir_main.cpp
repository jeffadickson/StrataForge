// =============================================================================
// STRATA-20 Freestanding Bare-Metal Versal Lockstep Cortex-R5F FDIR Supervisor
// Compiles cleanly with `arm-none-eabi-g++ -mcpu=cortex-r5 -ffreestanding
// -fno-exceptions -fno-rtti` proving zero-heap ARMv7-R bare-metal readiness.
// =============================================================================

#include <arm_r5/versal_r5_backend.hpp>
#include <strata/hyp/hypervisor.hpp>
#include <strata/services/fdir/fdir_engine.hpp>
#include <strata/services/fdir/health_and_fault_router.hpp>

#include <cstdint>

namespace {

strata::hyp::arch::VersalR5LockstepBackend                             g_r5_backend{};
strata::hyp::HypervisorCore<strata::hyp::arch::VersalR5LockstepBackend> g_r5_hyp{
    g_r5_backend, /*minor_us=*/10'000U, /*major_us=*/1'000'000U, /*fdir_pid=*/0U};
strata::services::fdir::FdirEngine<16U, 8U, 8U>                        g_fdir{};
strata::services::fdir::FaultRouter<16U, 8U>                           g_router{2U, 1U};

} // namespace

extern "C" uint32_t strata_r5_supervise_step(uint32_t cram_frame_addr) noexcept {
    using namespace strata;
    using namespace strata::hyp;

    (void)g_r5_hyp.map_region(
        MemRegionDesc{0U, MemPerm::DeviceRw, 0U, 0xA000'0000ULL, 0x0001'0000ULL});
    (void)g_fdir.register_cram_range(services::fdir::CramFrameRange{
        .frame_addr_start = 0x0001'0000U,
        .frame_addr_end   = 0x0001'FFFFU,
        .slot_id          = 1U,
        .owner_partition  = 2U,
        .enabled          = true,
    });

    const auto dir = g_fdir.handle_uncorrectable_cram_upset(cram_frame_addr);
    if (dir.level == services::fdir::RecoveryLevel::QuiesceAndReloadSlot) {
        (void)g_r5_backend.execute_slot_op(dir.primary_slot_id, SlotOpCode::Quiesce, 0U);
        return 1U;
    }
    return static_cast<uint32_t>(g_router.safe_mode_escalations());
}
