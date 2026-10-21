#!/usr/bin/env bash
# aos — the front door to a resident AIOS on a hosted substrate (macOS or
# Linux). The bare-metal machine has metal/aos-vm.sh; this drives the machine
# that runs directly on this host.
#
#   aos up                     start the machine (through launchd/systemd once installed)
#   aos down                   stop it
#   aos restart                stop, start, same root
#   aos status                 what is running, which root, which mind, uptime
#   aos send "<line>"          type one line, print what the machine said
#   aos ask "<question>"       ask the resident mind and wait for the answer
#   aos console                attach: what you type goes to the machine
#   aos think "<job>"          hand the mind a long job without waiting (watch: aos log)
#   aos log [n]                the last n lines the machine said (default 40)
#   aos mind <provider> <model>   set the resident mind, then restart
#   aos mind off               no resident mind
#   aos root <path>            which root to run on (its identity lives there)
#   aos build                  configure and build the binary this front door runs
#   aos install [--dry-run]    keep it alive and start it at login (launchd on
#                              macOS, a systemd --user unit on Linux)
#   aos uninstall              stop supervising it
#   aos config                 print the settings file
#   aos desktop [--tui]        boot the full native OS graphical workstation desktop
#   aos gui                    alias for aos desktop
#
# Settings live in ~/.config/aos.conf: AOS_ROOT AOS_RUN AOS_BIN AOS_PROVIDER
# AOS_MODEL AOS_PROVIDERS AOS_PATH AOS_BRIDGE. On Linux the unit is
# $XDG_CONFIG_HOME/systemd/user/$AOS_UNIT.service (AOS_UNIT defaults to aos).

set -uo pipefail

self=$0; [ -L "$self" ] && self=$(readlink "$self")
here=$(cd "$(dirname "$self")/.." && pwd)
repo=$(cd "$here/.." && pwd)
CONF=${AOS_CONF:-$HOME/.config/aos.conf}
LABEL=com.qbcsys.aios
PLIST=$HOME/Library/LaunchAgents/$LABEL.plist
OS=$(uname -s)
AOS_UNIT=${AOS_UNIT:-aos}
UNIT_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
UNIT=$UNIT_DIR/$AOS_UNIT.service
SYSTEMCTL=${AOS_SYSTEMCTL:-systemctl}

[ -f "$CONF" ] && . "$CONF"
AOS_ROOT=${AOS_ROOT:-$HOME/aos/root}
AOS_RUN=${AOS_RUN:-$HOME/aos/run}
AOS_BIN=${AOS_BIN:-}
AOS_PROVIDER=${AOS_PROVIDER:-}
AOS_MODEL=${AOS_MODEL:-}
AOS_PROVIDERS=${AOS_PROVIDERS:-}
AOS_PATH=${AOS_PATH:-}
AOS_BRIDGE=${AOS_BRIDGE:-}
FIFO=$AOS_RUN/in
LOG=$AOS_RUN/session.log
SESSION=$here/scripts/aos-session.sh
electron_bin=""
port=7440

say() { printf '%s\n' "$*" >&2; }
die() { say "aos: $*"; exit 1; }

save() {
  mkdir -p "$(dirname "$CONF")"
  printf 'AOS_ROOT=%q\nAOS_RUN=%q\nAOS_BIN=%q\nAOS_PROVIDER=%q\nAOS_MODEL=%q\nAOS_PROVIDERS=%q\nAOS_PATH=%q\nAOS_BRIDGE=%q\n' \
    "$AOS_ROOT" "$AOS_RUN" "$AOS_BIN" "$AOS_PROVIDER" "$AOS_MODEL" "$AOS_PROVIDERS" "$AOS_PATH" "$AOS_BRIDGE" > "$CONF"
}

# The binary this front door drives: whatever was configured, else whichever
# build tree exists beside the source.
find_bin() {
  [ -n "$AOS_BIN" ] && [ -x "$AOS_BIN" ] && { echo "$AOS_BIN"; return 0; }
  for b in "$here/build/aios" "$here/build-mac/aios"; do
    [ -x "$b" ] && { echo "$b"; return 0; }
  done
  return 1
}

