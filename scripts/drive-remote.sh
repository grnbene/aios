#!/usr/bin/env bash
# Drive the kernel's own git transport and ssh client against a REAL remote.
#
#   scripts/drive-remote.sh [build-dir] [work-dir]
#
# Everything here is the product being used the way an operator uses it: a bare
# repository served by a real `git daemon`, cloned by the kernel over its own
# wire protocol, then checked with real git — which is the only opinion that
# counts about whether what this machine wrote is a git repository.
#
# The ssh leg runs when this machine can already reach an sshd with the kernel's
# key. Point it at one with:
#     AIOS_DRIVE_SSH=user@host scripts/drive-remote.sh
# The drive prints the identity to authorise if the server refuses it.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$here/build}
work=${2:-/tmp/aios-drive-remote}
aios=$build/aios
[ -x "$aios" ] || { echo "no command surface at $aios — build first"; exit 1; }
command -v git >/dev/null || { echo "this drive needs real git to check the kernel's work"; exit 1; }

rm -rf "$work"; mkdir -p "$work"
root=$work/root
fail=0
step() { printf '\n=== %s\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
bad()  { printf '  FAIL %s\n' "$*"; fail=$((fail+1)); }

# ---------------------------------------------------------------- the remote
step "a real bare repository, served by a real git daemon"
src=$work/upstream
git init -q "$src"
mkdir -p "$src/lib"
echo "the object database is the kernel's own" > "$src/README.md"
printf '#!/bin/sh\necho hello from a program\n' > "$src/run.sh"
chmod +x "$src/run.sh"
ln -s ../README.md "$src/lib/link-to-readme"
for i in 1 2 3 4 5 6 7 8; do
  # A file that changes every commit, so the pack the remote builds carries
  # real deltas for the kernel to reconstruct rather than whole blobs.
  head -c 20000 /dev/urandom | base64 > "$src/data.txt"
  echo "generation $i" >> "$src/history.txt"
  git -C "$src" add -A
  git -C "$src" -c user.email=drive@aios -c user.name=drive commit -qm "generation $i"
done
bare=$work/upstream.git
git clone -q --bare "$src" "$bare"
upstream_head=$(git -C "$bare" rev-parse HEAD)
objects_here=$(git -C "$bare" rev-list --objects --all | sort | sha1sum | cut -d' ' -f1)
ok "upstream $bare at $upstream_head"

port=0
for p in $(seq 9520 9560); do
  if ! (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; then port=$p; break; fi
done
[ "$port" != 0 ] || { echo "no free port for the daemon"; exit 1; }
git daemon --base-path="$work" --export-all --enable=receive-pack --listen=127.0.0.1 --port=$port --reuseaddr >"$work/daemon.log" 2>&1 &
daemon_pid=$!
trap 'kill $daemon_pid 2>/dev/null' EXIT
sleep 1
ok "git daemon on 127.0.0.1:$port (pid $daemon_pid)"

run() { "$aios" --root "$root" exec "$@" 2>&1; }

# ---------------------------------------------------------------- ls-remote
step "the kernel asks the remote what it has"
out=$(run "git ls-remote git://127.0.0.1:$port/upstream.git")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "$upstream_head" && ok "the remote's HEAD came back" || bad "no HEAD in the advertisement"

# ---------------------------------------------------------------- clone
step "the kernel clones it — its own pkt-line, its own packfile indexing"
out=$(run "git clone git://127.0.0.1:$port/upstream.git upstream")
echo "$out" | sed 's/^/  | /'
clone=$root/repos/upstream
[ -d "$clone/.git/objects" ] || bad "no object database at $clone"

step "REAL git's opinion of what the kernel wrote"
if git -C "$clone" fsck --no-progress >"$work/fsck.log" 2>&1; then ok "git fsck: clean"; else bad "git fsck: $(head -3 "$work/fsck.log")"; fi
status=$(git -C "$clone" status --short)
[ -z "$status" ] && ok "git status: nothing to report — the work tree matches the commit" || bad "git status: $status"
got=$(git -C "$clone" rev-list --objects --all | sort | sha1sum | cut -d' ' -f1)
[ "$got" = "$objects_here" ] && ok "every object of the whole history, id for id ($got)" || bad "object graph differs: $got vs $objects_here"
[ -x "$clone/run.sh" ] && ok "the executable bit survived the checkout" || bad "run.sh is not executable"
[ -L "$clone/lib/link-to-readme" ] && ok "the symlink is a symlink" || bad "lib/link-to-readme is not a link"

# ---------------------------------------------------------------- fetch
step "one new commit upstream: a fetch brings it down, a pull moves onto it"
echo "a line only the second fetch can know" >> "$src/history.txt"
git -C "$src" add -A
git -C "$src" -c user.email=drive@aios -c user.name=drive commit -qm "after the clone"
git -C "$src" push -q "$bare" HEAD:main 2>/dev/null || git -C "$src" push -q "$bare" HEAD:master
moved=$(git -C "$bare" rev-parse HEAD)
out=$(run "git use upstream" "git fetch")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "$moved" && ok "the fetch brought the new commit down ($moved)" || bad "the fetch did not bring the new commit"
bytes=$(echo "$out" | sed -n 's/.*pack *: \([0-9.]*\) \([A-Za-z]*\),.*/\1 \2/p' | head -1)
case "$bytes" in *MiB*) bad "the fetch moved $bytes — that is the whole repository again";; *) ok "the fetch moved $bytes";; esac
head_here=$(git -C "$clone" rev-parse HEAD)
[ "$head_here" != "$moved" ] && ok "a fetch left this machine's own branch where it was ($head_here)" || bad "the fetch moved the local branch"
[ "$(git -C "$clone" rev-parse refs/remotes/origin/main 2>/dev/null || git -C "$clone" rev-parse refs/remotes/origin/master)" = "$moved" ] \
  && ok "refs/remotes/origin/* records where the remote is" || bad "the remote-tracking ref was not updated"
