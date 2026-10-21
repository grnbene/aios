#pragma once
// Agent Control: a wire protocol for programs that are not aios to create a
// capability-scoped agent entity, bind a real Intelligence to it, grant and
// revoke its authority, drive perceive->think->act cycles, and read back
// the audit ledger of exactly what it was allowed to do and did. Same frame
// shape as compute_serve (4-byte big-endian length + JSON), a different
// vocabulary of ops: agent lifecycle and capability, not compute graphs.
//
// Every MUTATING op is implemented by calling straight through to the
// existing Surface API (Surface::grant/request/spawn/bind_mind/cycle/
// terminate/revoke) -- never Kernel::grant, which mints capabilities
// unattenuated. This is the load-bearing property of the whole service: an
// external caller can never come away holding authority this service's own
// resident identity didn't already hold.
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::channel {

struct AgentControlOptions {
  uint16_t port = 7312;
  bool loopback_only = true;
  std::string auth_secret;      // empty: no auth required (dev only -- see agent-control.cpp)
  int frame_timeout_ms = 5000;
  int max_cycle_run_n = 100;    // cap on cycle.run_n's n, so one request can't run forever
  uint64_t replay_window_s = 300;  // a signed request's "ts" must be this close to the server clock
  // Minds an agent can be bound to as kind "external", by name -> the command that runs one (see agent.bind_mind):
  // "[NAME=value ...] program [args ...]", words split on spaces, quotes keeping a word together. Set at start
  // (--external-mind name=command, repeatable); a caller can only pick among these, never name a command.
  std::map<std::string, std::string> external_minds;
};

// Ops (request "op" -> what it does -> the exact Surface/Kernel call):
//
//   agent.create      {"op":"agent.create","label":"...","parent":"<id>"?}
//                      -> Surface::spawn(label, parent)
//   agent.bind_mind    {"op":"agent.bind_mind","entity":"<id>","kind":"qwen",
//                       "url":"...","model":"...","credential_ref":"file"?}
//                      -> make_qwen_intelligence + optional set_qwen_credential,
//                         then Surface::bind_mind(entity, std::move(mind))
//                      {"op":"agent.bind_mind","entity":"<id>","kind":"external","mind":"<name>",
//                       "provider":"..."?,"model":"..."?,"effort":"..."?,"host":"https://..."?}
//                      -> spawn_external_intelligence of the command the service was started with for
//                         <name> (AgentControlOptions::external_minds), with AIOS_MODEL_PROVIDER, AIOS_MODEL
//                         and AIOS_MIND_EFFORT set in that mind's environment from provider/model/effort
//                         (letters, digits and . - _ : / only), and AIOS_MODEL_HOST from host (an http(s)
//                         address: the API this agent's model is called on), then Surface::bind_mind. Each agent gets
//                         its own process, and so its own model.
//   agent.send         {"op":"agent.send","entity":"<id>","input":"..."}
//                      -> Surface::act_as(entity) then .set_state(context, input) --
//                         what the entity perceives on its NEXT cycle.run
//   agent.terminate    {"op":"agent.terminate","entity":"<id>"}
//                      -> Surface::terminate(entity)
//   agent.list         {"op":"agent.list"}
//                      -> every entity created via agent.create on THIS service
//                         process, from any caller, with label + bound mind +
//                         cycle count -- the dashboard's landing view
//   cap.grant          {"op":"cap.grant","to":"<id>","kind":"memory",
//                       "resource_id":"*"?,"rights":"read,write",
//                       "ttl_s":0?,"scope":0?}
//                      -> Surface::grant(to, ResourceRef{kind,id}, rights, ttl, scope)
//   cap.exercise       {"op":"cap.exercise","entity":"<id>","kind":"storage","resource":"path:/repo/a.txt",
//                       "rights":"write","action":"..."?}
//                      -> Surface::act_as(entity) then .exercise(...): allowed when the entity holds a capability
//                         covering it, refused when not, and either way an Exercised / REFUSED ledger event.
//                         Answers {"ok":true,"allowed":bool,"error":"..."?}.
//                         "resource" (here and in cap.grant / cap.request) is a name the service hashes into an id
//                         ("*" is the whole kind); "resource_id" is an id, as before.
//   agent.checkpoint   {"op":"agent.checkpoint","entity":"<id>"} -> Surface::checkpoint_of(entity); {"checkpoint":path}
//   agent.rewind       {"op":"agent.rewind","entity":"<id>","cycle":N}
//                      -> Surface::rewind(the checkpoint recorded at the start of cycle N); the entity keeps its identity
//   cycle.record       {"op":"cycle.record","entity":"<id>","cycle":N,"action","target","verdict","reason","reply",
//                       "observation","checkpoint"?,"tree"?} -> kept (and appended under the kernel root)
//   cycle.records      {"op":"cycle.records","entity":"<id>","cycle":N?} -> {"cycles":[...]}
//   cap.request        {"op":"cap.request","entity":"<id>","kind":"...",
//                       "resource_id":"*"?,"rights":"...","ttl_s":0?}
//                      -> Surface::act_as(entity) then .request(...) on that
//                         scoped Surface -- the AGENT requests from its own
//                         held authority, not the service granting on its behalf
//   cap.revoke         {"op":"cap.revoke","capability":"<id>"}
//                      -> Surface::revoke(capability)
//   cycle.run          {"op":"cycle.run","entity":"<id>"}
//                      -> Surface::cycle(entity), full CycleReport mirrored back
//   cycle.run_n        {"op":"cycle.run_n","entity":"<id>","n":5}
//                      -> Surface::cycle(entity) n times (capped), reports as a list
//   ledger.events      {"op":"ledger.events","entity":"<id>","since":0}
//                      -> Kernel::authority().events() filtered by actor/subject,
//                         from index `since` onward; response carries "cursor"
//                         (the new events().size()) to pass back as `since` next
//                         time -- append-only ledger, so this needs no new state
//   entity.status      {"op":"entity.status","entity":"<id>"}
//                      -> read-only entity + mind-binding status
//   svc.ping / svc.stats
//
// Every response: {"ok":bool,"id":"<echoed>","error":"<string>"?, ...}.
//
// Auth: if AgentControlOptions::auth_secret is non-empty, every request must
// carry {"auth": hex(hmac_sha256(secret, canonical_request_body))} -- see
// agent_control_serve.cpp for the exact canonicalization (the body with the
// "auth" field itself removed, so it signs what it's authenticating).
Result<void> agent_control_serve(Kernel& k, Surface& svc, const AgentControlOptions& o,
                                 std::function<void(const std::string&)> log,
                                 std::atomic<bool>* stop = nullptr);

}  // namespace aios::channel
