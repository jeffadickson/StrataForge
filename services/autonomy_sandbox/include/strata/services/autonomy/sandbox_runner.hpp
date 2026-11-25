#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace strata::services::autonomy {

/// Compact bounded bytecode / Wasm-AOT host-call instruction for isolated Class C/D autonomy.
enum class SandboxOp : uint8_t {
    Nop        = 0U,
    ReadTlmGt  = 1U, // If tlm[reg] > imm, set predicate = true
    EmitCmdIf  = 2U, // If predicate == true, submit command opcode = imm to allowlist gate
    Jump       = 3U, // Bounded loop / branch (preempted cleanly by step_budget!)
    Halt       = 4U,
};

struct SandboxInstr {
    SandboxOp op{SandboxOp::Nop};
    uint8_t   reg{0U};
    uint16_t  opcode{0U};
    int64_t   imm{0};
};

struct SandboxRunSummary {
    uint32_t steps_executed{0U};
    uint32_t cmds_forwarded{0U};
    uint32_t cmds_blocked{0U};
    bool     halted_cleanly{false};
    bool     budget_preempted{false};
};

/// Isolated Autonomy Sandbox.
/// Runs inside a dedicated unprivileged hypervisor partition with:
///  1. Hard step-budget preemption (prevents infinite-loop CPU starvation),
///  2. Read-Only telemetry snapshot inputs,
///  3. Strict Command Opcode Allowlist (blocks pyros/thrusters/DFX unlock even if script goes rogue).
template <size_t MaxAllowlist = 16U>
class AutonomySandbox {
public:
    [[nodiscard]] Status allow_opcode(uint16_t opcode) noexcept {
        for (size_t i = 0U; i < allow_count_; ++i) {
            if (allowlist_[i] == opcode) return Status::Ok;
        }
        if (allow_count_ >= MaxAllowlist) return Status::Full;
        allowlist_[allow_count_++] = opcode;
        return Status::Ok;
    }

    [[nodiscard]] bool is_opcode_allowed(uint16_t opcode) const noexcept {
        for (size_t i = 0U; i < allow_count_; ++i) {
            if (allowlist_[i] == opcode) return true;
        }
        return false;
    }

    /// Execute an uploaded autonomy module with hard step cap & command-allowlist filtering.
    template <typename CmdSubmitFn>
    [[nodiscard]] SandboxRunSummary
    execute(std::span<const SandboxInstr> program, std::span<const int64_t> ro_tlm,
            uint32_t max_steps, CmdSubmitFn&& submit_cmd) noexcept {
        SandboxRunSummary sum{};
        size_t            pc   = 0U;
        bool              pred = false;

        while (pc < program.size()) {
            if (sum.steps_executed >= max_steps) {
                sum.budget_preempted = true;
                return sum;
            }
            const auto& ins = program[pc];
            ++sum.steps_executed;

            switch (ins.op) {
                case SandboxOp::Nop:
                    ++pc;
                    break;
                case SandboxOp::ReadTlmGt:
                    pred = (ins.reg < ro_tlm.size()) && (ro_tlm[ins.reg] > ins.imm);
                    ++pc;
                    break;
                case SandboxOp::EmitCmdIf:
                    if (pred) {
                        if (is_opcode_allowed(ins.opcode)) {
                            submit_cmd(ins.opcode, ins.imm);
                            ++sum.cmds_forwarded;
                        } else {
                            ++sum.cmds_blocked;
                        }
                    }
                    ++pc;
                    break;
                case SandboxOp::Jump: {
                    const auto target = static_cast<size_t>(ins.imm);
                    pc                = (target < program.size()) ? target : program.size();
                    break;
                }
                case SandboxOp::Halt:
                    sum.halted_cleanly = true;
                    return sum;
            }
        }

        sum.halted_cleanly = true;
        return sum;
    }

private:
    std::array<uint16_t, MaxAllowlist> allowlist_{};
    size_t                             allow_count_{0U};
};

} // namespace strata::services::autonomy
