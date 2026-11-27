// =============================================================================
// STRATA-20 Live QEMU AArch64 Cortex-A72 EL2 Hypervisor Execution Self-Test
// Runs bare-metal under `qemu-system-aarch64 -machine virt,virtualization=on`:
//   1. Verifies CurrentEL == 2 and live CNTPCT_EL0 monotonic clock ticks.
//   2. Configures real ARMv8-A Stage-2 MMU (L1 1GB blocks via VTCR_EL2/VTTBR_EL2/HCR_EL2).
//   3. ERETs into an unprivileged EL1 Guest Partition:
//      - Executes real `HVC #0` hypercalls (TimeRead + DiagTrace) -> HypervisorCore.
//      - Writes to unmapped IPA `0x8000_0000` -> triggers real Stage-2 Data Abort (EC=0x24)!
//   4. EL2 catches Stage-2 abort, halts offending partition, wakes FDIR, persists
//      SECDED+TMR `NvFaultLog` CrashTrap, simulates warm reboot (`boot_epoch` 1 -> 2),
//      injects & corrects a 1-bit MRAM SEU, and verifies freestanding FIPS 180-4 SHA-256.
// =============================================================================

#include <aarch64/versal_a72_backend.hpp>
#include <strata/crypto/sha256.hpp>
#include <strata/hyp/hypervisor.hpp>
#include <strata/ipc/nv_fault_log.hpp>

#include <array>
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

extern "C" uint64_t qemu_run_el1_guest(uint64_t entry_pc, uint64_t guest_sp) noexcept;
extern "C" char     __el1_guest_stack_top[];

