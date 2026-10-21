#pragma once
/* The bring-up dashboard's window onto the fleet (metal/arm64/fleet_shell.cpp).
 * Plain structs and C calls: desktop.cpp is built freestanding, without the
 * standard library the fleet code lives in. */
#include <stddef.h>
#include <stdint.h>

struct FleetStatus {
  bool attached;            /* on a hub right now */
  bool configured;          /* FLEET.CFG is on the card: this board joins at boot */
  bool joining;             /* not on a hub, and an automatic attempt is pending */
  uint32_t retry_in_s;      /* ...in this many seconds (0 = as soon as eth0 has a lease) */
  char hub[64];             /* "<hub-ip>:7420", or "" */
  char self[16];            /* this board's kernel identity, short form */
  uint32_t roster_count;    /* members the hub reports, self excluded */
  char roster[16][16];      /* the first 16 of them, short form */
  uint32_t served_count;    /* peers that have opened a circuit to this board */
  uint64_t served_requests; /* requests they have made, all told */
};

struct FleetMember {
  char id[16];              /* short kernel identity */
  bool reached;             /* the node handshake and a telemetry call both answered */
  uint32_t rtt_ms;          /* that telemetry round trip, through the hub */
  uint32_t cpus, slots, jobs;
  uint64_t memory_in_use, total_memory;
  uint32_t databases;       /* hosted databases it advertises */
  char note[72];            /* why it was not reached */
};

extern "C" {
void fleet_status(FleetStatus* out);
/* Asks every member on the roster for its telemetry, over the node link.
 * Blocks while it does (a handshake per peer the first time, then a round
 * trip each); returns how many rows it filled. */
size_t fleet_survey(FleetMember* out, size_t max);
/* Join now from FLEET.CFG (on the next poll), or leave the hub and stay off
 * it until the next join. */
void fleet_request_join();
void fleet_leave();
/* desktop.cpp: called once per roster member, before that member's own
 * board_reach()/telemetry() round trip, so a slow or dead peer repaints a
 * "surveying N/M" message instead of leaving the whole screen frozen with
 * no way to tell "still working" from "crashed". */
void fleet_survey_progress(size_t index, size_t total, const char* peer_id);
}
