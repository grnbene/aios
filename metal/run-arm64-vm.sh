#!/usr/bin/env bash
# Run the AArch64 kernel as a long-lived VM on this machine.
#   metal/run-arm64-vm.sh            start it (detached; survives the shell)
#   metal/run-arm64-vm.sh status     is it up, and on what
#   metal/run-arm64-vm.sh console    attach to the serial console
#   metal/run-arm64-vm.sh shot FILE  screendump the display to a PPM
#   metal/run-arm64-vm.sh stop       stop it
#
# Detaching matters. Started as a child of an interactive shell (or of an agent
# process) the VM dies when that parent goes away, which is exactly what
# happened the first time this was launched. nohup + setsid + a pidfile is what
# makes it outlive whoever started it.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/metal-arm64
IMAGE=$out/aios-metal-arm64-virt.bin      # linked for virt's DRAM at 0x40000000, booted as an Image so it gets the DTB
DISK=$out/aios-metal-disk-arm64.img

STATE=${AOS_VM_STATE:-/tmp/aos-vm}           # prefix for the pidfile, logs and monitor socket
PIDFILE=$STATE.pid
LOG=$STATE.log
SERIAL_LOG=$STATE-serial.log
MONITOR=$STATE-monitor.sock
SERIAL_PORT=${AOS_SERIAL_PORT:-4444}
VNC_DISPLAY=${AOS_VNC_DISPLAY:-1}          # :1 => TCP 5901

# Apple's hypervisor where it exists, emulation everywhere else. HVF does NOT
# implement GICv2 ("HVF does not support GICv2 emulation"), so the accelerated
# path has to ask for GICv3; the kernel picks whichever the device tree says.
if qemu-system-aarch64 -accel help 2>/dev/null | grep -q '^hvf$'; then
  ACCEL=(-M "virt,gic-version=3,accel=hvf" -cpu host)
  MODE="hvf (hardware virtualisation)"
else
  ACCEL=(-M virt -cpu cortex-a72)
  MODE="tcg (emulation)"
fi

running() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

case "${1:-start}" in
  start)
    if running; then echo "already running, pid $(cat "$PIDFILE")"; exit 0; fi
    [ -f "$IMAGE" ] || { echo "no kernel image; run metal/build-arm64.sh first" >&2; exit 1; }
    # bs in bytes: BSD dd takes "1m", GNU dd only "1M"; both take a number.
    [ -f "$DISK" ] || dd if=/dev/zero of="$DISK" bs=1048576 count=64 2>/dev/null
    rm -f "$MONITOR"

    # setsid is not on macOS; nohup plus a background job in a subshell that
    # exits immediately is what actually detaches here.
    nohup qemu-system-aarch64 \
      -name "AIOS - AArch64 bare metal" \
      "${ACCEL[@]}" -m 2G -smp 4 -no-reboot \
      -kernel "$IMAGE" \
      -drive file="$DISK",if=none,id=drv0,format=raw \
      -device virtio-blk-device,drive=drv0 \
      -device virtio-gpu-device \
      -display none \
      -vnc 127.0.0.1:"$VNC_DISPLAY" \
      -chardev socket,id=ser0,host=127.0.0.1,port="$SERIAL_PORT",server=on,wait=off,logfile="$SERIAL_LOG" \
      -serial chardev:ser0 \
      -monitor unix:"$MONITOR",server,nowait \
      -pidfile "$PIDFILE" \
      > "$LOG" 2>&1 < /dev/null &
    disown || true

    for _ in $(seq 1 50); do running && break; sleep 0.2; done
    running || { echo "failed to start; see $LOG" >&2; cat "$LOG" >&2; exit 1; }
    echo "started pid $(cat "$PIDFILE") on $MODE"
    echo "  console   nc localhost $SERIAL_PORT       (serial log: $SERIAL_LOG)"
    echo "  screen    open vnc://localhost:$((5900 + VNC_DISPLAY))"
    echo "  monitor   $MONITOR"
    ;;

  status)
    if running; then
      echo "running, pid $(cat "$PIDFILE")"
      lsof -nP -iTCP:"$SERIAL_PORT" -sTCP:LISTEN 2>/dev/null | tail -1
      lsof -nP -iTCP:$((5900 + VNC_DISPLAY)) -sTCP:LISTEN 2>/dev/null | tail -1
    else
      echo "not running"; exit 1
    fi
    ;;

  console)
    running || { echo "not running" >&2; exit 1; }
    exec nc localhost "$SERIAL_PORT"
    ;;

  shot)
    running || { echo "not running" >&2; exit 1; }
    dest=${2:-/tmp/aos-vm-screen.ppm}
    # The monitor never closes the connection, and Linux's nc waits for that
    # forever (BSD nc returns): send, give it time to write, then hang up.
    rm -f "$dest"
    { printf 'screendump %s\n' "$dest" | nc -U "$MONITOR" >/dev/null 2>&1 & } ; ncpid=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$dest" ] && break; sleep 0.5; done
    sleep 0.5; kill "$ncpid" 2>/dev/null || true
    [ -f "$dest" ] && echo "wrote $dest" || { echo "screendump produced nothing" >&2; exit 1; }
    ;;

  stop)
    running || { echo "not running"; exit 0; }
    kill "$(cat "$PIDFILE")" 2>/dev/null || true
    sleep 1
    rm -f "$PIDFILE" "$MONITOR"
    echo "stopped"
    ;;

  *)
    echo "usage: $0 {start|status|console|shot [file]|stop}" >&2; exit 2
    ;;
esac