namespace {

using namespace strata;
using namespace strata::hyp;

// QEMU `virt` PL011 UART0 base address
constexpr uintptr_t kPl011UartBase = 0x0900'0000ULL;

void uart_putc(char c) noexcept {
    *reinterpret_cast<volatile uint32_t*>(kPl011UartBase) = static_cast<uint32_t>(static_cast<uint8_t>(c));
}

void uart_puts(const char* s) noexcept {
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

void uart_put_hex64(uint64_t v) noexcept {
    static constexpr char kHex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(kHex[(v >> static_cast<unsigned>(shift)) & 0xFU]);
    }
}

// 4 KB-aligned ARMv8-A Stage-2 Level-1 translation table (512 x 1 GB blocks)
alignas(4096) uint64_t g_s2_l1_table[512]{};

__attribute__((section(".nv_flight_log"), used)) NvFaultLogStorage<16U> g_mram_storage{};
NvFaultLog<16U>                                                         g_nv_log{g_mram_storage};
arch::VersalA72El2Backend                                               g_a72_backend{};
HypervisorCore<arch::VersalA72El2Backend>                               g_hypervisor{
    g_a72_backend, /*minor_us=*/10'000U, /*major_us=*/1'000'000U, /*fdir_pid=*/1U};

constexpr uint32_t kEsrEcHvc64      = 0x16U;
constexpr uint32_t kEsrEcDataAbortL = 0x24U;
constexpr uint64_t kQemuManifest    = 0xEE35'F9CF'C303'6975ULL;
constexpr uint64_t kIllegalIpaAddr  = 0x8000'0000ULL;

volatile uint64_t g_guest_observed_tai = 0ULL;
uint64_t          g_guest_hvc_count    = 0ULL;
uint64_t          g_last_fault_far     = 0ULL;
uint64_t          g_last_fault_esr     = 0ULL;

uint64_t issue_hvc(uint64_t call_id, uint64_t a0 = 0ULL, uint64_t a1 = 0ULL, uint64_t a2 = 0ULL) noexcept {
    register uint64_t x0 asm("x0") = call_id;
    register uint64_t x1 asm("x1") = a0;
    register uint64_t x2 asm("x2") = a1;
    register uint64_t x3 asm("x3") = a2;
    asm volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

/// Code executing at EL1 (unprivileged guest partition 2, Stage-2 MMU active!)
extern "C" void el1_guest_partition_entry() noexcept {
    // 1. Real HVC #0 -> TimeRead
    const uint64_t tai   = issue_hvc(static_cast<uint64_t>(HypCall::TimeRead));
    g_guest_observed_tai = tai;

    // 2. Real HVC #0 -> DiagTrace
    (void)issue_hvc(static_cast<uint64_t>(HypCall::DiagTrace), 0x57A7'A202ULL);

    // 3. Intentionally violate Stage-2 MMU isolation by writing to unmapped IPA 0x8000_0000!
    // Hardware immediately traps into EL2 with ESR_EL2.EC = 0x24 (Data Abort from Lower EL).
    *reinterpret_cast<volatile uint64_t*>(kIllegalIpaAddr) = 0xBAD0'DEAD'CAFE'BABEULL;

    for (;;) {
        asm volatile("wfe");
    }
}

} // namespace

extern "C" uint64_t strata_qemu_sync_handler(uint64_t esr_el2, uint64_t elr_el2, uint64_t far_el2,
                                             uint64_t /*hpfar_el2*/, uint64_t* guest_regs) noexcept {
    const uint32_t ec         = static_cast<uint32_t>((esr_el2 >> 26U) & 0x3FU);
    const uint8_t  caller_pid = g_a72_backend.current_vmid();

    if (ec == kEsrEcHvc64) {
        ++g_guest_hvc_count;
        const auto call = static_cast<HypCall>(guest_regs[0]);
        const auto res  = g_hypervisor.handle_hypcall(caller_pid, call, guest_regs[1],
                                                      guest_regs[2], guest_regs[3]);
        guest_regs[0]   = res.has_value() ? res.value() : static_cast<uint64_t>(res.error());
        return 0ULL; // Resume EL1 guest via ERET
    }

    // Fatal hardware Stage-2 abort from guest EL1!
    g_last_fault_far = far_el2;
    g_last_fault_esr = esr_el2;

    const uint64_t now_ns = g_a72_backend.read_hw_tai_ns();
    (void)g_nv_log.append(NvRecordKind::CrashTrap, caller_pid, static_cast<uint32_t>(esr_el2),
                          TaiTimestamp{now_ns}, elr_el2, far_el2, guest_regs[8]);

    g_hypervisor.on_hardware_trap(TrapContext{
        .partition_id = caller_pid,
        .trap_kind    = static_cast<uint8_t>(ec),
        .gen_id       = g_hypervisor.partition(caller_pid).gen.value,
        .pc           = elr_el2,
        .lr           = guest_regs[8],
        .syndrome     = esr_el2,
        .fault_addr   = far_el2,
        .tai_ns       = now_ns,
    });

    // Non-zero return unwinds back to EL2 test harness with ESR_EL2
    return esr_el2;
}

extern "C" int strata_qemu_el2_main() noexcept {
    uart_puts("\n[STRATA-EL2] Booting bare-metal AArch64 Hypervisor on QEMU Cortex-A72\n");

    // 1. Verify CurrentEL == 2
    uint64_t current_el = 0ULL;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    const uint64_t el = (current_el >> 2U) & 0x3ULL;
    uart_puts("[STRATA-EL2] CurrentEL=");
    uart_putc(static_cast<char>('0' + el));
    uart_puts("\n");
    if (el != 2ULL) {
        uart_puts("FAIL: Not executing in EL2!\n");
        return 1;
    }

    // 2. Verify live CNTPCT_EL0 hardware monotonic counter advances
    const uint64_t t0 = g_a72_backend.read_hw_tai_ns();
    uint64_t       t1 = t0;
    for (int spin = 0; spin < 2'000'000 && t1 <= t0; ++spin) {
        asm volatile("nop");
        t1 = g_a72_backend.read_hw_tai_ns();
    }
    if (t1 <= t0) {
        uart_puts("FAIL: CNTPCT_EL0 counter did not advance!\n");
        return 2;
    }

    // 3. Mount reboot-surviving MRAM NvFaultLog (boot_epoch = 1)
    if (g_nv_log.mount_on_boot(kQemuManifest, ResetReason::PowerOnCold, TaiTimestamp{t1}) != Status::Ok) {
        uart_puts("FAIL: NvFaultLog cold mount failed!\n");
        return 3;
    }

    // 4. Register static partitions & configure real Stage-2 MMU 1 GB L1 block table
    (void)g_hypervisor.register_partition(PartitionConfig{
        .partition_id = 0U, .criticality = Criticality::ClassB, .cpu_id = 0U, .budget_us = 2'000U});
    (void)g_hypervisor.register_partition(PartitionConfig{
        .partition_id = 1U, .criticality = Criticality::ClassB, .cpu_id = 0U,
        .privileged_slot_ctl = true, .privileged_part_ctl = true, .budget_us = 2'000U});
    (void)g_hypervisor.register_partition(PartitionConfig{
        .partition_id = 2U, .criticality = Criticality::ClassD, .cpu_id = 0U, .budget_us = 3'000U});

    // Entry 1 maps IPA 0x4000_0000..0x7FFF_FFFF -> PA 0x4000_0000 (RWX Normal WB Cacheable, AF=1)
    // Entry 0 (0x0000_0000..0x3FFF_FFFF, UART/MMIO) & Entry 2 (0x8000_0000..) remain UNMAPPED!
    constexpr uint64_t kS2BlockRwx =
        0x4000'0000ULL | (0xFULL << 2U) | (0x3ULL << 6U) | (0x3ULL << 8U) | (1ULL << 10U) | 0x1ULL;
    g_s2_l1_table[1] = kS2BlockRwx;

    const uint64_t s2_root_pa = reinterpret_cast<uint64_t>(&g_s2_l1_table[0]);
    g_a72_backend.set_gicd_sgir_addr(0x0800'0F00ULL); // QEMU virt GICv2 Distributor SGIR
    g_a72_backend.set_stage2_root(2U, s2_root_pa);
    (void)g_a72_backend.activate_partition_context(2U);

    // Program VTCR_EL2 (4KB granule, SL0=1 L1 start, T0SZ=25 -> 39-bit IPA single 4KB L1 table, PS=2 40-bit PA)
    constexpr uint64_t kVtcrEl2 =
        (1ULL << 31U) | (2ULL << 16U) | (0ULL << 14U) | (3ULL << 12U) |
        (1ULL << 10U) | (1ULL << 8U) | (1ULL << 6U) | 25ULL;
    asm volatile("msr vtcr_el2, %0" : : "r"(kVtcrEl2));
    // Enable FP/SIMD for EL1 & disable EL1 Stage-1 MMU cleanly
    asm volatile("msr cpacr_el1, %0" : : "r"(3ULL << 20U));
    asm volatile("msr sctlr_el1, %0" : : "r"(0x00C5'0838ULL));
    // Enable AArch64 EL1 (RW=1) + Stage-2 MMU (VM=1)
    constexpr uint64_t kHcrEl2 = (1ULL << 31U) | 1ULL;
    asm volatile("msr hcr_el2, %0" : : "r"(kHcrEl2));
    asm volatile("tlbi vmalls12e1\n dsb sy\n isb");

    // 5. ERET into real EL1 Guest Partition 2 -> runs HVC #0 calls, then faults on unmapped 0x8000_0000!
    uart_puts("[STRATA-EL2] Entering EL1 Guest Partition 2 with Stage-2 MMU active...\n");
    const uint64_t trapped_esr = qemu_run_el1_guest(
        reinterpret_cast<uint64_t>(&el1_guest_partition_entry),
        reinterpret_cast<uint64_t>(&__el1_guest_stack_top[0]));

    const uint32_t ec = static_cast<uint32_t>((trapped_esr >> 26U) & 0x3FU);
    uart_puts("[STRATA-EL2] Trapped EL1 fault ESR_EL2=");
    uart_put_hex64(trapped_esr);
    uart_puts(" FAR_EL2=");
    uart_put_hex64(g_last_fault_far);
    uart_puts("\n");

    if (g_guest_hvc_count != 2ULL || g_guest_observed_tai == 0ULL) {
        uart_puts("FAIL: EL1 HVC #0 hypercalls did not complete!\n");
        return 4;
    }
    if (ec != kEsrEcDataAbortL || g_last_fault_far != kIllegalIpaAddr) {
        uart_puts("FAIL: Unexpected Stage-2 fault syndrome or FAR_EL2!\n");
        return 5;
    }
    if (!g_hypervisor.partition(2U).halted) {
        uart_puts("FAIL: Hypervisor did not halt offending partition 2!\n");
        return 6;
    }

    // 6. Simulate warm reboot & verify NvFaultLog survives + Hamming(72,64) SECDED corrects 1-bit SEU
    if (g_nv_log.mount_on_boot(kQemuManifest, ResetReason::WatchdogReset,
                               TaiTimestamp{g_a72_backend.read_hw_tai_ns()}) != Status::Ok) {
        uart_puts("FAIL: NvFaultLog warm remount failed!\n");
        return 7;
    }
    const auto snap = g_nv_log.header_snapshot();
    if (snap.boot_epoch != 2U || g_nv_log.valid_entry_count() < 2U) {
        uart_puts("FAIL: NvFaultLog did not persist across warm reboot!\n");
        return 8;
    }
    // Flip 1 bit in stored CrashTrap record slot 1 -> verify SECDED single-bit correction!
    g_mram_storage.slots[1].words[2].data ^= (1ULL << 19U);
    const auto rec = g_nv_log.read_slot(1U);
    if (!rec.has_value() || rec.value().kind != NvRecordKind::CrashTrap ||
        rec.value().aux1 != kIllegalIpaAddr || g_nv_log.header_snapshot().ecc_ce_count < 1U) {
        uart_puts("FAIL: NvFaultLog SECDED recovery failed!\n");
        return 9;
    }

    // 7. Verify freestanding bare-metal FIPS 180-4 SHA-256 on Cortex-A72 silicon
    const char abc[] = {'a', 'b', 'c'};
    const auto digest = crypto::Sha256::hash(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(abc), 3U});
    if (std::to_integer<uint8_t>(digest[0]) != 0xBAU ||
        std::to_integer<uint8_t>(digest[1]) != 0x78U ||
        std::to_integer<uint8_t>(digest[31]) != 0xADU) {
        uart_puts("FAIL: Bare-metal SHA-256 self-test mismatch!\n");
        return 10;
    }

    uart_puts(
        "PASS: QEMU AArch64 EL2 Execution verified (CurrentEL=2, CNTPCT_EL0, Stage-2 MMU, "
        "EL1 HVC#0 x2, Stage-2 Data Abort EC=0x24 -> FDIR halt, NvFaultLog warm-reboot + SECDED, SHA-256)\n");
    return 0;
}
