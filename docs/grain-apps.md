# Running and managing Grain applications

`grain host` is a small daemon that runs your programs as managed services. It restarts them when they crash, upgrades them with zero downtime, and keeps a release history so you can roll back with one command.

You control it through `grain app` — either locally or across machines over TCP.

Related guides:
- [Writing a Grain application](grain-app-writing.md): manifests, routes, workers, and deployment hooks.
- [Hosting applications with AIOS](aios-serve.md): running the supervisor inside AIOS with fleet orchestration.
- Example applications: [Hello World](../examples/apps/hello/README.md), [REST API](../examples/apps/api/README.md), [Background Worker](../examples/apps/worker/README.md), and [Stickerfeed Catalog](../apps/stickerfeed/README.md).

---

## Quickstart

Start the host in one terminal:

```bash
grain host
```

It runs in the foreground, stores everything under `~/.grain` (override with `$GRAIN_HOME` or `--home`), and listens on `127.0.0.1:7480`.

In another terminal, check and deploy the hello-world example:

```bash
# Make sure the manifest and every .gr file compile
grain app check examples/apps/hello

# Deploy it
grain app deploy examples/apps/hello
```

You'll see:
```text
hello: 5 file(s), version 784e4bbb55fc517c, web on port 18601; everything compiles
hello: version 784e4bbb55fc517c (first deploy)
  started; healthy
  check.gr: passed
  running
```

Check what's running:

```bash
grain app status
```

```text
HOST         APP                STATE      VERSION           PORT   UP     RESTARTS REQUESTS  ERRORS
local        hello              running    784e4bbb55fc517c  18601  5s     0        2         0
```

Hit the endpoint:

```bash
curl http://localhost:18601/
```

```json
{"greeting":"hello from hello 784e4bbb55fc517c","up_s":5}
```

If you don't want a daemon while developing, just run the app directly:

```bash
grain app run examples/apps/hello
```

It runs right in your terminal, prints logs to stdout, and exits when you press Ctrl+C. Its settings, data and log go in a `.grain/` folder inside the app folder, not in `~/.grain`; delete it when you are done.

---

## How deployments work (zero downtime)

When you run `grain app deploy <dir>`, the host walks through these steps:

1. **Compiles everything first**. Every source file is compiled before anything touches disk. A syntax error or broken import stops the deploy immediately, leaving the running app untouched.
2. **Hashes the release**. The version is the SHA-256 hash of all files in the directory (e.g. `784e4bbb55fc517c`). If you deploy without changes, the host tells you that version is already running and exits.
3. **Spins up the new version on a spare port**. The old version keeps taking live traffic on the real port.
4. **Polls the health check**. It hits `health.path` (default `/`) until the new process answers with anything below a server error (or one of the codes `health.ok` lists).
5. **Runs `check.gr`**. If your app has a check script, the host calls its `check(app)` function against the staging port. You can assert responses, verify database tables, or test headers. If `check` returns false (or throws), the deploy aborts.
6. **Swaps the listening socket**. The host hands the real port's socket to the new process and tells the old one to drain. Active requests finish cleanly; new ones hit the new code. No dropped TCP packets.
7. **Runs `deployed.gr`**. If present, this runs after the new code is live. Use it for database migrations, populating caches, or updating ASQL Things.

If anything fails between steps 1 and 5, the host kills the staging process, prints the error, and leaves your production traffic running on the old release.

---

## Rollbacks and release history

Every version ever deployed is archived by hash under `~/.grain/apps/<name>/versions/`.

To see the history:

```bash
grain app versions hello
```

To roll back to the version before the current one:

```bash
grain app rollback hello
```

This starts the previous release using the same zero-downtime procedure: it tests the old version on a staging port, runs its `check.gr`, swaps the socket, and runs its `deployed.gr` so any schema or Thing changes roll back in lockstep.

---

## Settings and secrets

Keep secrets and environment settings out of your code and git commits.

### Settings

Normal settings are plain strings saved in `settings.json`:

```bash
grain app set hello GREETING="Good morning" TIMEOUT_MS=5000
grain app settings hello
grain app unset hello GREETING
```

Inside your app, read them with `getenv("GREETING")`.

### Secrets

Pass passwords, tokens, and private keys via stdin so they never show up in `ps` or your shell history:

```bash
echo -n 'super-secret-token' | grain app secret hello SIGNATURE
grain app secrets hello
```