# The machine is identified by the root it runs on, so two roots can run at once.
pid_of() { pgrep -f "aios --root $AOS_ROOT shell" 2>/dev/null | head -1; }
running() { [ -n "$(pid_of)" ]; }
# Supervised means a supervisor definition exists: the launchd plist on macOS,
# the systemd --user unit elsewhere.
supervised() {
  if [ "$OS" = "Darwin" ]; then [ -f "$PLIST" ]; else [ -f "$UNIT" ]; fi
}
# Installed is not the same as loaded: `down` deliberately unloads the agent so
# that KeepAlive stops resurrecting it, and `up` then has to load it back.
loaded() { launchctl print "gui/$(id -u)/$LABEL" >/dev/null 2>&1; }
# Colour codes, the prompt the machine reprints for piped input, and the
# markers `send` uses to close an exchange are all noise to a reader.
# The prompt has to come off before a marker can be recognised, since the
# machine reprints `aios> ` ahead of whatever it says next.
strip_noise() {
  LC_ALL=C tr -d '\r' | awk '
    {
      gsub(/\033\[[0-9;?]*[a-zA-Z]/, "")
      while (index($0, "aios> ") == 1) $0 = substr($0, 7)
      if ($0 ~ /^(echo )?__aos_(think_)?[0-9]+_[0-9]+$/) next
      print
    }'
}

ensure_bin() {
  AOS_BIN=$(find_bin) || die "no aios binary yet — run: aos build"
  save
}

start() {
  running && { say "already up (pid $(pid_of))"; return 0; }
  ensure_bin
  mkdir -p "$AOS_ROOT" "$AOS_RUN"
  if supervised && [ "$OS" != "Darwin" ]; then
    "$SYSTEMCTL" --user start "$AOS_UNIT.service" || die "systemctl --user start $AOS_UNIT refused"
  elif supervised; then
    if loaded; then
      launchctl kickstart -k "gui/$(id -u)/$LABEL" >/dev/null 2>&1
    else
      launchctl bootstrap "gui/$(id -u)" "$PLIST" >/dev/null 2>&1 || launchctl load -w "$PLIST" >/dev/null 2>&1
    fi
  else
    AOS_CONF=$CONF nohup "$SESSION" >>"$AOS_RUN/session.err" 2>&1 &
  fi
  for _ in $(seq 1 40); do running && break; sleep 0.25; done
  running || { say "the machine did not come up; look at $AOS_RUN/session.err and $LOG"; return 1; }
  say "up (pid $(pid_of)) · root $AOS_ROOT · mind ${AOS_PROVIDER:-none}"
}

stop() {
  # Under launchd the supervisor must be told, or KeepAlive simply respawns it.
  # Under systemd, Restart=always would respawn a killed process the same way.
  if supervised && [ "$OS" != "Darwin" ]; then
    "$SYSTEMCTL" --user stop "$AOS_UNIT.service" >/dev/null 2>&1
  elif supervised; then
    launchctl bootout "gui/$(id -u)/$LABEL" >/dev/null 2>&1 || launchctl unload "$PLIST" >/dev/null 2>&1
  fi
  p=$(pid_of); [ -n "$p" ] && kill "$p" 2>/dev/null
  for _ in $(seq 1 20); do running || break; sleep 0.25; done
  if running; then p=$(pid_of); kill -9 "$p" 2>/dev/null; sleep 0.5; fi
  running && say "still up (pid $(pid_of))" || say "down"
}

# One line in, exactly that line's answer out. A marker command closes the
# exchange, which is the only way to know the machine finished when its input
# is a pipe rather than a terminal.
# One pipe, one marker, so one exchange at a time. Without this, a panel
# refreshing while someone types in the shell reads the other's answer. mkdir
# is the atomic primitive every host here has; a holder that died leaves the
# directory behind, so a long wait reclaims it rather than wedging forever.
LOCKDIR=$AOS_RUN/exchange.lock
aos_lock() {
  local waited=0 pid
  while ! mkdir "$LOCKDIR" 2>/dev/null; do
    waited=$((waited + 1))
    if [ -f "$LOCKDIR/pid" ]; then
      pid=$(cat "$LOCKDIR/pid" 2>/dev/null)
      if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$LOCKDIR/pid" 2>/dev/null
        rmdir "$LOCKDIR" 2>/dev/null
        continue
      fi
    elif [ "$waited" -gt 40 ]; then
      # Ten seconds with no pid file at all: a lock directory left behind by an
      # older process that never recorded its owner. A live holder writes its
      # pid in the statement after mkdir, so this is not a lock mid-acquisition.
      rmdir "$LOCKDIR" 2>/dev/null
      continue
    fi
    if [ "$waited" -gt 2400 ]; then say "another exchange has held the machine's pipe for too long"; return 1; fi
    sleep 0.25
  done
  echo $$ > "$LOCKDIR/pid" 2>/dev/null
  trap 'rm -f "$LOCKDIR/pid" 2>/dev/null; rmdir "$LOCKDIR" 2>/dev/null' EXIT INT TERM
}
aos_unlock() { rm -f "$LOCKDIR/pid" 2>/dev/null; rmdir "$LOCKDIR" 2>/dev/null; trap - EXIT INT TERM; }

send_line() {
  running || die "the machine is not up — run: aos up"
  mkdir -p "$AOS_RUN"
  aos_lock || return 1
  send_line_locked "$@"
  local rc=$?
  aos_unlock
  return $rc
}

send_line_locked() {
  local line=$1 wait_s=${2:-60} mark before ticks slice log_sz
  mark="__aos_$$_${RANDOM}"
  before=$(wc -c < "$LOG" 2>/dev/null | tr -d ' ')
  before=${before:-0}
  {
    printf '%s\n' "$line"
    printf 'echo %s\n' "$mark"
  } > "$FIFO"
  ticks=0
  while [ "$ticks" -lt "$((wait_s * 4))" ]; do
    log_sz=$(wc -c < "$LOG" 2>/dev/null | tr -d ' ')
    log_sz=${log_sz:-0}
    if [ "$before" -gt "$log_sz" ]; then before=0; fi
    slice=$(tail -c "+$((before + 1))" "$LOG" 2>/dev/null)
    case "$slice" in
      *"$mark"*)
        printf '%s\n' "$slice" | awk -v m="$mark" 'index($0, m) { exit } { print }' | strip_noise | awk '/[^[:space:]]/ { started=1 } started { lines[++n] = $0; if ($0 ~ /[^[:space:]]/) last = n } END { for (i = 1; i <= last; i++) print lines[i] }'
        return 0;;
    esac
    running || die "the machine went away mid-command; see $LOG"
    sleep 0.25
    ticks=$((ticks + 1))
  done
  say "no answer in ${wait_s}s — the machine may still be working; see: aos log"
  return 1
}