out=$(run "git use upstream" "git pull")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "branch moved" && ok "the pull moved the branch and laid the files down" || bad "the pull did not advance the branch"
[ "$(git -C "$clone" rev-parse HEAD)" = "$moved" ] && ok "real git: HEAD is now the upstream commit" || bad "HEAD did not follow the pull"
status=$(git -C "$clone" status --short)
[ -z "$status" ] && ok "real git: the work tree is clean after the pull" || bad "after the pull real git sees: $status"
out=$(run "git use upstream" "git fetch")
echo "$out" | grep -q "0 object(s)" && ok "an up-to-date fetch moves no objects at all" || bad "a second fetch still transferred objects"

# ---------------------------------------------------------------- push
step "the other direction: the machine's own commit, packed here and pushed"
echo "written by the kernel itself, and pushed by it" > "$clone/FROM-AIOS.txt"
out=$(run "git use upstream" "git add FROM-AIOS.txt" "git commit a commit this machine made and pushed itself" \
          "git push git://127.0.0.1:$port/upstream.git main")
echo "$out" | sed 's/^/  | /'
pushed=$(git -C "$bare" rev-parse HEAD)
echo "$out" | grep -q "unpack ok" && ok "the remote unpacked what this kernel packed" || bad "the remote would not unpack it"
echo "$out" | grep -q "$pushed" && ok "the remote's branch moved to the commit this machine made ($pushed)" || bad "the remote's ref did not move"
if git -C "$bare" fsck --no-progress >"$work/fsck2.log" 2>&1; then ok "git fsck on the REMOTE: clean after taking this machine's pack"; else bad "the remote does not fsck: $(head -3 "$work/fsck2.log")"; fi
body=$(git -C "$bare" show HEAD:FROM-AIOS.txt 2>/dev/null)
[ "$body" = "written by the kernel itself, and pushed by it" ] && ok "real git reads the file back out of the pushed commit" || bad "the pushed content came back as: $body"

