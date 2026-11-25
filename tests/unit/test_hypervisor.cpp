#include <strata/hyp/hypervisor.hpp>
#include <aarch64/versal_a72_backend.hpp>
#include <arm_r5/versal_r5_backend.hpp>
#include <host_sil/host_sil_backend.hpp>
#include <microblaze/kintex_mb_backend.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    using namespace strata;
    using namespace strata::hyp;

    // 1. Host SIL Backend end-to-end Hypervisor Core verification (STRATA-HYP-002..009)
    arch::HostSilBackend<> backend{};
    HypervisorCore<arch::HostSilBackend<>> hyp(backend, /*minor_us=*/10'000U, /*major_us=*/100'000U,
                                               /*fdir_pid=*/1U);

    CHECK(hyp.register_partition(PartitionConfig{
              .partition_id = 0U, .criticality = Criticality::ClassB, .cpu_id = 0U,
              .privileged_slot_ctl = false, .privileged_part_ctl = false, .budget_us = 3'000U}) ==
          Status::Ok);
    CHECK(hyp.register_partition(PartitionConfig{
              .partition_id = 1U, .criticality = Criticality::ClassB, .cpu_id = 0U,
              .privileged_slot_ctl = true, .privileged_part_ctl = true, .budget_us = 2'000U}) ==
          Status::Ok);
    CHECK(hyp.register_partition(PartitionConfig{
              .partition_id = 2U, .criticality = Criticality::ClassD, .cpu_id = 0U,
              .privileged_slot_ctl = false, .privileged_part_ctl = false, .budget_us = 4'000U}) ==
          Status::Ok);

    // Over-budget partition on CPU 0 must be statically rejected (3+2+4+2 = 11ms > 10ms)
    CHECK(hyp.register_partition(PartitionConfig{
              .partition_id = 3U, .criticality = Criticality::ClassD, .cpu_id = 0U,
              .budget_us = 2'000U}) == Status::ValidationFailed);

    // 2. Hardware spatial memory isolation (STRATA-HYP-003)
    CHECK(hyp.map_region(MemRegionDesc{0U, MemPerm::ReadWrite, kRegionStackOrBss, 0x1000'0000ULL, 0x10000ULL}) ==
          Status::Ok);
    CHECK(hyp.map_region(MemRegionDesc{2U, MemPerm::ReadWrite, kRegionStackOrBss, 0x1001'0000ULL, 0x10000ULL}) ==
          Status::Ok);
    // Reject overlapping RW region between distinct partitions
    CHECK(hyp.map_region(MemRegionDesc{2U, MemPerm::ReadWrite, 0U, 0x1000'8000ULL, 0x4000ULL}) ==
          Status::PermissionDenied);
    // Allow explicit Read-Only shared consumer mapping onto partition 0's region
    CHECK(hyp.map_region(MemRegionDesc{2U, MemPerm::ReadOnly, kRegionSharedConsumerRo, 0x1000'0000ULL, 0x4000ULL}) ==
          Status::Ok);
    CHECK(hyp.memory_table().check_access(2U, 0x1000'0000ULL, 64U, /*is_write=*/false));
    CHECK(!hyp.memory_table().check_access(2U, 0x1000'0000ULL, 64U, /*is_write=*/true));

    // 3. Doorbell ACL + token-bucket rate limiter (STRATA-HYP-004)
    CHECK(hyp.register_doorbell(DoorbellChannelConfig{
              .channel_id = 7U, .src_partition = 2U, .dst_partition = 0U,
              .max_tokens = 2U, .refill_per_minor_frame = 1U}) == Status::Ok);

    // Unauthorized sender rejected
    const auto unauth = hyp.handle_hypcall(0U, HypCall::Notify, 7U);
    CHECK(!unauth.has_value() && unauth.error() == Status::PermissionDenied);

    // Authorized sender gets 2 tokens, 3rd throttled until next minor frame
    CHECK(hyp.handle_hypcall(2U, HypCall::Notify, 7U).has_value());
    CHECK(hyp.handle_hypcall(2U, HypCall::Notify, 7U).has_value());
    const auto throttled = hyp.handle_hypcall(2U, HypCall::Notify, 7U);
    CHECK(!throttled.has_value() && throttled.error() == Status::Busy);
    CHECK(hyp.doorbells().throttled_count(7U) == 1U);

    // Minor frame tick refills 1 token and updates SharedTimePage
    backend.set_sim_tai_ns(50'000'000ULL);
    hyp.begin_minor_frame(50'000'000ULL, 1U);
    CHECK(hyp.shared_time_page().tai_ns == 50'000'000ULL);
    CHECK((hyp.shared_time_page().seq & 1U) == 0U);
    CHECK(hyp.handle_hypcall(2U, HypCall::Notify, 7U).has_value());

    // 4. Deterministic CPU time slicing & Yield (STRATA-HYP-002)
    CHECK(hyp.select_next_runnable(0U).value() == 0U);
    CHECK(hyp.charge_cpu_slice(0U, 2'500U) == Status::Ok);
    CHECK(hyp.charge_cpu_slice(0U, 1'000U) == Status::DeadlineMissed); // Exceeds 3ms budget -> preempted
    CHECK(hyp.partition(0U).overrun_count == 1U);
    CHECK(hyp.select_next_runnable(0U).value() == 1U);
    CHECK(hyp.handle_hypcall(1U, HypCall::Yield).has_value());
    CHECK(hyp.select_next_runnable(0U).value() == 2U);

    // 5. Privilege gating on SlotOp & PartCtl + Warm/Cold restart (STRATA-HYP-006)
    CHECK(hyp.handle_hypcall(2U, HypCall::SlotOp, 1U, static_cast<uint64_t>(SlotOpCode::Quiesce)).error() ==
          Status::PermissionDenied);
    CHECK(hyp.handle_hypcall(1U, HypCall::SlotOp, 1U, static_cast<uint64_t>(SlotOpCode::LoadPartialPdi))
              .has_value());
    CHECK(backend.slot_op_count() == 1U);

    // Populate partition 2 RAM (.bss + CriticalStore), then trigger WarmRestart -> CriticalStore preserved!
    backend.partition_ram(2U).bss_and_stack[0]  = std::byte{0xAA};
    backend.partition_ram(2U).critical_store[0] = std::byte{0x55};
    const auto gen_after_warm =
        hyp.handle_hypcall(1U, HypCall::PartCtl, 2U, static_cast<uint64_t>(PartCtlOp::WarmRestart));
    CHECK(gen_after_warm.has_value() && gen_after_warm.value() == 2U);
    CHECK(hyp.partition(2U).mode == PartitionMode::WarmStart);
    CHECK(backend.partition_ram(2U).bss_and_stack[0] == std::byte{0x00});
    CHECK(backend.partition_ram(2U).critical_store[0] == std::byte{0x55});

    // ColdRestart scrubs both .bss and CriticalStore
    backend.partition_ram(2U).bss_and_stack[0] = std::byte{0xBB};
    const auto gen_after_cold =
        hyp.handle_hypcall(1U, HypCall::PartCtl, 2U, static_cast<uint64_t>(PartCtlOp::ColdRestart));
    CHECK(gen_after_cold.has_value() && gen_after_cold.value() == 3U);
    CHECK(hyp.partition(2U).mode == PartitionMode::ColdStart);
    CHECK(backend.partition_ram(2U).bss_and_stack[0] == std::byte{0x00});
    CHECK(backend.partition_ram(2U).critical_store[0] == std::byte{0x00});

    // 6. Trap capture & out-of-kernel FDIR supervisor wake (STRATA-HYP-008)
    CHECK(hyp.handle_hypcall(1U, HypCall::WaitEvent).has_value());
    CHECK(hyp.partition(1U).waiting_event);
    hyp.on_hardware_trap(TrapContext{
        .partition_id = 2U, .trap_kind = 1U, .gen_id = 3U, .pc = 0x4000'1234ULL, .lr = 0x4000'1200ULL,
        .syndrome = 0x9200'0047ULL, .fault_addr = 0xDEAD'BEEFULL, .tai_ns = 55'000'000ULL});
    CHECK(hyp.partition(2U).halted);
    CHECK(!hyp.partition(1U).waiting_event); // FDIR woken!
    const auto popped = hyp.pop_trap();
    CHECK(popped.has_value() && popped.value().fault_addr == 0xDEAD'BEEFULL);

    // DiagTrace hypcall
    CHECK(hyp.handle_hypcall(0U, HypCall::DiagTrace, 0xCAFE'BABEULL).has_value());
    CHECK(backend.recorder_writes() == 1U && backend.recorder_entry(0U).word == 0xCAFE'BABEULL);

    // 7. Verify all target architecture backends compile and satisfy HypervisorCore (STRATA-HYP-009)
    {
        arch::VersalA72El2Backend a72{};
        HypervisorCore<arch::VersalA72El2Backend> h_a72(a72, 10'000U, 100'000U);
        CHECK(h_a72.map_region(MemRegionDesc{0U, MemPerm::ReadWrite, 0U, 0x2000'0000ULL, 0x10000ULL}) == Status::Ok);
        CHECK(a72.stage2_entry_count() == 1U);
    }
    {
        arch::VersalR5LockstepBackend r5{};
        HypervisorCore<arch::VersalR5LockstepBackend> h_r5(r5, 10'000U, 100'000U);
        CHECK(h_r5.map_region(MemRegionDesc{0U, MemPerm::DeviceRw, 0U, 0xA000'0000ULL, 0x1000ULL}) == Status::Ok);
        CHECK(r5.mpu_region_count() == 1U);
    }
    {
        arch::KintexMicroBlazeBackend mb{};
        HypervisorCore<arch::KintexMicroBlazeBackend> h_mb(mb, 20'000U, 100'000U);
        CHECK(h_mb.map_region(MemRegionDesc{0U, MemPerm::ReadWrite, 0U, 0x8000'0000ULL, 0x10000ULL}) == Status::Ok);
        CHECK(mb.tlb_count() == 1U);
        CHECK(mb.execute_slot_op(0U, SlotOpCode::AieColumnReset, 0U) == Status::InvalidArg);
        CHECK(mb.execute_slot_op(0U, SlotOpCode::LoadPartialPdi, 0x1234U) == Status::Ok);
    }

    std::printf("PASS: test_hypervisor (all 8 hypcalls + 4 arch backends)\n");
    return 0;
}
