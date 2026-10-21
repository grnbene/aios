#!/bin/sh
# This machine's membership in the AOS fleet: attach to chi-il-1's hub and
# stay. The cluster key is read by aios itself from /etc/aos/fleet.key
# (root:<user> 0640) - never from this file or the unit, and never expanded
# here: `fleet join ... @FILE` reads the file, so the key is not on any
# command line (`ps` shows every process's arguments to every user; the old
# KEY=$(cat ...) put it there). `sleep` keeps the attached process alive;
# systemd restarts it (and so re-attaches) when it ends.
set -e
exec /opt/aos/bin/aios --root /opt/aos/fleet-root exec "fleet join ${AOS_HUB:?set AOS_HUB in /etc/aos/fleet.env} ${AOS_HUB_PORT:-7420} @/etc/aos/fleet.key" "sleep 4000000"
