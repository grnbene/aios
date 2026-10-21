#pragma once
// A node's live self-report. Capacity is static (cpus, memory, configured
// slots); load is what is running here right now. The scheduler compares these
// across the fleet to decide where a job should live. Nothing here knows what a
// job IS — only how much of the machine it occupies.
#include <vector>
#include <string>
#include "aios/core/bytes.hpp"
#include "aios/core/ids.hpp"

namespace aios {

struct NodeTelemetry {
  Id128 kernel;                    // which kernel is reporting
  uint32_t logical_cpus = 1;       // hardware parallelism
  uint32_t slots = 1;              // jobs this node will run concurrently (its scheduling capacity)
  uint64_t total_memory = 0;
  uint64_t available_memory = 0;
  uint64_t memory_in_use = 0;      // fabric bytes currently held
  uint32_t movable_entities = 0;   // non-kernel, non-resident jobs hosted here (the movable load)
  uint64_t heartbeat = 0;          // advances every report; a stalled beat means a sick node
  uint64_t wall_ns = 0;            // the reporter's clock, for freshness
  std::vector<std::string> custom_kernels; // registered custom ops hosted on this node
  
  struct HostedDbInfo {
    std::string db_name;
    uint16_t asql_port = 0;      // e.g. 7311 (compute RPC)
    uint16_t mysql_port = 0;     // e.g. 3307 (MySQL wire protocol)
    uint32_t table_count = 0;
    bool read_only = false;        // a cdc mirror: replication overwrites what is written here
    bool fleet_read_only = false;  // fleet peers' SQL (sql_on, ON <machine>) may only read it
  };
  std::vector<HostedDbInfo> hosted_databases;

  // How oversubscribed the node is, normalized so >= 1.0 means at/over capacity.
  // Whichever axis is tighter — jobs-per-slot or memory — sets the pressure.
  double pressure() const {
    double cpu = slots ? double(movable_entities) / double(slots) : double(movable_entities);
    double mem = total_memory ? double(memory_in_use) / double(total_memory) : 0.0;
    return cpu > mem ? cpu : mem;
  }
  // Jobs it could still take before its slots are full (may be negative == oversubscribed).
  int free_slots() const { return int(slots) - int(movable_entities); }

  void encode(Writer& w) const {
    w.id(kernel); w.u32(logical_cpus); w.u32(slots);
    w.u64(total_memory); w.u64(available_memory); w.u64(memory_in_use);
    w.u32(movable_entities); w.u64(heartbeat); w.u64(wall_ns);
    w.u32((uint32_t)custom_kernels.size());
    for (const auto& k : custom_kernels) w.str(k);
    w.u32((uint32_t)hosted_databases.size());
    for (const auto& db : hosted_databases) {
      w.str(db.db_name);
      w.u16(db.asql_port);
      w.u16(db.mysql_port);
      w.u32(db.table_count);
      // One byte, two flags: a decoder that predates fleet_read_only reads
      // any non-zero byte as read-only, which is what such a member is to it.
      w.u8((db.read_only ? 1 : 0) | (db.fleet_read_only ? 2 : 0));
    }
  }
  static NodeTelemetry decode(Reader& r) {
    NodeTelemetry t;
    t.kernel = r.id(); t.logical_cpus = r.u32(); t.slots = r.u32();
    t.total_memory = r.u64(); t.available_memory = r.u64(); t.memory_in_use = r.u64();
    t.movable_entities = r.u32(); t.heartbeat = r.u64(); t.wall_ns = r.u64();
    if (r.ok() && r.remaining() > 0) {
      uint32_t count = r.u32();
      for (uint32_t i = 0; i < count && r.ok(); ++i) {
        t.custom_kernels.push_back(r.str());
      }
    }
    if (r.ok() && r.remaining() > 0) {
      uint32_t db_count = r.u32();
      for (uint32_t i = 0; i < db_count && r.ok(); ++i) {
        HostedDbInfo db;
        db.db_name = r.str();
        db.asql_port = r.u16();
        db.mysql_port = r.u16();
        db.table_count = r.u32();
        const uint8_t flags = r.u8();
        db.read_only = (flags & 1) != 0;
        db.fleet_read_only = (flags & 2) != 0;
        t.hosted_databases.push_back(db);
      }
    }
    return t;
  }
};

}  // namespace aios
