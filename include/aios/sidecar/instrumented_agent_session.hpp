// aios/sidecar/instrumented_agent_session.hpp
#pragma once

#include "aios/core/result.hpp"
#include "aios/sidecar/binary_telemetry.hpp"
#include "aios/sidecar/capability_guard.hpp"
#include "aios/sidecar/event_fabric.hpp"
#include "aios/sidecar/git_telemetry_events.hpp"
#include "aios/sidecar/vault_workspace.hpp"
#include "aios/sidecar/module_vault.hpp"
#include <chrono>
#include <cstring>
#include <span>
#include <string>

namespace aios::sidecar {

using ::aios::Result;
using ::aios::Status;
using ::aios::Ok;

class InstrumentedAgentSession {
public:
  InstrumentedAgentSession(uint64_t trace_id,
                           uint16_t agent_id,
                           const std::string& repo_name,
                           const std::string& branch,
                           const CapabilityToken& token,
                           BinaryTelemetryCore& telemetry,
                           CapabilityGuard& guard,
                           EventFabric& fabric,
                           VaultWorkspace& vws,
                           uint64_t first_span_id = 1)   // callers that open many sessions on one trace keep span ids distinct
      : trace_id_(trace_id),
        span_counter_(first_span_id),
        agent_id_(agent_id),
        repo_name_(repo_name),
        branch_(branch),
        token_(token),
        telemetry_(telemetry),
        guard_(guard),
        fabric_(fabric),
        vws_(vws) {}

  // 1. Instrumented File Read
  Result<std::span<const uint8_t>> read_file(const std::string& rel_path) {
    uint64_t start_ns = now_ns();
    uint64_t span_id = next_span();
    std::string res_key = "repo:" + repo_name_ + "/" + rel_path;

    if (!guard_.validate_for_resource(token_, res_key, CapabilityRight::Read)) {
      telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                        static_cast<uint16_t>(GitTelemetryEvent::TokenCheckDenied),
                        kStatusDenied, duration_us(start_ns), agent_id_, rel_path.c_str());
      return Result<std::span<const uint8_t>>(Status::Denied, "Capability check failed");
    }

    auto data = vws_.read(rel_path);
    if (!data) {
      telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                        static_cast<uint16_t>(GitTelemetryEvent::FileRead),
                        kStatusNotFound, duration_us(start_ns), agent_id_, rel_path.c_str());
      return Result<std::span<const uint8_t>>(Status::NotFound, rel_path + " not found");
    }

    telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                      static_cast<uint16_t>(GitTelemetryEvent::FileRead),
                      kStatusOk, duration_us(start_ns), static_cast<uint32_t>(data->size()), rel_path.c_str());
    return *data;
  }

  // 2. Instrumented File Stage
  Result<void> stage_file(const std::string& rel_path, const void* data, size_t len) {
    uint64_t start_ns = now_ns();
    uint64_t span_id = next_span();
    std::string res_key = "repo:" + repo_name_ + "/" + rel_path;

    if (!guard_.validate_for_resource(token_, res_key, CapabilityRight::Write)) {
      telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                        static_cast<uint16_t>(GitTelemetryEvent::TokenCheckDenied),
                        kStatusDenied, duration_us(start_ns), agent_id_, rel_path.c_str());
      return Result<void>(Status::Denied, "Capability check failed");
    }

    // Publish Intent to peers
    GitEventPayload ev{};
    ev.type = GitEventType::IntentToEdit;
    ev.agent_id = agent_id_;
    std::strncpy(ev.path, rel_path.c_str(), sizeof(ev.path) - 1);
    fabric_.publish("git:" + branch_, &ev, sizeof(ev), agent_id_);

    // Stage in Vault
    vws_.write(rel_path, data, len);

    // Broadcast staged event
    ev.type = GitEventType::FileStaged;
    auto h = ModuleVault::compute_sha256(data, len);
    std::strncpy(ev.sha256_or_oid, h.c_str(), sizeof(ev.sha256_or_oid) - 1);
    fabric_.publish("git:" + branch_, &ev, sizeof(ev), agent_id_);

    telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                      static_cast<uint16_t>(GitTelemetryEvent::FileStage),
                      kStatusOk, duration_us(start_ns), static_cast<uint32_t>(len), rel_path.c_str());
    return Ok();
  }

  // 3. Instrumented Commit & Merge
  Result<std::string> commit_and_merge(const std::string& msg) {
    uint64_t start_ns = now_ns();
    uint64_t span_id = next_span();

    std::string branch_resource = "repo:" + repo_name_ + ":branch:" + branch_;
    if (!guard_.validate_for_resource(token_, branch_resource, CapabilityRight::Write)) {
      telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                        static_cast<uint16_t>(GitTelemetryEvent::TokenCheckDenied),
                        kStatusDenied, duration_us(start_ns), agent_id_, branch_.c_str());
      return Result<std::string>(Status::Denied, "Capability check failed");
    }

    telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                      static_cast<uint16_t>(GitTelemetryEvent::MergeAttempt),
                      kStatusOk, 0, agent_id_, branch_.c_str());

    auto report = vws_.commit_and_merge(branch_, msg);
    uint32_t dur_us = duration_us(start_ns);

    if (!report.error.empty()) {
      // Not a conflict: the commit could not be journaled, so nothing was merged.
      telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                        static_cast<uint16_t>(GitTelemetryEvent::MergeConflict),
                        kStatusIoError, dur_us, 0, "journal");
      return Result<std::string>(Status::IoError, report.error);
    }

    if (!report.clean) {
      telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                        static_cast<uint16_t>(GitTelemetryEvent::MergeConflict),
                        kStatusConflict, dur_us,
                        static_cast<uint32_t>(report.conflict_paths.size()),
                        report.conflict_paths.empty() ? "tree" : report.conflict_paths[0].c_str());
      return Result<std::string>(Status::Conflict, "Branch diverged with conflict");
    }

    // Success
    telemetry_.record(trace_id_, span_id, kServiceGitAgent,
                      static_cast<uint16_t>(GitTelemetryEvent::MergeClean),
                      kStatusOk, dur_us, agent_id_, report.new_commit_oid.c_str());

    // Broadcast to ring buffer
    GitEventPayload ev{};
    ev.type = GitEventType::BranchMerged;
    ev.agent_id = agent_id_;
    std::strncpy(ev.sha256_or_oid, report.new_commit_oid.c_str(), sizeof(ev.sha256_or_oid) - 1);
    fabric_.publish("git:" + branch_, &ev, sizeof(ev), agent_id_);

    return report.new_commit_oid;
  }

  uint64_t trace_id() const { return trace_id_; }
  uint16_t agent_id() const { return agent_id_; }

private:
  static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
  }
  static uint32_t duration_us(uint64_t start_ns) {
    return static_cast<uint32_t>((now_ns() - start_ns) / 1000);
  }
  uint64_t next_span() { return span_counter_++; }

  uint64_t trace_id_;
  uint64_t span_counter_;
  uint16_t agent_id_;
  std::string repo_name_;
  std::string branch_;
  CapabilityToken token_;
  BinaryTelemetryCore& telemetry_;
  CapabilityGuard& guard_;
  EventFabric& fabric_;
  VaultWorkspace& vws_;
};

} // namespace aios::sidecar
