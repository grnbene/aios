# ASQL packaging

Build one portable binary, install it on any host with two commands.

## Why it is built the way it is

**glibc is only backward compatible.** Target deployment hosts across clusters span different glibc floors:

| Host / Target Group | OS              | glibc | systemd | MariaDB  |
|---------------------|-----------------|-------|---------|----------|
| `chi-il-1`, `atl-ga-1` (primary hosts) | AlmaLinux 9.7   | 2.34  | 252     | 10.11.15 |
| `km` (secondary hosts)                 | AlmaLinux 8.10  | 2.28  | 239     | 10.3.39  |
| `sn` (legacy hosts)                    | CentOS 7        | 2.17  | 219     | 10.9.8   |

A binary built on AlmaLinux 9 will not start on older hosts like `km` or `sn`. One built on newer distributions (e.g. Ubuntu 24.04, glibc 2.39) starts on none of them. Therefore, the release build is compiled against the glibc 2.17 baseline so that a single artifact runs across all hosts.

Legacy CentOS 7 hosts cannot build the binary directly: CentOS 7 ships with GCC 4.8.5 without modern toolsets. Builds should be produced either in Docker (via `build-centos7.sh`), or on a dedicated EL7 build host with devtoolset-11 (GCC 11.2+) and cmake3 3.17.5+ via `BUILDER=<host>` (e.g. `devops`; never build on production hosts). Builds only write to `/tmp` on the builder.

The link is `-static-libstdc++ -static-libgcc`, **not** fully static:
`src/compute/opencl_device.cpp:341` documents that a statically linked build
cannot safely `dlopen`.

Two things the el7 build needs that the builder does not have, both fetched
into the builder's `/tmp` so the host itself is never modified:

- **cmake >= 3.20** — el7 ships 3.16/3.17, the project requires 3.20.
- **`CL/cl.h`** — `src/compute/opencl_device.cpp` includes it unconditionally.
  The OpenCL library is `dlopen`'d at runtime, so these headers are a
  compile-time need only and vendoring them adds no runtime dependency.

Building for el7 also needed one source fix, in `src/hal/linux_host.cpp`:
glibc only grew the `memfd_create` wrapper in 2.27, so on glibc 2.17 the call
does not compile. The syscall itself is present (RHEL backported it into its
3.10 kernel — verified on sn, kernel 3.10.0-1160), so the file now calls it
directly when the wrapper is absent and uses the native wrapper everywhere
else.

## Build

Two ways to the same artifact:

    # in a container on any docker host (arch): a git ref, never the dirty tree
    TARGETS="aios-cli aios-asql-top aios-asql-console" ./build-centos7.sh origin/main ../build-centos7
    ./pack.sh ../build-centos7 dist          # tarball + .sha256 + MANIFEST

    # or on an el7 host over ssh, when there is no docker
    BUILDER=devops ./build-portable.sh   # builds on devops, writes dist/

Output is `dist/aios-asql-<version>-linux-x86_64-glibc2.17.tar.gz` plus a
`.sha256`. The script refuses to package a binary whose glibc requirement came
out above the 2.17 floor, and smoke-tests it on the builder first.

## Install

Unpack the tarball and run the installer inside it:

    tar xzf aios-asql-<version>-linux-x86_64-glibc2.17.tar.gz
    cd aios-asql-<version>-linux-x86_64-glibc2.17

    ./install-asql.sh --host km --dry-run   # print every change, make none
    ./install-asql.sh --host km             # do it

`--host` ships the package over ssh and runs the installer on the target;
without it, the installer acts on the machine it is already on.

It is safe to re-run. It backs up the existing binary before swapping, never
overwrites an existing `/etc/aios/asql.conf` or an existing secret file, and
rolls back to the previous binary if the service fails to come up.
`--no-service` installs the files, user and conf without a systemd unit (a
container, or a host you run by hand: `asql-serve` as user `aios`);
`--no-start` writes and enables the unit but leaves the service alone.

A conf that pins `AIOS_BIN` to some other binary (a hand deploy's
`aios.<hash>`) is re-pointed at the package's binary, with a backup of the conf.

## Is it right? — `asql-check`

    /opt/aios/bin/asql-check            every whitelisted table against the master (minutes)
    /opt/aios/bin/asql-check --quick    service, wire, binlog position, Things (seconds)
    /opt/aios/bin/asql-check orders

The acceptance test after an install or upgrade, exit 0 only when: the unit is
running (by start timestamp and pid), the unix socket answers, the binlog is
caught up (or within `--lag-ok` / `LAG_OK` bytes), every table is present with
the master's row and column counts (`SHOW ASQL MIRROR`: the engine asks the
master itself, one tagged statement per table), and every `.thing` on disk
loaded. Run it right after the first start and it will say the wire is not up:
the engine seeds the whitelist before the wire listens, and a first seed over a
WAN takes as long as the tables are big — watch `journalctl -u aios-compute`
for `whitelist - N of M table(s) present`. A stream the network breaks resumes
from the table's last primary key on a fresh snapshot; a table without a
single-column key starts over once, then waits for the next start.

