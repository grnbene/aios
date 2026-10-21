#!/usr/bin/env bash
# aos-vm — run the bare-metal AIOS in a VM and drive it from the shell.
#
#   aos-vm up                         boot (tmux session "aios"; a QEMU window if a desktop is present); attach the saved mind
#   aos-vm down                       power the machine off
#   aos-vm status                     what is running, which disk, which mind
#   aos-vm console                    attach to the serial console (tmux; detach with Ctrl-b d)
#   aos-vm send "<console line>"      type one line, print what the machine said
#   aos-vm ask "<question>"           ask the resident mind, wait for its answer
#   aos-vm gui <off|cocoa|gtk|vnc|auto> configure display isolation (off = headless background, cocoa = macOS window, vnc = remote view, gtk = desktop window)
#   aos-vm mind <provider> <model>    switch the mind: e.g. anthropic claude-sonnet-5 | antigravity-cli gemini-3.6-flash-medium | claude-cli claude-sonnet-5
#   aos-vm mind off                   detach the mind
#   aos-vm screen [out.png]           screenshot the machine's display
#   aos-vm build                      git pull, rebuild the kernel, image and disk
#   aos-vm upgrade                    new boot partition from the build, KEEPING the machine's persistent root; restarts if running
#   aos-vm newdisk                    fresh, empty machine (a new kernel identity)
#   aos-vm disk <path>                which disk image to boot (saved)
#   aos-vm config                     show the saved settings (~/.config/aos-vm.conf)
#
# Settings persist in ~/.config/aos-vm.conf: AOS_DISK AOS_PROVIDER AOS_MODEL AOS_SMP AOS_MEM AOS_GUI.
set -uo pipefail
self=$0; [ -L "$self" ] && self=$(readlink -f "$self"); here=$(cd "$(dirname "$self")/.." && pwd)
CONF=${AOS_CONF:-$HOME/.config/aos-vm.conf}
SESSION=aios; WIRE=aios-mind
MIND_SOCK=/tmp/aios-mind.sock; MON_SOCK=/tmp/aios-monitor.sock
AOS_DISK=${AOS_DISK:-$HOME/aos/disk.img}; AOS_PROVIDER=${AOS_PROVIDER:-}; AOS_MODEL=${AOS_MODEL:-}; AOS_SMP=${AOS_SMP:-4}; AOS_MEM=${AOS_MEM:-1024}; AOS_GUI=${AOS_GUI:-gtk}
[ -f "$CONF" ] && . "$CONF"
save() { mkdir -p "$(dirname "$CONF")"; printf 'AOS_DISK=%q\nAOS_PROVIDER=%q\nAOS_MODEL=%q\nAOS_SMP=%q\nAOS_MEM=%q\nAOS_GUI=%q\n' "$AOS_DISK" "$AOS_PROVIDER" "$AOS_MODEL" "$AOS_SMP" "$AOS_MEM" "$AOS_GUI" > "$CONF"; }
say() { printf '%s\n' "$*" >&2; }
die() { say "aos-vm: $*"; exit 1; }
running() { tmux has-session -t $SESSION 2>/dev/null && (pidof qemu-system-x86_64 >/dev/null 2>&1 || pgrep -x qemu-system-x86_64 >/dev/null 2>&1); }
strip() { sed 's/\x1b\[[0-9;=?]*[a-zA-Z]//g'; }
pane() { tmux capture-pane -p -t $SESSION -S -${1:-2000} 2>/dev/null | strip; }
firmware() {
  for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/x64/OVMF.4m.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/qemu/OVMF.fd; do [ -f "$f" ] && { echo "-bios $f"; return; }; done
  for f in /usr/share/OVMF/OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE_4M.fd /opt/homebrew/share/qemu/edk2-x86_64-code.fd /usr/local/share/qemu/edk2-x86_64-code.fd; do [ -f "$f" ] && { echo "-drive if=pflash,format=raw,readonly=on,file=$f"; return; }; done
  [ -d /usr/lib/grub/i386-pc ] || [ "$(uname)" = "Darwin" ] || die "no OVMF firmware found and no BIOS GRUB either"
  echo ""   # BIOS
}
gui_args() {
  local d=${DISPLAY:-:0}
  case "$AOS_GUI" in
    0|no|off|none|headless) echo "DISP=-vga std -display none"; return;;
    vnc*) echo "DISP=-vga std -display vnc=127.0.0.1:0"; return;;
    cocoa) echo "DISP=-vga std -display cocoa,full-grab=off,zoom-to-fit=on,left-command-key=off"; return;;
    gtk|window|on|auto)
      if [ "$(uname)" = "Darwin" ]; then
        echo "DISP=-vga std -display cocoa,full-grab=off,zoom-to-fit=on,left-command-key=off"
      else
        echo "ENV=DISPLAY=$d XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority} DISP=-vga std -display gtk"
      fi
      return;;
    *) echo "DISP=$AOS_GUI"; return;;
  esac
}
wait_prompt() { local t=${1:-180}; for _ in $(seq 1 $((t*2))); do pane 60 | grep -q "^aios> *$" && return 0; running || return 1; sleep 0.5; done; return 1; }
# Type one line and return exactly what the machine printed for it (a marker line closes the exchange).
run_line() {
  local line=$1 mark="__aos_vm_done_$$_$RANDOM"
  tmux send-keys -t $SESSION -l -- "$line"; tmux send-keys -t $SESSION Enter
  tmux send-keys -t $SESSION -l -- "echo $mark"; tmux send-keys -t $SESSION Enter
  local t=${2:-900}
  for _ in $(seq 1 $((t*2))); do
    local out; out=$(pane 5000)
    if printf '%s\n' "$out" | grep -q "^$mark\$"; then
      printf '%s\n' "$out" | awk -v l="aios> $line" -v m="$mark" 'index($0,l)==1{f=1;next} f && $0=="aios> echo "m{exit} f' 
      return 0
    fi
    running || { say "the machine is gone"; return 1; }
    sleep 0.5
  done
  say "timed out waiting for the machine"; return 1
}
wire_up() {
  [ -n "$AOS_PROVIDER" ] || return 0
  tmux kill-session -t $WIRE 2>/dev/null
  tmux new-session -d -x 200 -y 50 -s $WIRE "cd '$here' && AIOS_MODEL_PROVIDER='$AOS_PROVIDER' AIOS_MODEL='$AOS_MODEL' python3 metal/mind-wire.py $MIND_SOCK; echo '[wire ended]'; sleep 86400"
  sleep 2
  tmux capture-pane -p -t $WIRE | grep -m1 "^wire:" >&2 || true
}
bind_mind() {
  local out; out=$(run_line "mind bind mind wire" 30) || return 1
  printf '%s\n' "$out"
  printf '%s\n' "$out" | grep -q "^bound" || { say "the mind did not bind — look at: tmux attach -t $WIRE"; return 1; }
}

