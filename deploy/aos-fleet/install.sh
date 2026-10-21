#!/bin/sh
# Install this machine's part in the AOS fleet as a system unit (run with sudo).
#   sudo deploy/aos-fleet/install.sh [user=$SUDO_USER] [member|hub|observer]
#     member    aos-fleet.service: /opt/aos/bin/aios joins the hub and stays (default)
#     hub       aos-hub.service:   /opt/aos/aios runs `fleet hub` (the rendezvous + relay)
#     observer  aos-observer.service: /opt/aos/aios writes /opt/aos/fleet-status.json
# hub and observer run /opt/aos/aios, the path deploy/aos-fleet-sync.sh copies
# new builds to before it restarts them; a member runs its own snapshot.
# A user with no sudo runs aos-member.service as a systemd --user unit instead
# (see the comment at its top).
#
# Every role needs the cluster key at /etc/aos/fleet.key first (root:<user>
# 0640) - the units will not start without it, and read it with `@FILE`, so it
# is never on a command line. Copy it from the hub host's /etc/aos/fleet.key:
#   ssh <hub-host> 'sudo cat /etc/aos/fleet.key' | sudo tee /etc/aos/fleet.key >/dev/null
#   sudo chown root:"$USER" /etc/aos/fleet.key && sudo chmod 640 /etc/aos/fleet.key
# (A hub still started the old way, with the key as a plain argument, exposes
# it in /proc/<pid>/cmdline; `aos-hub.service` does not.)
# Settings that differ per host (AOS_HUB=, AOS_HUB_PORT=) go in /etc/aos/fleet.env.
set -e
here=$(cd "$(dirname "$0")" && pwd)
user=${1:-${SUDO_USER:-arch}}
role=${2:-member}
case "$role" in
  member)
    install -d -m 755 /opt/aos/bin
    install -d -m 755 -o "$user" /opt/aos/fleet-root
    install -m 755 "$here/../../build/aios" /opt/aos/bin/aios          # a snapshot: rebuilds in the tree do not disturb it
    install -m 755 "$here/fleet-member.sh" /opt/aos/bin/fleet-member.sh
    unit=aos-fleet;;
  hub)
    install -d -m 755 /opt/aos
    install -d -m 755 -o "$user" /opt/aos/hub-root
    install -m 755 "$here/../../build/aios" /opt/aos/aios
    unit=aos-hub;;
  observer)
    install -d -m 755 -o "$user" /opt/aos
    install -d -m 755 -o "$user" /opt/aos/observer-root
    install -m 755 "$here/../../build/aios" /opt/aos/aios
    unit=aos-observer;;
  *) echo "install.sh: role must be member, hub or observer (got '$role')" >&2; exit 2;;
esac
sed "s/@USER@/$user/" "$here/$unit.service" > /etc/systemd/system/$unit.service
systemctl daemon-reload
systemctl enable --now $unit.service
sleep 3
systemctl --no-pager status $unit.service | head -5