## What lands on the host

    /opt/aios/bin/aios          the engine
    /opt/aios/bin/asql-serve    reads the conf, execs the engine
    /opt/aios/bin/asql-check    the acceptance test (above)
    /opt/aios/bin/aios-webserver, /etc/aios/webserver.conf.example   the HTTP(S) front for php-fpm;
                                its unit is enabled only once /etc/aios/webserver.conf exists
    /opt/aios/things/           the Things declarations shipped with this version (declare with the console)
    /opt/aios/bin/asql-export.sh, asql-import.sh   move the data between hosts
    /opt/aios/root/             kernel root: sql_db, store, tasks
    /opt/aios/cdc.pass          secrets, 0600, aios:aios
    /opt/aios/mysql.pass
    /opt/aios/compute.token
    /etc/aios/asql.conf         the only file that differs between hosts
    /etc/systemd/system/aios-compute.service

## Configuration

Everything lives in `/etc/aios/asql.conf` — ports, CDC upstream, and the table
whitelist, which is 71 tables on chi-il-1 and is the reason it cannot stay in
`ExecStart`. The engine reads no environment and takes positional `key=value`
arguments, so `asql-serve` translates the conf into that argument string. The
systemd unit is then identical on every host.

Leave a key empty to fall through to the engine's own default rather than
pinning it to a blank. To see exactly what will be run:

    /opt/aios/bin/asql-serve --print

Passwords stay out of the unit file: the engine's `cdc_pass_file` /
`mysql_pass_file` / `token_file` options point at the 0600 files above, so
nothing secret appears in `systemctl show` or `ps`.

## Migrating a host that already runs a hand-written unit

chi-il-1 and atl-ga-1 carry all their arguments inline in `ExecStart`. Derive
the conf from what is actually running before installing, never from the
example:

    ./import-unit-conf.sh chi-il-1 /tmp/chi-asql.conf

A key the live unit does not set is emitted **empty** on purpose. On chi-il-1
the unit sets no `mysql_bind`, so the wire listener is on `0.0.0.0`; seeding
`MYSQL_BIND=127.0.0.1` from the example would silently cut off every remote
Navicat/DBeaver client. Diff `asql-serve --print` against the live `ExecStart`
before you restart anything.

## Three host differences the installer handles

- **`/etc/aios` must be group-readable by the service user.** The service runs
  as `aios`, so it has to TRAVERSE the directory, not merely read the file in
  it. Created `root:root 0750` the engine cannot reach its own config and exits
  78 on every start, which `Restart=always` turns into a crash loop. This cost
  chi-il-1 about ninety seconds of downtime on 2026-09-18. The installer now
  creates the directory `root:aios` and, before restarting, checks readability
  **as the service user** rather than as root.

- **systemd 219 on `sn` rejects `ReadWritePaths`** — it is an unknown directive
  before systemd 231. The installer omits it there and includes it elsewhere,
  then runs `systemd-analyze verify` on what it generated.
- **`--help` is the only safe smoke test.** The CLI treats an unrecognised flag
  as a command and boots the kernel for real; `--version` does not exist. The
  pre-swap load probe uses `--help`.

## Verifying a restart

The installer compares `ExecMainStartTimestamp` before and after, and checks
`MainPID`. `systemctl is-active` reports `active` both before and after a
restart that was silently refused, so it is not used as evidence.

## Moving the data to another server

Do not copy `/opt/aios` (or the live `sql_db`): it holds old generations and
backups, and a copy taken during a save is torn. Use the pair:

    # on the source host (any user that can read the conf and the store)
    asql-export.sh -o /tmp/asql.tar.zst        # also writes /tmp/asql.tar.zst.sha256

    # copy both files, then on the target host (ASQL installed there already)
    asql-import.sh asql.tar.zst

The export asks the running engine for `EXPORT SNAPSHOT 'name'`: it checkpoints,
hard-links one generation of table files under `sql_db/exports/name` (instant,
no extra disk, writers wait milliseconds), and the script packs that and removes
it. It is refused while another session has a transaction open; the script
asks again. Output is zstd when installed, else gzip.

The import verifies the checksum, stops `aios-compute`, puts the store in place
(an existing one is only moved aside, `--force`, never deleted), starts the
service and waits until it answers. The source's replication position is
dropped unless `--keep-replication-state`: it names a binlog on the source's
MariaDB. `--no-service` for an engine not run by the systemd unit.
