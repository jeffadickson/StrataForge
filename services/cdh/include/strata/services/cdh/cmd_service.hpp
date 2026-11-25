#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/spsc_channel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace strata::services::cdh {

/// 4-stage Command Verification Report emitted to ground or command sequencer.
struct CmdVerificationReport {
    uint32_t  cmd_seq{0U};
    uint16_t  opcode{0U};
    uint8_t   target_partition{0U};
    CmdStage  stage{CmdStage::Accepted};
    CmdResult result{CmdResult::Ok};
    uint8_t   progress_step{0U};
    uint8_t   progress_pct{0U};
    uint64_t  tai_ns{0U};
};

static_assert(WirePod<CmdVerificationReport>);

/// Partition RBAC command policy (prevents unprivileged partitions from invoking critical opcodes).
struct PartitionAuthEntry {
    uint8_t  source_partition{0U};
    uint16_t min_opcode{0U};
    uint16_t max_opcode{0U};
    bool     require_sdls_auth{true};
    bool     enabled{false};
};

/// Command Service with partition RBAC, SDLS enforcement, and 4-stage verification reports.
template <size_t MaxInFlight = 16U, size_t MaxAuthRules = 16U, size_t ReportRingDepth = 32U>
    requires PowerOfTwo<ReportRingDepth>
class CmdService {
public:
    explicit CmdService(SpscRingStorage<CmdVerificationReport, ReportRingDepth>& report_ring) noexcept
        : report_pub_(report_ring, 0U, NullNotifier{}, GenId{0U}) {}

    [[nodiscard]] Status add_auth_rule(const PartitionAuthEntry& rule) noexcept {
        for (auto& r : auth_rules_) {
            if (!r.enabled) {
                r         = rule;
                r.enabled = true;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Stage 1 (Acceptance): Verify source RBAC + SDLS auth and register in-flight tracking.
    [[nodiscard]] Expected<uint32_t, CmdResult>
    accept_command(uint8_t source_partition, uint8_t target_partition, uint16_t opcode,
                   bool sdls_authenticated, TaiTimestamp now) noexcept {
        if (!is_authorized(source_partition, opcode, sdls_authenticated)) {
            emit_report(0U, opcode, target_partition, CmdStage::Accepted, CmdResult::Denied, 0U, 0U,
                        now);
            ++rejected_count_;
            return Unexpected(CmdResult::Denied);
        }

        auto* slot = alloc_inflight();
        if (slot == nullptr) {
            emit_report(0U, opcode, target_partition, CmdStage::Accepted, CmdResult::Busy, 0U, 0U,
                        now);
            return Unexpected(CmdResult::Busy);
        }

        const uint32_t seq     = next_seq_++;
        slot->active           = true;
        slot->cmd_seq          = seq;
        slot->opcode           = opcode;
        slot->target_partition = target_partition;
        slot->stage            = CmdStage::Accepted;

        emit_report(seq, opcode, target_partition, CmdStage::Accepted, CmdResult::Ok, 0U, 0U, now);
        ++accepted_count_;
        return seq;
    }

    /// Stage 2 (Execution Started).
    [[nodiscard]] Status report_started(uint32_t cmd_seq, TaiTimestamp now) noexcept {
        auto* slot = find_inflight(cmd_seq);
        if (slot == nullptr) return Status::InvalidArg;
        slot->stage = CmdStage::Started;
        emit_report(cmd_seq, slot->opcode, slot->target_partition, CmdStage::Started, CmdResult::Ok,
                    0U, 0U, now);
        return Status::Ok;
    }

    /// Stage 3 (Execution Progress — e.g., DFX bitstream load 50%, SBIT 90%).
    [[nodiscard]] Status report_progress(uint32_t cmd_seq, uint8_t step, uint8_t pct,
                                         TaiTimestamp now) noexcept {
        auto* slot = find_inflight(cmd_seq);
        if (slot == nullptr) return Status::InvalidArg;
        slot->stage = CmdStage::Progress;
        emit_report(cmd_seq, slot->opcode, slot->target_partition, CmdStage::Progress,
                    CmdResult::InProgress, step, pct, now);
        return Status::Ok;
    }

    /// Stage 4 (Execution Completed — releases in-flight tracking slot).
    [[nodiscard]] Status report_completed(uint32_t cmd_seq, CmdResult final_res,
                                          TaiTimestamp now) noexcept {
        auto* slot = find_inflight(cmd_seq);
        if (slot == nullptr) return Status::InvalidArg;
        emit_report(cmd_seq, slot->opcode, slot->target_partition, CmdStage::Completed, final_res,
                    0U, 100U, now);
        slot->active = false;
        ++completed_count_;
        return Status::Ok;
    }

    [[nodiscard]] uint32_t accepted_count() const noexcept { return accepted_count_; }
    [[nodiscard]] uint32_t rejected_count() const noexcept { return rejected_count_; }
    [[nodiscard]] uint32_t completed_count() const noexcept { return completed_count_; }

private:
    struct InFlightCmd {
        bool     active{false};
        uint32_t cmd_seq{0U};
        uint16_t opcode{0U};
        uint8_t  target_partition{0U};
        CmdStage stage{CmdStage::Accepted};
    };

    [[nodiscard]] bool is_authorized(uint8_t source_part, uint16_t opcode,
                                     bool sdls_auth) const noexcept {
        for (const auto& r : auth_rules_) {
            if (!r.enabled) continue;
            if (r.source_partition == source_part && opcode >= r.min_opcode &&
                opcode <= r.max_opcode) {
                return !r.require_sdls_auth || sdls_auth;
            }
        }
        return false;
    }

    InFlightCmd* alloc_inflight() noexcept {
        for (auto& s : inflight_) {
            if (!s.active) return &s;
        }
        return nullptr;
    }

    InFlightCmd* find_inflight(uint32_t seq) noexcept {
        for (auto& s : inflight_) {
            if (s.active && s.cmd_seq == seq) return &s;
        }
        return nullptr;
    }

    void emit_report(uint32_t seq, uint16_t opcode, uint8_t target_part, CmdStage stage,
                     CmdResult res, uint8_t step, uint8_t pct, TaiTimestamp now) noexcept {
        const CmdVerificationReport rpt{
            .cmd_seq          = seq,
            .opcode           = opcode,
            .target_partition = target_part,
            .stage            = stage,
            .result           = res,
            .progress_step    = step,
            .progress_pct     = pct,
            .tai_ns           = now.ns(),
        };
        (void)report_pub_.try_push(rpt);
    }

    SpscProducer<CmdVerificationReport, ReportRingDepth> report_pub_;
    std::array<InFlightCmd, MaxInFlight>                 inflight_{};
    std::array<PartitionAuthEntry, MaxAuthRules>         auth_rules_{};
    uint32_t                                             next_seq_{1U};
    uint32_t                                             accepted_count_{0U};
    uint32_t                                             rejected_count_{0U};
    uint32_t                                             completed_count_{0U};
};

} // namespace strata::services::cdh
