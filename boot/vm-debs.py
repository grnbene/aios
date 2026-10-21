#!/usr/bin/env python3
"""Resolve, fetch and verify Ubuntu arm64 packages without apt or root.

  boot/vm-debs.py <cache-dir> <lockfile> pkg [pkg ...]

The index comes from one frozen release pocket (ports.ubuntu.com, noble), so the
same names resolve to the same files on every run. InRelease is checked with
gpgv against the Ubuntu archive keyring, each Packages.xz against InRelease's
SHA256, and each .deb against the SHA256 in Packages. Prints one .deb path per
line (dependency closure, Pre-Depends + Depends, first satisfiable alternative)
and writes <lockfile>: name version sha256 for every package taken.
"""
import hashlib, lzma, os, re, subprocess, sys, urllib.request

MIRROR = os.environ.get("VM_MIRROR", "https://ports.ubuntu.com/ubuntu-ports")
SUITE = os.environ.get("VM_SUITE", "noble")
COMPONENTS = ["main", "universe"]
ARCH = "arm64"
KEYRING = os.environ.get("VM_KEYRING", "/usr/share/keyrings/ubuntu-archive-keyring.gpg")
# Never pulled in: they only matter to a system that runs maintainer scripts.
SKIP = {"debconf", "debconf-2.0", "init-system-helpers", "dpkg", "install-info", "libpam-runtime",
        "systemd", "systemd-sysv", "libsystemd-shared", "dbus", "default-dbus-system-bus", "dbus-system-bus", "cdebconf"}


def fetch(url, dest):
    if os.path.exists(dest):
        return dest
    tmp = dest + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
    os.rename(tmp, dest)
    return dest


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def parse(text):
    out = []
    for para in text.split("\n\n"):
        d, key = {}, None
        for line in para.splitlines():
            if line.startswith((" ", "\t")) and key:
                d[key] += "\n" + line
            elif ":" in line:
                key, v = line.split(":", 1)
                d[key] = v.strip()
        if "Package" in d:
            out.append(d)
    return out


def main():
    cache, lockfile, wanted = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(os.path.join(cache, "debs"), exist_ok=True)
    rel = fetch(f"{MIRROR}/dists/{SUITE}/InRelease", os.path.join(cache, f"{SUITE}-InRelease"))
    subprocess.run(["gpgv", "--keyring", KEYRING, rel], check=True, capture_output=True)
    sums = {}
    insha = False
    for line in open(rel, encoding="utf-8", errors="replace"):
        if line.startswith("SHA256:"):
            insha = True
            continue
        if insha:
            if not line.startswith(" "):
                insha = False
                continue
            h, _, name = line.split()
            sums[name] = h
    pkgs, provides = {}, {}
    for comp in COMPONENTS:
        name = f"{comp}/binary-{ARCH}/Packages.xz"
        p = fetch(f"{MIRROR}/dists/{SUITE}/{name}", os.path.join(cache, f"{SUITE}-{comp}-Packages.xz"))
        if sha256(p) != sums[name]:
            os.unlink(p)
            sys.exit(f"checksum mismatch: {name}")
        for d in parse(lzma.open(p).read().decode("utf-8", "replace")):
            pkgs.setdefault(d["Package"], d)
            for pr in d.get("Provides", "").split(","):
                pr = re.sub(r"\(.*\)", "", pr).strip()
                if pr:
                    provides.setdefault(pr, []).append(d["Package"])

    chosen, order = {}, []

    def pick(alts):
        names = [re.sub(r"\(.*?\)|\[.*?\]|<.*?>", "", a).strip().split(":")[0] for a in alts.split("|")]
        for n in names:
            if n in chosen or n in SKIP:
                return None
        for n in names:
            if n in pkgs:
                return n
        for n in names:
            for p in provides.get(n, []):
                if p in chosen:
                    return None
            if provides.get(n):
                return provides[n][0]
        print(f"warning: nothing provides {alts}", file=sys.stderr)
        return None

    def visit(n):
        if n in chosen or n in SKIP:
            return
        if n not in pkgs:
            sys.exit(f"no package {n} in {SUITE}/{ARCH}")
        chosen[n] = pkgs[n]
        for field in ("Pre-Depends", "Depends"):
            for alts in filter(None, (x.strip() for x in pkgs[n].get(field, "").split(","))):
                dep = pick(alts)
                if dep:
                    visit(dep)
        order.append(n)

    for w in wanted:
        visit(w)
    with open(lockfile, "w") as lock:
        lock.write(f"# {MIRROR} {SUITE} {ARCH}: package version sha256\n")
        for n in sorted(chosen):
            d = chosen[n]
            lock.write(f"{n} {d['Version']} {d['SHA256']}\n")
    for n in order:
        d = chosen[n]
        dest = os.path.join(cache, "debs", os.path.basename(d["Filename"]))
        fetch(f"{MIRROR}/{d['Filename']}", dest)
        if sha256(dest) != d["SHA256"]:
            os.unlink(dest)
            sys.exit(f"checksum mismatch: {d['Filename']}")
        print(dest)


if __name__ == "__main__":
    main()