write_plist() {
  mkdir -p "$(dirname "$PLIST")" "$AOS_RUN"
  cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$SESSION</string>
  </array>
  <key>EnvironmentVariables</key>
  <dict>
    <key>AOS_CONF</key><string>$CONF</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$AOS_RUN/launchd.out</string>
  <key>StandardErrorPath</key><string>$AOS_RUN/launchd.err</string>
</dict>
</plist>
PL
}

# The systemd --user unit: the same contract as the plist — start at login
# (default.target), restart on exit, the settings file named outright.
write_unit() {
  [ "$1" = /dev/stdout ] || mkdir -p "$(dirname "$1")" "$AOS_RUN"
  cat > "$1" <<UN
[Unit]
Description=AIOS resident machine (root $AOS_ROOT)

[Service]
Type=simple
Environment=AOS_CONF=$CONF
ExecStart=$SESSION
Restart=always
RestartSec=2
StandardOutput=append:$AOS_RUN/systemd.out
StandardError=append:$AOS_RUN/systemd.err

[Install]
WantedBy=default.target
UN
}

case "${1:-help}" in
  up) start;;
  down) stop;;
  restart) stop; start;;
  status)
    printf 'root       %s\n' "$AOS_ROOT"
    printf 'binary     %s\n' "$(find_bin || echo 'none — run: aos build')"
    printf 'mind       %s\n' "${AOS_PROVIDER:-none}${AOS_MODEL:+/$AOS_MODEL}"
    if supervised && [ "$OS" != "Darwin" ]; then
      printf 'supervised %s\n' "yes ($UNIT, $("$SYSTEMCTL" --user is-active "$AOS_UNIT.service" 2>/dev/null || true), restarts on exit and at login)"
    elif supervised; then
      printf 'supervised %s\n' "yes ($LABEL, restarts on exit and at login)"
    else
      printf 'supervised %s\n' "no (run: aos install)"
    fi
    if running; then
      printf 'state      up (pid %s)\n' "$(pid_of)"
      if [ -d "$LOCKDIR" ]; then printf '           (mind busy — a job is running; watch: aos log)\n'; else send_line info 5 2>/dev/null | head -12 | awk '{ print "           " $0 }' || printf '           (mind busy — see: aos log)\n'; fi
    else
      printf 'state      down\n'
    fi;;
  # 60s is right for a question about the machine and far too short for work it
  # does — cloning a repository over its own git transport takes minutes, and a
  # front door that gives up on its own machine reports a failure that did not
  # happen. AOS_SEND_WAIT raises the ceiling for the caller who knows better.
  send) shift; [ $# -gt 0 ] || die 'aos send "<line>"   (AOS_SEND_WAIT=<seconds> for work that takes a while)'; send_line "$*" "${AOS_SEND_WAIT:-60}";;
  ask)
    shift; [ $# -gt 0 ] || die 'aos ask "<question>"'
    # A cognitive cycle reaches an outside mind and can take a while.
    send_line "ask $*" "${AOS_SEND_WAIT:-60}";;
  think)
    # Fire a long cognitive job WITHOUT blocking. Hand the mind the job, then a
    # detached waiter holds the pipe lock until the job's marker lands — so no
    # second cycle overlaps and desyncs the mind — and releases it when done.
    # Returns immediately; watch progress with `aos log`.
    shift; [ $# -gt 0 ] || die 'aos think "<job>"'
    running || die "the machine is not up — run: aos up"
    mkdir -p "$AOS_RUN"
    if ! mkdir "$LOCKDIR" 2>/dev/null; then say "the mind is busy with another job — watch: aos log"; exit 1; fi
    mark="__aos_think_$$_${RANDOM}"
    printf '%s\n' "ask $*" > "$FIFO"
    printf 'echo %s\n' "$mark" > "$FIFO"
    ( while running && ! grep -q "$mark" "$LOG" 2>/dev/null; do sleep 1; done; rm -f "$LOCKDIR/pid" 2>/dev/null; rmdir "$LOCKDIR" 2>/dev/null ) >/dev/null 2>&1 &
    # The waiter owns the lock: with its pid recorded, a send waits for the job
    # instead of reclaiming an "unowned" lock after ten seconds, and the
    # machine's watchdog reclaims it only if the waiter died.
    echo $! > "$LOCKDIR/pid" 2>/dev/null
    disown 2>/dev/null || true
    say "dispatched to the mind — it is working in the background; watch it with: aos log";;
  console)
    running || die "the machine is not up — run: aos up"
    say "attached to $AOS_ROOT — what you type goes to the machine; Ctrl-C to leave"
    tail -n 5 -f "$LOG" & tailpid=$!
    trap 'kill $tailpid 2>/dev/null; exit 0' INT TERM
    while IFS= read -r line; do printf '%s\n' "$line" > "$FIFO"; done
    kill $tailpid 2>/dev/null;;
  log) tail -n "${2:-40}" "$LOG" | strip_noise;;
  mind)
    shift
    case "${1:-}" in
      off|none|"") AOS_PROVIDER=""; AOS_MODEL="";;
      *) AOS_PROVIDER=$1; AOS_MODEL=${2:-};;
    esac
    if [ -z "$AOS_PROVIDERS" ] && [ -f "$repo/providers.cjs" ]; then AOS_PROVIDERS=$repo/providers.cjs; fi
    save
    say "mind: ${AOS_PROVIDER:-none}${AOS_MODEL:+/$AOS_MODEL}"
    if running; then say "restarting so the machine picks it up"; stop; start; fi;;
  root)
    shift; [ $# -gt 0 ] || die "aos root <path>"
    AOS_ROOT=$1; save; say "root: $AOS_ROOT (a new root means a new kernel identity)";;
  build)
    b=$here/build
    cmake -S "$here" -B "$b" ${AOS_GENERATOR:+-G "$AOS_GENERATOR"} >/dev/null || die "configure failed"
    cmake --build "$b" || die "build failed"
    AOS_BIN=$b/aios; save; say "built $AOS_BIN";;
  install)
    dry=0; [ "${2:-}" = "--dry-run" ] && dry=1
    if [ "$dry" -eq 1 ]; then AOS_BIN=$(find_bin) || die "no aios binary yet — run: aos build"; else ensure_bin; fi
    # The interactive PATH is recorded now: launchd and systemd --user hand the
    # machine almost nothing, and the bridge to an outside mind needs node on the path.
    AOS_PATH=$PATH
    if [ -z "$AOS_PROVIDERS" ] && [ -f "$repo/providers.cjs" ]; then AOS_PROVIDERS=$repo/providers.cjs; fi
    if [ "$OS" != "Darwin" ]; then
      command -v "$SYSTEMCTL" >/dev/null 2>&1 || die "install supervises with systemd --user on Linux, and there is no systemctl here; run $SESSION under your own supervisor"
      if [ "$dry" -eq 1 ]; then
        # Nothing is written where systemd looks: the unit goes to stdout, or to
        # AOS_UNIT_OUT, so it can be checked (systemd-analyze --user verify).
        if [ -n "${AOS_UNIT_OUT:-}" ]; then write_unit "$AOS_UNIT_OUT"; else write_unit /dev/stdout; fi
        say "dry run: would write $UNIT, then: $SYSTEMCTL --user daemon-reload && $SYSTEMCTL --user enable --now $AOS_UNIT.service"
        exit 0
      fi
      save
      write_unit "$UNIT"
      p=$(pid_of); [ -n "$p" ] && { kill "$p" 2>/dev/null; sleep 0.5; }  # one runner per root
      "$SYSTEMCTL" --user daemon-reload || die "systemctl --user daemon-reload failed"
      "$SYSTEMCTL" --user enable --now "$AOS_UNIT.service" || die "systemctl --user refused $UNIT"
      for _ in $(seq 1 40); do running && break; sleep 0.25; done
      if running; then
        say "installed and up (pid $(pid_of)) · $UNIT · restarts on exit and at login"
        say "(to start at boot without a login session: loginctl enable-linger $USER)"
      else
        say "installed, but it has not come up; see: $SYSTEMCTL --user status $AOS_UNIT and $AOS_RUN/systemd.err"
      fi
      exit 0
    fi
    [ "$dry" -eq 1 ] && { say "dry run: would write $PLIST and bootstrap gui/$(id -u)/$LABEL"; exit 0; }
    save
    write_plist
    launchctl bootout "gui/$(id -u)/$LABEL" >/dev/null 2>&1
    launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null || launchctl load -w "$PLIST" || die "launchctl refused $PLIST"
    for _ in $(seq 1 40); do running && break; sleep 0.25; done
    if running; then
      say "installed and up (pid $(pid_of)) · restarts on exit and at login"
    else
      say "installed, but it has not come up; see $AOS_RUN/launchd.err"
    fi;;
  uninstall)
    if [ "$OS" != "Darwin" ]; then
      if [ -f "$UNIT" ]; then
        "$SYSTEMCTL" --user disable --now "$AOS_UNIT.service" >/dev/null 2>&1
        rm -f "$UNIT"
        "$SYSTEMCTL" --user daemon-reload >/dev/null 2>&1
      fi
      say "no longer supervised (the root at $AOS_ROOT is untouched)"
      exit 0
    fi
    launchctl bootout "gui/$(id -u)/$LABEL" >/dev/null 2>&1 || launchctl unload "$PLIST" >/dev/null 2>&1
    rm -f "$PLIST"
    say "no longer supervised (the root at $AOS_ROOT is untouched)";;
  config) [ -f "$CONF" ] && cat "$CONF" || say "no settings yet ($CONF)";;
  desktop|gui)
    # The machine's own compositor, and nothing else. There used to be a web
    # mirror here (a node server and a browser page on port 7440); it is gone,
    # along with the telemetry it invented.
    shift
    force_tui=0
    for arg in "$@"; do
      if [ "$arg" = "--tui" ] || [ "$arg" = "-t" ]; then force_tui=1; fi
    done

    running || start
    ensure_bin
    if [ "$force_tui" -eq 1 ]; then
      say "launching AIOS TUI Desktop..."
      "$AOS_BIN" --root "$AOS_ROOT" desktop --tui
    else
      say "launching AIOS SDL2 Compositor..."
      "$AOS_BIN" --root "$AOS_ROOT" desktop
    fi;;
  # The usage is the comment block at the top, up to the first line that is not a comment.
  help|-h|--help) awk 'NR == 1 { next } !/^#/ { exit } { sub(/^# ?/, ""); print }' "$self";;
  *) die "unknown command '$1' — try: aos help";;
esac