cmd=${1:-help}; shift || true
case "$cmd" in
  up)
    running && { say "already up (tmux attach -t $SESSION)"; exit 0; }
    [ -f "$AOS_DISK" ] || { say "no disk at $AOS_DISK — making a fresh one from the build"; "$0" newdisk || exit 1; }
    tmux kill-session -t $SESSION 2>/dev/null; rm -f $MIND_SOCK $MON_SOCK
    fw=$(firmware); g=$(gui_args); env=""; disp=""
    case "$g" in
      ENV=*)
        env=$(echo "$g" | sed -n 's/ENV=\(.*\) DISP=.*/\1/p')
        disp=$(echo "$g" | sed -n 's/.*DISP=\(.*\)/\1/p')
        ;;
      DISP=*)
        disp=$(echo "$g" | sed 's/DISP=//')
        ;;
      *)
        disp="$g"
        ;;
    esac
    accel="-accel tcg,thread=multi -cpu max"
    [ -w /dev/kvm ] && accel="-enable-kvm -cpu host"
    tmux new-session -d -x 220 -y 50 -s $SESSION "$env qemu-system-x86_64 -name AOS -smp $AOS_SMP -m $AOS_MEM $fw -drive file=$AOS_DISK,if=virtio,format=raw -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -boot c $accel $disp -serial stdio -serial unix:$MIND_SOCK,server,nowait -monitor unix:$MON_SOCK,server,nowait; echo '[the machine powered off]'; sleep 86400"
    say "booting $AOS_DISK (${AOS_SMP} cpus, ${AOS_MEM} MiB, gui: ${AOS_GUI})$( [ -n "$env" ] && echo ', window on desktop')…"
    wait_prompt || die "no console prompt — see: tmux attach -t $SESSION"
    pane 40 | grep -E "^Kernel:|^AIOS" | tail -1
    if [ -n "$AOS_PROVIDER" ]; then wire_up; bind_mind || true; else say "no mind configured: aos-vm mind <provider> <model>"; fi
    say "console: aos-vm console   ·   ask: aos-vm ask \"…\""
    [ "$AOS_GUI" != "off" ] && say "macos window controls: drag edges to shrink/resize · Ctrl+Option+G releases mouse grab · Cmd+Tab/Cmd+M/Cmd+H work normally"
    ;;
  down)
    if running; then
      tmux send-keys -t $SESSION -l -- "host poweroff"; tmux send-keys -t $SESSION Enter
      if [ -S "$MON_SOCK" ]; then echo "quit" | nc -U "$MON_SOCK" 2>/dev/null || true; fi
      for _ in $(seq 1 20); do running || break; sleep 0.2; done
    fi
    tmux kill-session -t $SESSION 2>/dev/null; tmux kill-session -t $WIRE 2>/dev/null
    pkill -f "qemu-system-x86_64 -name AOS" 2>/dev/null; say "down";;
  status)
    if running; then say "UP   disk $AOS_DISK (gui: $AOS_GUI) · $(pane 200 | grep -E '^Kernel:|^AIOS' | tail -1)"; else say "DOWN disk $AOS_DISK (gui: $AOS_GUI)"; fi
    if tmux has-session -t $WIRE 2>/dev/null; then say "mind $AOS_PROVIDER/$AOS_MODEL · $(tmux capture-pane -p -t $WIRE | grep -vE '^$' | tail -1)"; else say "mind none attached (saved: ${AOS_PROVIDER:-none}/${AOS_MODEL:-})"; fi
    running && run_line "entity list" 20 | grep -E "^[0-9a-f]{32}"; true;;
  console) running || die "not up"; exec tmux attach -t $SESSION;;
  send) running || die "not up"; [ $# -ge 1 ] || die "send \"<console line>\""; run_line "$*";;
  ask) running || die "not up"; [ $# -ge 1 ] || die "ask \"<question>\""; run_line "ask $*" 1800;;
  gui)
    [ $# -ge 1 ] || die "gui <off|cocoa|gtk|vnc|auto>   (e.g. aos-vm gui cocoa for desktop window, or off for headless)"
    case "$1" in
      off|none|0|headless) AOS_GUI="off";;
      cocoa) AOS_GUI="cocoa";;
      gtk|window|on) AOS_GUI="gtk";;
      vnc) AOS_GUI="vnc";;
      auto) AOS_GUI="auto";;
      *) die "unknown gui setting '$1' (choose off, cocoa, gtk, vnc, or auto)";;
    esac
    save
    say "saved display mode: $AOS_GUI"
    if running; then say "note: restart VM to apply change (aos-vm down && aos-vm up)"; fi;;
  mind)
    [ $# -ge 1 ] || die "mind <provider> <model> | mind off   (e.g. anthropic claude-sonnet-5 · antigravity-cli gemini-3.6-flash-medium · claude-cli claude-sonnet-5)"
    if [ "$1" = off ]; then AOS_PROVIDER=""; AOS_MODEL=""; save; running && run_line "mind unbind mind" 20; tmux kill-session -t $WIRE 2>/dev/null; say "mind detached"; exit 0; fi
    [ $# -ge 2 ] || die "mind <provider> <model>"
    AOS_PROVIDER=$1; AOS_MODEL=$2; save
    if running; then run_line "mind unbind mind" 20 >/dev/null; wire_up; bind_mind; else say "saved: $AOS_PROVIDER/$AOS_MODEL — it attaches on 'aos-vm up'"; fi;;
  screen)
    running || die "not up"; out=${1:-$HOME/aos-screen.png}
    python3 - "$MON_SOCK" "$out" <<'PY'
import socket, sys, time, os, struct, zlib
mon, out = sys.argv[1], sys.argv[2]; ppm = out + ".ppm"
s = socket.socket(socket.AF_UNIX); s.connect(mon); s.settimeout(3)
try: s.recv(4096)
except Exception: pass
s.sendall(("screendump %s\n" % ppm).encode()); time.sleep(1.5)
for _ in range(20):
    if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
    time.sleep(0.3)
d = open(ppm, "rb").read(); p = d.split(maxsplit=4); w, h, pix = int(p[1]), int(p[2]), p[4]
def chunk(t, b): c = t + b; return struct.pack(">I", len(b)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
raw = b"".join(b"\x00" + pix[y*w*3:(y+1)*w*3] for y in range(h))
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
os.remove(ppm); print(out)
PY
    ;;
  build)
    root=$(cd "$here/.." && git rev-parse --show-toplevel 2>/dev/null) || root="$here/.."
    (cd "$root" && git pull --ff-only origin main) || die "git pull failed in $root"
    (cd "$here" && cmake --build build -j"$(nproc)" 2>&1 | grep -E "error|warning: unused"; metal/build.sh 2>&1 | tail -1 && metal/mkdisk.sh 2>&1 | tail -1);;
  upgrade)
    new="$here/build/metal/aios-metal-disk.img"; [ -f "$new" ] || die "no built disk image — aos-vm build first"
    [ -f "$AOS_DISK" ] || die "no disk at $AOS_DISK"
    was=0; running && { was=1; "$0" down; }
    python3 - "$AOS_DISK" "$new" <<'PY'