out=$(run "git use upstream" "git push git://127.0.0.1:$port/upstream.git main")
echo "$out" | grep -q "already at" && ok "a second push sends nothing: the remote is already there" || bad "an up-to-date push still sent something"

step "a push that would drop someone else's work is refused, unless forced"
git -C "$src" fetch -q "$bare" main 2>/dev/null
git -C "$src" reset -q --hard FETCH_HEAD
echo "a commit the machine has never seen" > "$src/ELSEWHERE.txt"
git -C "$src" add -A
git -C "$src" -c user.email=drive@aios -c user.name=drive commit -qm "someone else pushed first"
git -C "$src" push -q "$bare" HEAD:main
echo "and one more of ours" > "$clone/AGAIN.txt"
out=$(run "git use upstream" "git add AGAIN.txt" "git commit a commit that would overwrite theirs" \
          "git push git://127.0.0.1:$port/upstream.git main")
echo "$out" | grep -qi "not a fast-forward" && ok "refused: $(echo "$out" | tail -1 | cut -c1-96)" || bad "a non-fast-forward push was not refused: $(echo "$out" | tail -1)"
out=$(run "git use upstream" "git push git://127.0.0.1:$port/upstream.git main force=1")
echo "$out" | grep -q "forced" && ok "force=1 says so out loud and the remote takes it" || bad "the forced push did not go through"
git -C "$bare" fsck --no-progress >/dev/null 2>&1 && ok "the remote still fscks clean after the forced push" || bad "the remote is damaged after a forced push"

# ---------------------------------------------------------------- branches
step "a branch made here, moved onto, and pushed as a branch"
out=$(run "git use upstream" "git branch side" "git switch side")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "\[SWITCH\] on side" && ok "HEAD moved onto the new branch" || bad "the switch did not take"
echo "work that only exists on the side branch" > "$clone/SIDE.txt"
out=$(run "git use upstream" "git add SIDE.txt" "git commit a commit on the side branch" \
          "git push git://127.0.0.1:$port/upstream.git side")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "unpack ok" && ok "the branch was created on the remote" || bad "the branch push failed"
git -C "$bare" rev-parse --verify -q refs/heads/side >/dev/null && ok "real git on the remote lists refs/heads/side" || bad "the remote has no side branch"
out=$(run "git use upstream" "git switch main")
echo "$out" | grep -q "\[SWITCH\] on main" && ok "switching back moves HEAD again" || bad "could not switch back"
[ ! -f "$clone/SIDE.txt" ] && ok "the side branch's file is gone from the work tree, as git leaves it" || bad "SIDE.txt survived the switch back"
status=$(git -C "$clone" status --short)
[ -z "$status" ] && ok "real git: the work tree is clean after moving between branches" || bad "real git sees: $status"

# ---------------------------------------------------------------- refusals
step "the ugly edges — each one a refusal with a reason, none of them a crash"
check_refusal() {
  local what="$1" want="$2"; shift 2
  local o
  o=$("$aios" --root "$root" exec "$@" 2>&1)
  if echo "$o" | grep -qi "$want"; then ok "$what: $(echo "$o" | tail -1 | cut -c1-110)"
  else bad "$what: expected /$want/, got: $(echo "$o" | tail -1)"; fi
}
check_refusal "https to non-existent repo" "not-found" "git clone https://example.com/x.git"
check_refusal "no such repository"    "not exported\|does not appear to be a git repository\|not-found" "git ls-remote git://127.0.0.1:$port/nosuch.git"
check_refusal "nothing listening"     "refused\|io-error"  "git ls-remote git://127.0.0.1:9/x.git"
check_refusal "an entity with no network authority" "may not reach the network" \
  "entity create stranger => s" "git ls-remote git://127.0.0.1:$port/upstream.git as=\$s"

