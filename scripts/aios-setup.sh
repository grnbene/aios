#!/usr/bin/env bash
# aios-setup.sh: take a fresh checkout to a working machine, the way
# mysql_install_db + mysql_secure_installation take a MySQL install to a server.
# Safe to re-run: it never overwrites a key, a profile or a root it already made.
#
#   scripts/aios-setup.sh                      build if needed, make ~/aos/root, its key, check it boots
#   scripts/aios-setup.sh --dry-run            print every step, change nothing
#   scripts/aios-setup.sh --key-from FILE      use a shared capability key (machines that trust each other)
#   scripts/aios-setup.sh --upstream db.example:3306 --upstream-user repl --upstream-db shop
#                                              add a MariaDB/MySQL connection profile (asks for the password)
#   scripts/aios-setup.sh --console            also supervise the resident console (scripts/aos.sh install)
#   scripts/aios-setup.sh --asql-server        also package ASQL and hand it to packaging/install-asql.sh (root)
#
# Everything it makes lives in the root (default ~/aos/root) and in the build
# directory; nothing outside them is touched unless --console or --asql-server
# is given.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd -P)
ROOT=${AOS_ROOT:-$HOME/aos/root}
BUILD=$HERE/build
REBUILD=0
DRY=0
KEY_FROM=""
UPSTREAM=""
UP_USER=""
UP_DB=""
UP_PASS_FILE=""
CONSOLE=0
ASQL_SERVER=0
YES=0

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --root)             ROOT=${2:?--root needs a directory}; shift 2 ;;
    --build-dir)        BUILD=${2:?--build-dir needs a directory}; shift 2 ;;
    --rebuild)          REBUILD=1; shift ;;
    --dry-run)          DRY=1; shift ;;
    --key-from)         KEY_FROM=${2:?--key-from needs a file}; shift 2 ;;
    --upstream)         UPSTREAM=${2:?--upstream needs host[:port]}; shift 2 ;;
    --upstream-user)    UP_USER=${2:?}; shift 2 ;;
    --upstream-db)      UP_DB=${2:?}; shift 2 ;;
    --upstream-pass-file) UP_PASS_FILE=${2:?}; shift 2 ;;
    --console)          CONSOLE=1; shift ;;
    --asql-server)      ASQL_SERVER=1; shift ;;
    -y|--yes)           YES=1; shift ;;
    -h|--help)          usage 0 ;;
    *) echo "unknown option: $1" >&2; usage 2 ;;
  esac
