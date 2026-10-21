#!/usr/bin/env bash
# The resident machine, as a supervised process. This is what launchd (or
# systemd, or a bare shell) keeps alive; everything a person types arrives
# through the input pipe, and everything the machine says lands in the log.
#
# The pipe is opened READ-WRITE by this process on purpose: a reader-only open
# would see EOF the moment the last outside writer closed, and the machine
# would exit. Holding a writer here means `aos send` can come and go freely.
set -uo pipefail

CONF=${AOS_CONF:-$HOME/.config/aos.conf}
[ -f "$CONF" ] && . "$CONF"

AOS_ROOT=${AOS_ROOT:-$HOME/aos/root}
AOS_RUN=${AOS_RUN:-$HOME/aos/run}
AOS_BIN=${AOS_BIN:-}
AOS_PROVIDER=${AOS_PROVIDER:-}
AOS_MODEL=${AOS_MODEL:-}
AOS_PROVIDERS=${AOS_PROVIDERS:-}
AOS_PATH=${AOS_PATH:-}
AOS_BRIDGE=${AOS_BRIDGE:-}

# The launchd environment is nearly empty, so the interactive PATH recorded at
# install time is what lets the machine reach node (the bridge) and any CLI
# provider behind it.
[ -n "$AOS_PATH" ] && export PATH="$AOS_PATH"

if [ -z "$AOS_BIN" ] || [ ! -x "$AOS_BIN" ]; then
  echo "aos-session: no aios binary (AOS_BIN=$AOS_BIN); run: aos build" >&2
  exit 78
fi

# A configured mind is bound by the command surface at boot; with none set the
# machine still runs, it just has nobody resident to ask.
if [ -n "$AOS_PROVIDER" ]; then
  export AIOS_MODEL_PROVIDER="$AOS_PROVIDER"
  [ -n "$AOS_MODEL" ] && export AIOS_MODEL="$AOS_MODEL"
fi
[ -n "$AOS_PROVIDERS" ] && export ASSAY_PROVIDERS="$AOS_PROVIDERS"

# Name the bridge outright. A supervised process starts with a working
# directory of the supervisor's choosing, and the surface would otherwise fall
# back to a mind that only echoes.
here=$(cd "$(dirname "$0")/.." && pwd)
# A bridge named in the settings file wins over the one beside the source.
if [ -n "$AOS_BRIDGE" ] && [ -f "$AOS_BRIDGE" ]; then
  export AIOS_BRIDGE="$AOS_BRIDGE"
elif [ -f "$here/intelligence/assay/bridge.cjs" ]; then
  export AIOS_BRIDGE="$here/intelligence/assay/bridge.cjs"
fi
cd "$here" || true

# The machine finds its run directory (the pipe lock its watchdog checks, the
# scene file) through AOS_RUN, so it is the same one this front door uses.
export AOS_RUN
mkdir -p "$AOS_ROOT" "$AOS_RUN"
FIFO=$AOS_RUN/in
LOG=$AOS_RUN/session.log
[ -p "$FIFO" ] || { rm -f "$FIFO"; mkfifo -m 600 "$FIFO"; }

# A restart is a new process on the same root: the kernel identity, the object
# store and the file tree come back. Mark the boundary so the log reads honestly.
{
  echo
  echo "=== session start $(date '+%Y-%m-%d %H:%M:%S') · root $AOS_ROOT · mind ${AOS_PROVIDER:-none}/${AOS_MODEL:-none}"
} >> "$LOG"

exec 3<> "$FIFO"
exec "$AOS_BIN" --root "$AOS_ROOT" shell <&3 >> "$LOG" 2>&1
