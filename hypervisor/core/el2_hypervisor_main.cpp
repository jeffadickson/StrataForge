// =============================================================================
// STRATA-20 Freestanding Bare-Metal Versal Cortex-A72 EL2 Hypervisor Entry
// Compiles & links cleanly with `-ffreestanding -nostdlib -fno-exceptions -fno-rtti`
// proving real AArch64 EL2 Stage-2/HVC/trap codegen with zero libc or heap.
// =============================================================================

#include <aarch64/versal_a72_backend.hpp>
#include <strata/hyp/hypervisor.hpp>
#include <strata/ipc/nv_fault_log.hpp>

#include <cstddef>
#include <cstdint>

extern "C" void* memset(void* dst, int ch, size_t n) noexcept {
    auto* p = static_cast<unsigned char*>(dst);
    for (size_t i = 0U; i < n; ++i) p[i] = static_cast<unsigned char>(ch);
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, size_t n) noexcept {
    auto*       d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0U; i < n; ++i) d[i] = s[i];
    return dst;
}

namespace {

struct alignas(64) HypBootCheckpoint {
    uint64_t boot_count;
    uint64_t last_trap_far;
};

__attribute__((section(".critical_store"), used)) HypBootCheckpoint             g_hyp_critical_checkpoint{};
__attribute__((section(".nv_flight_log"), used)) strata::NvFaultLogStorage<16U> g_mram_nv_log_storage{};

strata::NvFaultLog<16U>                                               g_nv_log{g_mram_nv_log_storage};
strata::hyp::arch::VersalA72El2Backend                                g_a72_backend{};
strata::hyp::HypervisorCore<strata::hyp::arch::VersalA72El2Backend>   g_hypervisor{
    g_a72_backend, /*minor_us=*/10'000U, /*major_us=*/1'000'000U, /*fdir_pid=*/1U};

constexpr uint32_t kEsrEcHvc64      = 0x16U;
constexpr uint64_t kHypManifestHash = 0x14A6'F9D1'9243'B827ULL;

} // namespace

extern "C" uint64_t strata_el2_sync_handler(uint64_t esr_el2, uint64_t elr_el2, uint64_t far_el2,
                                            uint64_t* guest_regs) noexcept {
    using namespace strata;
    using namespace strata::hyp;
    const uint32_t ec          = static_cast<uint32_t>((esr_el2 >> 26U) & 0x3FU);
    const uint8_t  caller_pid  = g_a72_backend.current_vmid();

    if (ec == kEsrEcHvc64) {
        const auto call = static_cast<HypCall>(guest_regs[0]);
        const auto res  = g_hypervisor.handle_hypcall(caller_pid, call, guest_regs[1],
                                                      guest_regs[2], guest_regs[3]);
        return res.has_value() ? res.value() : static_cast<uint64_t>(res.error());
    }

    const uint64_t now_ns = g_a72_backend.read_hw_tai_ns();
    // Persist trap dump into refresh-free TMR+SECDED MRAM flight log so it survives cold reboot
    (void)g_nv_log.append(NvRecordKind::CrashTrap, caller_pid, static_cast<uint32_t>(esr_el2),
                          TaiTimestamp{now_ns}, elr_el2, far_el2, guest_regs[8]);

    // Synchronous hardware abort from guest EL1 -> halt offender & wake FDIR partition
    g_hypervisor.on_hardware_trap(TrapContext{
        .partition_id = caller_pid,
        .trap_kind    = 1U, // Data/Instruction abort
        .gen_id       = g_hypervisor.partition(caller_pid).gen.value,
        .pc           = elr_el2,
        .lr           = guest_regs[8],
        .syndrome     = esr_el2,
        .fault_addr   = far_el2,
        .tai_ns       = now_ns,
    });
    return 0ULL;
}

extern "C" void strata_el2_main() noexcept {
    using namespace strata;
    using namespace strata::hyp;

    // Mount reboot-surviving MRAM flight log (preserves prior epochs across power cycles)
    (void)g_nv_log.mount_on_boot(kHypManifestHash, ResetReason::PowerOnCold,
                                 TaiTimestamp{g_a72_backend.read_hw_tai_ns()});

    // Register static mission partitions & Stage-2 isolation windows
    (void)g_hypervisor.register_partition(PartitionConfig{
        .partition_id = 0U, .criticality = Criticality::ClassB, .cpu_id = 0U, .budget_us = 2'000U});
    (void)g_hypervisor.register_partition(PartitionConfig{
        .partition_id = 1U, .criticality = Criticality::ClassB, .cpu_id = 0U,
        .privileged_slot_ctl = true, .privileged_part_ctl = true, .budget_us = 2'000U});
    (void)g_hypervisor.register_partition(PartitionConfig{
        .partition_id = 2U, .criticality = Criticality::ClassD, .cpu_id = 0U, .budget_us = 3'000U});

    g_a72_backend.set_stage2_root(0U, 0x0021'0000ULL);
    g_a72_backend.set_stage2_root(1U, 0x0022'0000ULL);
    g_a72_backend.set_stage2_root(2U, 0x0023'0000ULL);

    (void)g_hypervisor.map_region(
        MemRegionDesc{0U, MemPerm::ReadWrite, kRegionStackOrBss, 0x1000'0000ULL, 0x0020'0000ULL});
    (void)g_hypervisor.map_region(
        MemRegionDesc{1U, MemPerm::ReadWrite, kRegionStackOrBss, 0x1100'0000ULL, 0x0010'0000ULL});
    (void)g_hypervisor.map_region(
        MemRegionDesc{2U, MemPerm::ReadWrite, kRegionStackOrBss, 0x1300'0000ULL, 0x0020'0000ULL});

    // Begin first minor frame & activate first runnable partition context
    g_hypervisor.begin_minor_frame(g_a72_backend.read_hw_tai_ns(), 0U);
    (void)g_hypervisor.select_next_runnable(0U);
}
