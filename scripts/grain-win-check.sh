#!/usr/bin/env bash
# grain on Windows, checked from Linux: grain.exe cross-compiled with MinGW (no SDL, no library: the Win32
# API), the window tests run under Wine against the same expected output as Linux, then a real window on a
# virtual X server driven with real input (clicks, typing in two layouts, wheel, resize, Alt+F4).
#
#   scripts/grain-win-check.sh [work-dir]      needs docker; the first run builds an image (~1.6 GB)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-${TMPDIR:-/tmp}/grain-win-check}
DOCKER=${DOCKER:-docker}
IMAGE=grain-win-builder
mkdir -p "$WORK/out" "$WORK/live"

if ! $DOCKER image inspect $IMAGE >/dev/null 2>&1; then
  cat > "$WORK/Dockerfile" <<'EOF'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
    g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix mingw-w64-x86-64-dev cmake make \
    wine64 wine xvfb xdotool x11-apps python3 ca-certificates && rm -rf /var/lib/apt/lists/*
EOF
  $DOCKER build -t $IMAGE "$WORK"
fi

cat > "$WORK/tc.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
EOF

cat > "$WORK/live/input.gr" <<'EOF'
let clicks = 0
let typed = ""
let pane = create_pane({"title": "grain on Windows", "width": 360, "height": 200,
  "on_draw": fn (cv, p) {
    canvas_clear(cv, "#14202e")
    canvas_fill_rect(cv, 20, 20, 30 + clicks * 30, 24, "#3fb950")
    canvas_draw_text(cv, "typed " + typed, 20, 100, "#e6edf3", "", 18)
  },
  "on_event": fn (p, e) {
    if e.type == "mouse_move" { return nil }
    let line = e.type
    if e.type == "click" or e.type == "mouse_down" or e.type == "mouse_up" { line = line + " " + e.button + " at " + str(e.x) + "," + str(e.y) }
    if e.type == "key" or e.type == "key_up" { line = line + " " + e.key + (e.shift ? " +shift" : "") }
    if e.type == "text" { line = line + " " + repr(e.text); typed = typed + e.text }
    if e.type == "wheel" { line = line + " dy=" + str(e.dy) }
    if e.type == "resize" { line = line + " " + str(e.width) + "x" + str(e.height) }
    print(line)
  },
  "on_click": fn (p, e) { clicks = clicks + 1 }})
print("run_panes returned:", run_panes({"frames": 900}))
print("clicks:", clicks)
EOF

cat > "$WORK/run.sh" <<'EOF'
set -e
export LANG=C.UTF-8 LC_ALL=C.UTF-8 WINEDEBUG=-all WINEPREFIX=/tmp/wp
cmake -S /src/grain -B /w/out -DCMAKE_TOOLCHAIN_FILE=/w/tc.cmake -DCMAKE_BUILD_TYPE=Release > /w/configure.log
grep -q "Win32 API, no library" /w/configure.log || { echo "FAIL: not configured for the Win32 display"; exit 1; }
cmake --build /w/out --target grain -j"$(nproc)" > /w/build.log 2>&1 || { tail -30 /w/build.log; exit 1; }
if x86_64-w64-mingw32-objdump -p /w/out/grain.exe | grep -i "DLL Name" | grep -viE "kernel32|user32|gdi32|ws2_32|bcrypt|crypt32|iphlpapi|advapi32|shell32|ole32|msvcrt|api-ms-win|ntdll|ucrtbase|secur32|dbghelp|psapi|shlwapi"; then
  echo "FAIL: grain.exe needs a DLL that is not part of Windows"; exit 1
fi
wineboot -i > /dev/null 2>&1 || true
fail=0
cd /src/grain/tests/window
for t in panes face; do
  set +e; GRAIN_VIDEO=dummy timeout 120 wine /w/out/grain.exe run $t.gr 2>&1 | tr -d '\r' > /w/$t.out; st=${PIPESTATUS[0]}; set -e
  if diff -q /w/$t.out ../expected/window.$t.out > /dev/null && [ "$st" = "$(cat ../expected/window.$t.out.status)" ]; then echo "ok   window.$t (headless, same output as Linux)"
  else echo "FAIL window.$t"; diff /w/$t.out ../expected/window.$t.out | head; fail=1; fi
done

Xvfb :7 -screen 0 1024x768x24 > /dev/null 2>&1 &
export DISPLAY=:7
sleep 1
cd /w/live
wine /w/out/grain.exe run input.gr 2>&1 | tr -d '\r' > /w/live/events.out & GP=$!
W=
for i in $(seq 1 60); do W=$(xdotool search --name "grain on Windows" 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 0.5; done
[ -n "$W" ] || { echo "FAIL: no window appeared"; exit 1; }
sleep 1
xdotool mousemove --window "$W" 50 40 click 1; sleep 0.2
xdotool mousemove --window "$W" 60 40 click 1; sleep 0.2
xdotool mousemove --window "$W" 70 40 click 3; sleep 0.2
xdotool type "ж"; sleep 0.2   # xdotool loses the first key it remaps into the keymap; this one is not checked
xdotool type --delay 80 "Hi Привет"; sleep 0.2
xdotool key Return; sleep 0.2
xdotool click 5; sleep 0.3
xdotool windowsize "$W" 500 260; sleep 0.8
xdotool key alt+F4
for i in $(seq 1 40); do kill -0 $GP 2>/dev/null || break; sleep 0.25; done
check() { if grep -qF -- "$1" /w/live/events.out; then echo "ok   $2"; else echo "FAIL $2 (no \"$1\")"; fail=1; fi; }
check "click left at 50,40" "a left click reaches the pane at its own coordinates"
check "click right at 70,40" "a right click"
check "key h +shift" "a shifted key, named as on Linux"
check 'text "П"' "typed Cyrillic text"
check "key return" "Return as a key, not text"
check "wheel dy=-1.0" "the wheel"
check "resize 500x260" "a resize from the window system"
check "key f4" "Alt+F4"
check "run_panes returned: true" "closing the window ends run_panes()"
check "clicks: 3" "on_click for each click"
exit $fail
EOF

$DOCKER run --rm -v "$ROOT":/src:ro -v "$WORK":/w $IMAGE bash /w/run.sh