import struct, shutil, sys, os
old, new = sys.argv[1], sys.argv[2]
def parts(p):
    m = open(p, "rb").read(512)
    return [(m[446+16*i+4], struct.unpack_from("<I", m, 446+16*i+8)[0], struct.unpack_from("<I", m, 446+16*i+12)[0]) for i in range(4)]
po, pn = parts(old), parts(new)
if not (po[1][0] == 0x7F and pn[1] == po[1]): sys.exit("the AOS partition geometry differs between the old disk and the new build; refusing to splice")
start, count = pn[1][1] * 512, pn[1][2] * 512
tmp = old + ".new"; shutil.copy(new, tmp)
with open(old, "rb") as src, open(tmp, "r+b") as dst:
    src.seek(start); dst.seek(start); dst.write(src.read(count))
os.replace(tmp, old); print("upgraded the boot partition of", old, "— the persistent root is untouched")
PY
    [ $was = 1 ] && "$0" up || true;;
  newdisk)
    src="$here/build/metal/aios-metal-disk.img"; [ -f "$src" ] || { (cd "$here" && metal/mkdisk.sh | tail -1) || exit 1; }
    mkdir -p "$(dirname "$AOS_DISK")"; [ -f "$AOS_DISK" ] && { cp "$AOS_DISK" "$AOS_DISK.bak"; say "previous disk kept at $AOS_DISK.bak"; }
    cp "$src" "$AOS_DISK"; save; say "fresh machine at $AOS_DISK";;
  disk) [ $# -ge 1 ] || die "disk <path>"; AOS_DISK=$(readlink -f "$1"); save; say "disk: $AOS_DISK";;
  config) cat "$CONF" 2>/dev/null || say "(no saved settings yet)";;
  help|-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//';;
  *) die "unknown command '$cmd' (aos-vm help)";;
esac
