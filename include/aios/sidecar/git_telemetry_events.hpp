// aios/sidecar/git_telemetry_events.hpp
#pragma once

#include <cstdint>

namespace aios::sidecar {

// Dedicated Service ID for the Git Subsystem in aios-engine
constexpr uint32_t kServiceGitAgent = 0x00004749; // "GI" in ASCII

// Event Types for Span Correlation
enum class GitTelemetryEvent : uint16_t {
  WorkspaceInit      = 100, // In-memory snapshot created from commit
  FileRead           = 101, // Zero-copy blob resolution via ModuleVault
  FileStage          = 102, // Blob hash staged to PackageManifest
  PatchDryRun        = 103, // PatchEngine validation
  MergeAttempt       = 104, // 3-way tree merge initiated
  MergeClean         = 105, // Fast-forward or clean 3-way merge completed
  MergeConflict      = 106, // Direct conflict detected on AST/file
  TokenCheckDenied   = 107, // CapabilityGuard rejected access
  RemotePushFinished = 108  // Packfile serialized and sent to remote
};

// Status Codes
constexpr uint16_t kStatusOk       = 200;
constexpr uint16_t kStatusDenied   = 403;
constexpr uint16_t kStatusNotFound = 404;
constexpr uint16_t kStatusConflict = 409;
constexpr uint16_t kStatusIoError  = 500;

// Event Types for EventFabric messaging
enum class GitEventType : uint16_t {
  None         = 0,
  IntentToEdit = 1,
  FileStaged   = 2,
  BranchMerged = 3
};

struct GitEventPayload {
  GitEventType type{GitEventType::None};
  uint16_t agent_id{0};
  char path[128]{0};
  char sha256_or_oid[65]{0};
  uint8_t _reserved[35]{0};
};
static_assert(sizeof(GitEventPayload) <= 232, "GitEventPayload must fit into EventMessage payload");

} // namespace aios::sidecar