Secrets are encrypted on disk with AES-256-GCM (`~/.grain/apps/<name>/secrets.sealed`) using a random 256-bit machine key (`~/.grain/machine.key`, mode 0600). Your app reads them with `getenv("SIGNATURE")` just like settings.

Settings and secret changes take effect on the next start:

```bash
grain app restart hello
```

---

## Monitoring and logs

```bash
# Overview of all apps
grain app status

# Just one app
grain app status hello

# Recent logs
grain app logs hello

# Follow live output
grain app logs hello -f

# Last 50 lines
grain app logs hello -n 50
```

The log records app `print()` calls, uncaught exceptions, and supervisor lifecycle events (starts, health checks, restarts, rollbacks).

### Stopping, starting, and removing

```bash
grain app stop hello       # stops the process; stays stopped across host reboots
grain app start hello      # starts it again
grain app restart hello    # restarts the current version
grain app remove hello     # stops and unregisters the app (keeps version archives and data)
```

If an app crashes unexpectedly, the host restarts it with exponential backoff. If it crashes 10 times in 5 minutes, the host marks it `failed` and stops restarting so it doesn't peg the CPU. Once you fix the bug, run `grain app deploy` or `grain app restart` to bring it back.

---

## Managing remote nodes

Every `grain app` command takes an optional `at <target>`:

| Target | What it reaches |
|---|---|
| *(omitted)* | Local daemon (`127.0.0.1:7480`) |
| `at web2` | Named node in `~/.grain/hosts.json` |
| `at 10.0.0.5:7480 key=@web2.key` | Direct address with key file |

### Setting up a remote server

1. On the remote server (`web2`), get its public key:
   ```bash
   grain host key
   ```

2. On your laptop, save that key and register the alias:
   ```bash
   chmod 600 web2.key
   grain app add-host web2 10.0.0.5:7480 key=@web2.key
   grain app hosts
   ```

3. Deploy and manage it from your laptop:
   ```bash
   grain app deploy ./examples/apps/hello at web2
   grain app status --all
   grain app logs hello -f at web2
   ```

By default `grain host` binds loopback (`127.0.0.1`). When running on a remote node, start it with `--all-ips` (and firewall port 7480 to your internal network or VPN). All communication is authenticated and encrypted; connections with the wrong key are dropped immediately.

---

## Running on system boot (systemd)

To run the host as a systemd service:

```bash
grain host install
```

This writes and enables a systemd service unit that restarts on failure and starts automatically on boot.

You can also pass custom flags to `grain host`:
```bash
grain host --port 8480 --home /var/lib/grain --all-ips
```

---

## Where files live

The host keeps everything in `~/.grain` (or `$GRAIN_HOME`):

```text
~/.grain/
├── host.key              # authentication key for incoming control connections
├── machine.key           # AES-256-GCM key for encrypting secrets (mode 0600)
├── hosts.json            # known remote servers
└── apps/
    └── <name>/
        ├── versions/     # release tarballs, named by SHA-256 hash
        ├── record.json   # currently running version and state
        ├── settings.json # environment settings
        ├── secrets.sealed# encrypted secrets
        ├── data/         # app working directory / persistent data
        └── log           # stdout, stderr, and supervisor events
```

Backing up `~/.grain` preserves all releases, configuration, and data.

---

## Command cheat sheet

```text
Host daemon:
  grain host [--home DIR] [--port N] [--all-ips]   run supervisor
  grain host install                               install systemd service
  grain host key                                   print authentication key

Apps:
  grain app check <dir>                            verify syntax and compile all files
  grain app run <dir>                              run in foreground (no host needed)
  grain app deploy <dir> [at HOST]                 deploy with zero downtime
  grain app rollback <app> [at HOST]               revert to previous release
  grain app versions <app> [at HOST]               list release history

Supervision:
  grain app status [APP] [at HOST | --all]         show running apps
  grain app start <app> [at HOST]                  start an app
  grain app stop <app> [at HOST]                   stop an app
  grain app restart <app> [at HOST]                restart an app
  grain app remove <app> [at HOST]                 unregister an app
  grain app logs <app> [-n N] [-f] [at HOST]       show or follow logs

Configuration:
  grain app set <app> K=V... [at HOST]             set environment variable
  grain app unset <app> K... [at HOST]             remove setting override
  grain app settings <app> [at HOST]               list settings
  grain app secret <app> NAME [at HOST]            set secret from stdin
  grain app secrets <app> [at HOST]                list secret names

Remote hosts:
  grain app hosts                                  list known hosts
  grain app add-host NAME ADDR[:PORT] key=<k|@f>   save host alias
```