# ---------------------------------------------------------------- ssh
step "ssh — the kernel's own client"
out=$(run "ssh keygen")
echo "$out" | sed 's/^/  | /'
echo "$out" | grep -q "SHA256:" && ok "an ed25519 identity of this machine" || bad "no identity"
if command -v ssh-keygen >/dev/null; then
  if ssh-keygen -y -f "$root/ssh/id_ed25519" >/dev/null 2>&1; then
    ok "OpenSSH reads the private key this kernel wrote"
  else bad "OpenSSH will not read the key this kernel wrote"; fi
fi
if [ -n "${AIOS_DRIVE_SSH:-}" ]; then
  # An sshd will only accept a key its account already authorises, so a drive
  # against a real host is pointed at one this machine already holds.
  keyarg=""
  [ -n "${AIOS_DRIVE_SSH_KEY:-}" ] && keyarg="key=$AIOS_DRIVE_SSH_KEY"
  out=$(run "ssh $AIOS_DRIVE_SSH uname -sm $keyarg")
  echo "$out" | sed 's/^/  | /'
  echo "$out" | grep -q "exit          : 0" && ok "a command ran on $AIOS_DRIVE_SSH and came back" || bad "the ssh leg did not complete"
  echo "$out" | grep -q "curve25519-sha256 + chacha20-poly1305" && ok "curve25519 kex, chacha20-poly1305 transport" || bad "unexpected algorithms"
  echo "$out" | grep -q "host key      : SHA256:" && ok "the host key was verified and recorded" || bad "no host key check"

  step "the same clone again, but carried over ssh"
  out=$(run "git clone ssh://$AIOS_DRIVE_SSH$bare over-ssh $keyarg")
  echo "$out" | sed 's/^/  | /'
  sshclone=$root/repos/over-ssh
  if git -C "$sshclone" fsck --no-progress >/dev/null 2>&1; then ok "git fsck: clean over ssh"; else bad "the ssh clone does not fsck"; fi
  got=$(git -C "$sshclone" rev-list --objects --all | sort | sha1sum | cut -d' ' -f1)
  want=$(git -C "$bare" rev-list --objects --all | sort | sha1sum | cut -d' ' -f1)
  [ "$got" = "$want" ] && ok "identical object graph over ssh ($got)" || bad "the ssh clone differs: $got vs $want"

  step "and a push over ssh, into a repository real git then reads"
  out=$(run "git use over-ssh" "git push $keyarg" )
  echo "$out" | sed 's/^/  | /'
  echo "$out" | grep -q "already at\|unpack ok" && ok "the ssh push was accepted (or was already there)" || bad "the ssh push did not complete"

  step "a host key that changed is refused, not accepted quietly"
  cp "$root/ssh/known_hosts" "$work/known_hosts.bak"
  awk '{ if (NF>=3) { $3="AAAAC3NzaC1lZDI1NTE5AAAAIBRUP2gIh1XACJxWaSwe6BX+e+V2LR+QSt+Y0C+Y8Qo1" } print }' "$work/known_hosts.bak" > "$root/ssh/known_hosts"
  out=$(run "ssh $AIOS_DRIVE_SSH id $keyarg")
  echo "$out" | grep -qi "has CHANGED" && ok "refused: $(echo "$out" | tail -1 | cut -c1-90)" || bad "a changed host key was not refused: $(echo "$out" | tail -1)"
  cp "$work/known_hosts.bak" "$root/ssh/known_hosts"
else
  echo "  --   ssh leg skipped: set AIOS_DRIVE_SSH=user@host [AIOS_DRIVE_SSH_KEY=<private key>]"
  echo "  --   this machine's identity is $(cut -d' ' -f2 "$root/ssh/id_ed25519.pub" | cut -c1-24)…"
fi

printf '\n'
if [ "$fail" = 0 ]; then echo "REMOTE: PASS"; else echo "REMOTE: FAIL ($fail)"; fi
exit $fail
