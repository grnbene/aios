#pragma once
// The machine's SQL engine, reached through the substrate instead of a port.
//
// One SqlDatabase per kernel, opened once on the kernel's own object store and
// shared by everything on the machine that wants SQL (grain, the desktop's
// database pane, fleet peers). Two engines on one store each hold the tables in
// memory and flush over one another, so nothing should build its own.
//
// Across machines, a query rides the node link - the same authenticated, end
// to end encrypted circuit `fleet run` uses, addressed by kernel identity
// through the hub - rather than the MySQL wire to a host:port. A fleet peer
// behind the hub has no address anyone can dial (its node address is
// "relay:<id>"), so the wire could never reach it.
#include <functional>
#include <memory>
#include <string>
#include "aios/core/result.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios {
class Kernel;
class NodeService;
class RemoteNode;
}  // namespace aios

namespace aios::userland {

// This kernel's engine (opened on first use, on "sql_db"), or null when it did
// not open on this substrate.
SqlDatabase* machine_sql(Kernel& k);
// The engine if one is already open on this kernel, never opening one (a
// clock ticking its Things must not load a store just to find it has none).
SqlDatabase* machine_sql_if_open(Kernel& k);

// Make an engine the process already runs the machine's engine (compute
// serve's mirror), instead of opening a second one on the same store.
// read_only: fleet peers get SELECT/SHOW/DESCRIBE/EXPLAIN only.
void set_machine_sql(Kernel& k, SqlDatabase* db, bool read_only);

// Answer fleet peers' SQL requests on this node's link from machine_sql(k).
void serve_machine_sql(NodeService& node, Kernel& k);

// The Things' fleet hook (ON <machine>, ON ALL, MIRROR, MACHINES) for this
// kernel. Installed by machine_sql()/set_machine_sql(), serve_machine_sql()
// (fleet join) and `sql query`; harmless to install again.
void install_things_fleet(Kernel& k);

// Whether fleet peers may WRITE through this machine's SQL engine (sql_on,
// ... ON <machine>). Off unless this member opts in: `fleet sql writes=on`,
// kept in <root>/fleet_sql.conf as `writes=1`. Reads are always answered.
bool fleet_sql_writes_allowed(Kernel& k);
Result<void> set_fleet_sql_writes(Kernel& k, bool on);
// True when a fleet peer's write here would be refused: the machine engine is
// a read-only mirror (compute serve), or writes were never allowed.
bool machine_sql_is_fleet_read_only(Kernel& k);

// How this machine opens a node link to a fleet peer named by an identity
// prefix. The default goes through the kernel's own fleet membership
// (Kernel::fleet_join / reach); the Pi, whose hub link is polled and lives
// outside the Kernel, installs its own.
using PeerReach = std::function<Result<std::shared_ptr<RemoteNode>>(Kernel& k, const std::string& peer_prefix)>;
void set_peer_reach(PeerReach reach);
// Who is on the fleet, for the same kind of process: a kernel's own relay by
// default; the Pi's link lives outside its kernel and says so here.
using PeerRoster = std::function<std::vector<Id128>(Kernel& k)>;
void set_peer_roster(PeerRoster roster);
// Before the kernel goes: no fleet round starts after this (one in flight is
// waited for), and the kernel's engine is closed - its clock thread joined -
// while everything it uses still exists.
void machine_sql_shutdown(Kernel& k);

// One statement on a fleet peer's engine.
Result<SqlQueryResult> sql_on_peer(Kernel& k, const std::string& peer_prefix, const std::string& sql, int timeout_ms = 120000);

}  // namespace aios::userland

namespace aios::userland::grain { class Vm; }
namespace aios::userland {
// sql(statement) and sql_on(peer, statement) for a grain VM on kernel k.
void install_sql_builtins(grain::Vm& vm, Kernel& k);
}  // namespace aios::userland