done

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m %s\n' "$*"; }
warn() { printf '    \033[33mwarn\033[0m %s\n' "$*"; }
die()  { printf '\033[31mfail\033[0m %s\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY" = 1 ]; then printf '    would: %s\n' "$*"; else "$@"; fi; }
ask()  { [ "$YES" = 1 ] && return 0; read -r -p "    $1 [y/N] " a; [ "$a" = y ] || [ "$a" = Y ]; }

# ---------------------------------------------------------------- 1. tools
say "checking the tools a build needs"
command -v cmake >/dev/null || die "cmake is missing (3.20 or newer)"
cm=$(cmake --version | awk 'NR==1{print $3}')
[ "$(printf '%s\n3.20\n' "$cm" | sort -V | head -1)" = 3.20 ] || die "cmake $cm is too old: 3.20 or newer"
ok "cmake $cm"
CXX_BIN=${CXX:-$(command -v g++ || command -v clang++ || true)}
[ -n "$CXX_BIN" ] || die "no C++ compiler (g++ 10+ or clang++ 13+)"
ok "$("$CXX_BIN" --version | head -1)"
# src/compute/opencl_device.cpp includes CL/cl.h unconditionally; the library itself is dlopen'd at run time.
if echo '#include <CL/cl.h>' | "$CXX_BIN" -x c++ -fsyntax-only - 2>/dev/null; then ok "OpenCL headers"
else warn "no CL/cl.h: install opencl-headers (Debian/Ubuntu) or opencl-headers (Arch/Fedora) before building"; fi
for opt in mysql:'the mysql client, to talk to ASQL' openssl:'openssl, for TLS certificates' qemu-system-x86_64:'QEMU, for the bare-metal boot tests'; do
  command -v "${opt%%:*}" >/dev/null && ok "${opt%%:*}" || warn "optional: ${opt#*:} (${opt%%:*})"
done

# ---------------------------------------------------------------- 2. build
BIN=$BUILD/aios
if [ "$REBUILD" = 1 ] || [ ! -x "$BIN" ]; then
  say "building into $BUILD"
  jobs=$(( $(nproc 2>/dev/null || sysctl -n hw.ncpu) - 1 )); [ "$jobs" -ge 1 ] || jobs=1
  run cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
  run cmake --build "$BUILD" -j"$jobs"
else
  ok "already built: $BIN (--rebuild to build again)"
fi
[ "$DRY" = 1 ] || [ -x "$BIN" ] || die "the build did not produce $BIN"

# ---------------------------------------------------------------- 3. root
say "the machine's root: $ROOT"
if [ -d "$ROOT" ]; then ok "exists; keeping it"; else run mkdir -p "$ROOT"; fi
run mkdir -p "$ROOT/sql_db"
run chmod 700 "$ROOT"

# ---------------------------------------------------------------- 4. key
# Every capability token this machine mints is signed with this key. There is
# no default key in the source. One machine, one key, unless machines must
# accept each other's tokens: then they share one (--key-from).
say "capability key"
KEY=$ROOT/capability.key
if [ -n "$KEY_FROM" ]; then
  [ -r "$KEY_FROM" ] || die "cannot read $KEY_FROM"
  if [ -f "$KEY" ] && ! cmp -s "$KEY" "$KEY_FROM"; then
    ask "replace this machine's existing key with $KEY_FROM? tokens it already minted stop validating" \
      || die "kept the existing key"
  fi
  run install -m 600 "$KEY_FROM" "$KEY"
  [ "$DRY" = 1 ] || ok "shared key from $KEY_FROM"
elif [ -f "$KEY" ]; then
  ok "exists; keeping it"
else
  if [ "$DRY" = 1 ]; then echo "    would: write 32 random bytes as hex to $KEY (0600)"
  else (umask 077; od -An -N32 -tx1 /dev/urandom | tr -d ' \n' > "$KEY"; echo >> "$KEY")
    ok "generated $KEY (0600). Keep it private; copy it only to machines that must trust this one"; fi
fi

# ---------------------------------------------------------------- 5. upstream profile
if [ -n "$UPSTREAM" ]; then
  say "connection profile for $UPSTREAM"
  [ -n "$UP_USER" ] || die "--upstream needs --upstream-user"
  host=${UPSTREAM%%:*}; port=${UPSTREAM##*:}; [ "$port" = "$UPSTREAM" ] && port=3306
  id=$(printf '%s' "${host}_${port}" | tr -c 'A-Za-z0-9_\n' '_')
  INI=$ROOT/sql_db/connections.ini
  if [ -f "$INI" ] && grep -qx "id=$id" "$INI"; then
    ok "profile $id already in $INI; keeping it"
  else
    if [ -n "$UP_PASS_FILE" ]; then pass=$(head -1 "$UP_PASS_FILE")
    elif [ "$DRY" = 1 ]; then pass=""
    else read -r -s -p "    password for $UP_USER@$host (not echoed): " pass; echo; fi
    if [ "$DRY" = 1 ]; then echo "    would: append profile $id ($UP_USER@$host:$port/${UP_DB:-}) to $INI (0600)"
    else
      (umask 077; printf '[profile]\nid=%s\nname=%s\nhost=%s\nport=%s\nsocket_path=\nuser=%s\npassword=%s\ndatabase=%s\nuse_ssl=0\nssh_host=\nssh_port=0\nssh_user=\nis_local_embedded=0\nthings_write=0\n' \
        "$id" "$host:$port" "$host" "$port" "$UP_USER" "$pass" "$UP_DB" >> "$INI")
      chmod 600 "$INI"
      ok "added profile $id to $INI (0600)"
    fi
    if command -v mysql >/dev/null && [ "$DRY" = 0 ]; then
      MYSQL_PWD=$pass mysql -h "$host" -P "$port" -u "$UP_USER" -N -e "SELECT @@version, @@binlog_format" 2>/dev/null \
        | { read -r v f && ok "reached it: server $v, binlog_format $f$([ "$f" = ROW ] || echo ' (replication needs ROW)')"; } \
        || warn "could not log in to $host:$port as $UP_USER from here"
    fi
  fi
fi

# ---------------------------------------------------------------- 6. first boot
say "first boot of the root"
if [ "$DRY" = 1 ]; then echo "    would: $BIN --root $ROOT exec info"
else
  out=$(AIOS_NO_ACCEL=1 "$BIN" --root "$ROOT" exec info 2>&1) || { echo "$out"; die "the machine did not boot on $ROOT"; }
  echo "$out" | grep -E "Kernel ID|Console ID|Root" | sed 's/^/    /'
  [ -f "$ROOT/kernel.identity" ] && ok "kernel identity $ROOT/kernel.identity (stays with this root across restarts)"
fi

# ---------------------------------------------------------------- 7. optional services
if [ "$CONSOLE" = 1 ]; then
  say "resident console (scripts/aos.sh)"
  if ask "supervise the console for $(id -un) now and at login?"; then
    run env AOS_ROOT="$ROOT" AOS_BIN="$BIN" "$HERE/scripts/aos.sh" install
  else warn "skipped: run AOS_ROOT=$ROOT scripts/aos.sh install later"; fi
fi
if [ "$ASQL_SERVER" = 1 ]; then
  say "ASQL as a system service (packaging/install-asql.sh, needs root)"
  DIST=$BUILD/dist
  run "$HERE/packaging/pack.sh" "$BUILD" "$DIST"
  pkg=$(ls -d "$DIST"/aios-asql-*/ 2>/dev/null | grep -v '\.tar' | tail -1 || true)
  if [ -z "$pkg" ]; then
    tarball=$(ls -t "$DIST"/aios-asql-*.tar.gz 2>/dev/null | head -1 || true)
    [ -n "$tarball" ] && run tar -C "$DIST" -xzf "$tarball" && pkg=$(ls -d "$DIST"/aios-asql-*/ | tail -1)
  fi
  if [ "$DRY" = 1 ] || [ -z "$pkg" ]; then echo "    would: sudo ${pkg:-<package>}/install-asql.sh --dry-run, then without --dry-run"
  else
    sudo "$pkg/install-asql.sh" --dry-run
    ask "install it for real?" && sudo "$pkg/install-asql.sh" || warn "skipped: sudo $pkg/install-asql.sh"
  fi
fi

# ---------------------------------------------------------------- done
cat <<EOF

$( [ "$DRY" = 1 ] && echo "Dry run: nothing was changed." || echo "Ready." )

  console          $BIN --root $ROOT shell
  run a script     $BIN --root $ROOT run scripts/drive-kernel.aos
  SQL server       $BIN --root $ROOT exec "sql serve port=3307"   then: mysql -h127.0.0.1 -P3307 -uroot
  resident machine AOS_ROOT=$ROOT scripts/aos.sh install | status | console
  docs             README.md has the index: docs/console.md is the place to start
EOF
