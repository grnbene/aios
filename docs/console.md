# The `aios` console

`build/aios` is the command surface of AIOS. Everything you can do to a
machine (look at it, make entities, grant authority, run SQL, serve a port,
put something on the display) is a line typed into this surface. This page
covers how to start it, how to keep one running, and what the commands are.

A model is not required. The kernel, the shell, entities, authority, storage,
SQL, networking and compute all work with no model attached. A handful of
commands (`ask`, `dream`, `organ grow`) hand work to a model bound in the
*intelligence slot*; those are described in their own section and say what
happens when nothing real is bound.

Every example below was run against `build/aios` with a throwaway root and
`AIOS_NO_ACCEL=1`. Output is trimmed (`…`), not edited.

## Contents

- [Starting the console](#starting-the-console)
- [What a root is](#what-a-root-is)
- [Line syntax: variables, `=>`, `!`](#line-syntax)
- [The familiar shell](#the-familiar-shell)
- [Entities, capabilities and authority](#entities-capabilities-and-authority)
- [Standing directives and `attend`](#standing-directives-and-attend)
- [The resident machine (`scripts/aos.sh`)](#the-resident-machine)
- [The intelligence slot (optional)](#the-intelligence-slot-optional)
- [Command groups](#command-groups)
- [Environment variables](#environment-variables)

## Starting the console

```
build/aios [--root DIR] [-q|--quiet] <mode> ...
```

`--root` defaults to `$AIOS_ROOT`, then `./aios-root`. `aios --help` prints the
modes:

| Mode | What it does |
|---|---|
| `shell` | interactive prompt (`aios> `); `quit` or `exit` leaves |
| `exec 'cmd' ['cmd' …]` | run each argument as one line, stop at the first failure (exit 1) |
| `run FILE.aos` | run a script line by line, stop at the first unexpected failure |
| `attend [SECONDS]` | stay up and carry out standing directives; extra arguments are run first |
| `info` | the `info` command, then exit |
| `models` | list the models reachable from this host right now (see below) |
| `desktop` / `gui` | the graphical desktop (needs a display, or `--headless`) |
| `canvas-dump WxH f.ppm [frames]` | paint the desktop canvas once to a file |
| anything else | treated as a single command line: `aios --root R whoami` is `exec whoami` |

### One-shot: `exec`

```
$ AIOS_NO_ACCEL=1 build/aios --root /tmp/aios-docs-console/root exec info whoami
System Overview
  Kernel ID       : bde2bf6ed3f97d6f000154546671a986
  Console ID      : 0e17c7e8555ad02c000154546701a986  (console entity)
  Root            : /tmp/aios-docs-console/root
  Uptime          : 59.45 ms
  Host            : x86_64 · arch-qbc · 4 cpus · 19.49 GiB · 1 NUMA node(s)

Substrate & Fabric Resources
  Memory Fabric   : 0 objects, 0 B (system 0 B, device 0 B, …), 0 migrations
  Entities        : 3 active (states: 1, channels: 0, graphs: 0, capabilities: 25, store objects: 0)
  …
  Accelerators    : accelerators disabled (AIOS_NO_ACCEL)
you are entity 0e17c7e8555ad02c000154546701a986 (console); the kernel bde2bf6ed3f9 is another entity
```

A failing line ends `exec` with exit status 1:

```
$ build/aios --root R exec 'which ls frob'
ls: shell builtin
frob: not found
FAILED: not-found
```

### Interactive: `shell`

```
$ build/aios --root /tmp/aios-docs-console/fresh-root shell
Kernel a66a1940b8c0 · console entity 165ec1c5c75e · root /tmp/aios-docs-console/fresh-root · type help or ask a question directly
aios> whoami
you are entity 165ec1c5c75e87e6000750a23e7f160a (console); the kernel a66a1940b8c0 is another entity
aios> export GREETING=hi
aios> echo $GREETING
hi
aios> exit
```

The shell also starts any boot services registered with `daemon`, and runs
due standing directives while it waits for input. It reads from a pipe as well
as a terminal, which is how the resident machine below drives it.

### Scripts: `run`

A `.aos` script is console lines, one per line. Blank lines and lines starting
with `#` are skipped. Each line is echoed before it runs unless you pass `-q`.

```
# demo.aos
entity create worker => w
cap grant $w memory * read,write => c
# a leading ! means this line is expected to fail
!cap grant $w kernel * destroy
echo worker is $w
```

```
$ build/aios --root R -q run demo.aos
entity 333245355f585341000dac9b277250c8 (worker)
capability 2ac8e454dbeca0ae000dac9b27c250c8 (derived from the console's authority)
  refused as expected: denied: the kernel refuses: console 0e17c7e8555a holds read,execute,grant,schedule,checkpoint over kernel:*; destroy is not among them — an entity cannot grant what it does not hold
worker is 333245355f585341000dac9b277250c8
demo.aos: 4 commands, 0 unexpected failure(s)
```

The count is the script's own lines. The lines the binary runs itself at every
start (`jepa reboot-resume`, the `AIOS_FLEET_HUB` join)
are not counted and do not appear in `history`. The exit status follows the
summary line: `run` exits 1 whenever it counted an unexpected failure, not only
when a line stopped the script.

The repository's own operator drive is a good longer example. Its plugin minds
are named by relative path (`./mind_reflect.so`); a relative plugin or program
path that is not found from the working directory is looked for beside the
running binary, so the drive runs from anywhere:

```
$ cd /tmp && /repos/aios/build/aios --root /tmp/aios-fix-cli/r8g run /repos/aios/scripts/drive-kernel.aos
…
/repos/aios/scripts/drive-kernel.aos:135 › mind bind $mind plugin ./mind_reflect.so out=$out
bound plugin:reflect to 94733eebec90 publishing to ad3c95b41c5f
…
/repos/aios/scripts/drive-kernel.aos: 165 commands, 0 unexpected failure(s)
```

Colour (the dim echo of each line, red failures) is written only when stdout is
a terminal, and not even then with `NO_COLOR` set (any value) or `TERM=dumb`.
Piped into a file or a log, the output is plain text.

### Headless desktop

The desktop is outside the scope of this page, but it can be checked with no
display:

```
$ build/aios --root R desktop --headless --frames 3 --dump /tmp/aios-docs-console/desk.ppm
headless: 3 frame(s) at 1280x800 -> /tmp/aios-docs-console/desk.ppm
```

## What a root is

A root is a directory that *is* the machine's persistent state. Point two
processes at the same root and they are the same machine (same kernel ID, same
console entity, same files). Point at a new directory and you get a new
kernel identity.

A fresh root after a few commands:

```
$ ls /tmp/aios-docs-console/fresh-root
authority.log  fs  kernel.identity  language-model.bin  store
```

| Path | Contents |
|---|---|
| `kernel.identity` | the kernel's ID; this is what makes the root the same machine next time |
| `authority.log` | every grant, request, admission and refusal, kept across restarts |
| `store/` | the object store (records, conversation memory, dream records) |
| `fs/` | the shell's file tree: `/` in the shell is `<root>/fs` |
| `language-model.bin` | the machine's own small language model (see [unknown lines](#lines-that-are-not-commands)) |
| `checkpoints/<entity>/N.aoc` | entity checkpoints written by `checkpoint` |
| `directives/`, `run/`, `sql_db/`, `mail/`, `contacts/`, `calendar/`, `tasks/`, `ssh/`, `repos/`, `organs/`, `mind.key` | appear when the subsystem that owns them is first used |

The console and the resident `mind` entity are *residents*: they persist
across boots. Entities you create with `entity create` do not. They are gone
the next time a process opens the root, unless you restore them from a
checkpoint:

```
$ build/aios --root R exec 'entity create scratch => e' 'checkpoint $e' 'entity terminate $e' 'restore $path' 'entity show $e'
entity 4a76ef11e6003d820006045399335ce4 (scratch)
checkpoint /tmp/aios-docs-console/root/checkpoints/4a76ef11e6003d820006045399335ce4/1.aoc
terminated 4a76ef11e600 (memory released, capabilities revoked, channels destroyed)
restored entity 4a76ef11e6003d820006045399335ce4 (scratch) states=0 memory=0 caps=4 restores=1
4a76ef11e600 (4a76ef11e6003d820006045399335ce4)
  label=scratch status=active gen=7 states=0 memory=0 caps=4 checkpoints=1 restores=1
    parent → 0e17c7e8555a
```

`rewind <path>` is the other way back from a checkpoint.

## Line syntax

These rules apply to every line, whether you type it, pass it to `exec`, or
put it in a script:

- `$NAME` expands a session variable. `export N=V` sets one, `unset N`
  removes it, `env` lists them.
- `… => NAME` binds whatever the command produced (an entity ID, a capability
  ID, a checkpoint path) to `$NAME`. `set NAME` binds the last produced value.
- A leading `!` means the line must fail. If it is refused, you get
  `refused as expected: …` and the script carries on. If it succeeds, that
  counts as a failure.
- Some commands set a variable of their own. `checkpoint` sets `$path`, for
  example.
- Labels work where an entity is expected: `mind bind mind echo` and
  `directive add … as=console` both resolve the label to its entity.

## The familiar shell

`man` with no argument lists them:

```
$ build/aios --root R exec man
The Familiar Shell (mapped onto AOS; storage authority applies):
  pwd · cd · ls [-l] · cat · edit <file> <old> <new> · echo [>|>> file] · touch · mkdir [-p] · rm [-r] · rmdir · mv · cp · tree · stat · find · grep · wc · head · tail
  ps (entities) · free (memory fabric) · uname [-a] · hostname · whoami · id · date · uptime · nproc · lsblk · df · dmesg · env · export N=V · unset · clear · history · man <cmd> · which
```

`help` lists the kernel commands, grouped (system, authority, entities and
memory, compute, channels and store, intelligence, applications and shell).
`help <cmd>` shows one command in full: its aliases and every subcommand, with
its usage and what it does. The text comes from `cli/command_docs.cpp`, and
docs/reference.md is generated from the same table.

The file commands act on the root's own tree (`<root>/fs`). `/host/...` is the
substrate's filesystem as this machine sees it. Reads need storage `read`,
writes need storage `write`, and `ps`/`free`/`uname`/... need kernel `read`.
All of this is checked against the console entity's capabilities.

```
$ build/aios --root R exec 'mkdir /notes' 'echo hello from the console > /notes/a.txt' \
    'ls -l /notes' 'cat /notes/a.txt' 'cd /notes' 'pwd' 'wc a.txt' 'grep hello a.txt' 'tree /' 'stat /notes/a.txt'
- 23        a.txt
hello from the console
/notes
1     4      23       a.txt
hello from the console
/
`-- notes/
    `-- a.txt
/notes/a.txt  file  23 bytes

$ build/aios --root R exec 'edit /c.txt hello goodbye' 'cat /c.txt'
edited /c.txt (1 replacement)
goodbye from the console
```

Looking at the machine:

```
$ build/aios --root R exec ps free 'uname -a' date uptime nproc df dmesg
ENTITY ID     STATUS      LABEL               MIND
0e17c7e8555a  active      console             -
9e180e63debc  active      mind                echo
bde2bf6ed3f9  active      kernel              -
MEMORY        TOTAL         USED
System RAM    19.5G         0
Fabric: 0 objects across system/device/shared/persistent/remote, 0 migrations
AIOS arch-qbc bde2bf6ed3f9 x86_64 4-cpu AIOS
2026-10-01 21:57:32 UTC
up 0.0s, 3 entities
4
the AOS persistent root is at /tmp/aios-docs-console/root; use `host disks` / `host volume` for the underlying store
AOS keeps no ring log; `info` is the live state and the object store is the history
```

`ps` lists entities, not host processes. `free` reports the memory fabric.
`lsblk`/`df` point at `storage`, which lists real volumes.

`head`/`tail` take the count as `-n N`, `-nN` or `-N`. `mkdir` needs the
parent to exist and the directory not to; `mkdir -p` creates parents and
accepts a directory that is already there. `find [path] -name text` keeps the
paths whose last component contains text.

`grep [-E|-F] [-i] [-v] [-n] <pattern> <file...>` reads the pattern as a POSIX
basic regular expression (`-E` extended, `-F` a literal string). No line
selected is exit status 1, and a pattern that does not compile is refused:

```
$ build/aios --root R exec 'mkdir -p /w/a/b' 'echo first line > /w/log.txt' 'echo Second Line >> /w/log.txt' \
    'echo third 42 >> /w/log.txt' 'grep -i -n LINE /w/log.txt' 'grep ^th.*2$ /w/log.txt' 'grep -E [0-9]+ /w/log.txt'
1:first line
2:Second Line
third 42
third 42
$ build/aios --root R exec 'grep -E (unclosed /w/log.txt'
FAILED: invalid: grep: invalid regular expression '(unclosed': Mismatched '(' and ')' in regular expression
```

`man <cmd>` describes a familiar command as this shell implements it, and for
a kernel command (or any alias of one) prints the same as `help <cmd>`; `which`
says whether a word is a familiar command:

```
$ build/aios --root R exec 'man grep' 'man show' 'man poweroff'
grep [-E|-F] [-i] [-v] [-n] <pattern> <file...>: lines matching a basic regular expression (-E extended, -F a literal string); exit status 1 when no line is selected
display show|set|status|list    What is on the glass
reboot | poweroff               Ask the substrate to restart or power off; refused with a reason if it will not
```

## Entities, capabilities and authority

The console is itself an entity with a fixed authority profile. It has kernel
*access* (read, execute, checkpoint, schedule, grant), but kernel *ownership*
stays with the kernel entity. `authority mine` shows exactly what it holds:

```
$ build/aios --root R exec 'authority mine'
entity 0e17c7e8555ad02c000154546701a986 (console)  — resident: persists across boots
e6fa6709d9d6  holder=0e17c7e8555a  channel:*   grant,destroy,publish,consume,allocate  from=d35828015119
c184fa755bb6  holder=0e17c7e8555a  entity:*    read,execute,grant,destroy,allocate  from=96c9dc8e5074
8535eec25a21  holder=0e17c7e8555a  state:*     read,write,grant,destroy,allocate  from=4a66d31b56ef
6fdc7c2fd400  holder=0e17c7e8555a  kernel:*    read,execute,grant,schedule,checkpoint  from=ac2f49a2d595
234d70bcdb7b  holder=0e17c7e8555a  memory:*    read,write,grant,destroy,allocate  scope=0 B/4.00 GiB  from=1fab35b452ae
78a29d4f589c  holder=0e17c7e8555a  compute:*   execute,grant,schedule  from=7193b3fbd254
3c5391dc5ff7  holder=0e17c7e8555a  network:*   grant,publish,consume  from=e8be5ad5d73a
da6b1b96d841  holder=0e17c7e8555a  storage:*   read,write,grant  from=3500a648d0cf
not held over kernel:*: write,destroy,publish,consume,allocate  — kernel ownership stays with the kernel entity bde2bf6ed3f9
```

### Commands

| Command | |
|---|---|
| `entity create <label> [parent]` | new entity, child of the console |
| `entity list` · `entity show <e>` | |
| `entity relate <a> <b> <kind>` · `entity terminate <e>` | |
| `cap grant <e> <kind> <id\|*> <rights> [ttl_s] [scope_bytes]` | derive a capability from the console's own |
| `cap request <kind> <id\|*> <rights> [ttl_s]` | ask the kernel for authority for the console |
| `cap delegate <e> <cap> <to> <rights> [ttl_s]` · `cap revoke <cap>` | revoke also removes everything delegated from it |
| `cap list [e]` · `cap mine` · `cap find <e> <kind> [id]` · `cap check <e> <kind> <id\|*> <right>` | |
| `authority mine` · `authority residents` · `authority log [n]` · `authority refusals` | |

Resource kinds: `memory compute channel storage entity network state kernel`.
Rights are comma-separated: `read,write,execute,allocate,destroy,grant,publish,consume,schedule,checkpoint`.

### Granting, and being refused

```
$ build/aios --root R exec 'entity create worker => w' \
    'cap grant $w memory * read,write 60 1048576 => c1' 'cap list $w' 'cap check $w memory * read'
entity 4f9a27a09423297b0004467ca570c871 (worker)
capability 64ac86c019a8bdd40004467ca5c0c871 (derived from the console's authority)
CAP ID        HOLDER        RESOURCE              RIGHTS                DETAILS
c6f43886995d  4f9a27a09423  storage:*             read
830534139bc8  4f9a27a09423  state:*               read,write,destroy,allocate
64ac86c019a8  4f9a27a09423  memory:*              read,write              scope=0 B/1.00 MiB  expires=+59.999 s  from=234d70bcdb7b
ed52adea1b7e  4f9a27a09423  memory:*              allocate                scope=0 B/1.00 GiB
213db25d1a03  4f9a27a09423  channel:*             allocate
permitted
```

An entity cannot grant what it does not hold, and kernel ownership is never
granted:

```
$ build/aios --root R exec 'entity create helper => h' 'cap grant $h kernel * destroy'
entity 0b64f608e23698c8000a151f66e3295b (helper)
FAILED: denied: the kernel refuses: console 0e17c7e8555a holds read,execute,grant,schedule,checkpoint over kernel:*; destroy is not among them — an entity cannot grant what it does not hold

$ build/aios --root R exec 'cap request kernel * write'
FAILED: denied: the machine refuses: kernel ownership (write) is never granted — the kernel entity alone holds it
```

Refusals are kept in `authority.log`, so a later process on the same root can
read them:

```
$ build/aios --root R exec 'authority refusals'
REFUSED actor=0e17c7e8555a wanted kernel:* destroy for 0b64f608e236  the kernel refuses: …
REFUSED actor=0e17c7e8555a wanted kernel:* write for 0e17c7e8555a  the machine refuses: kernel ownership (write) is never granted — the kernel entity alone holds it
```

`authority log [n]` shows the last *n* events of every kind (`derived`,
`requested`, `admitted`, `refused`, …):

```
derived    actor=0e17c7e8555a subject=4f9a27a09423 memory:* read,write cap=64ac86c019a8  granted from 234d70bcdb7b
requested  actor=0e17c7e8555a subject=0e17c7e8555a memory:* read
admitted   actor=0e17c7e8555a subject=0e17c7e8555a memory:* read cap=234d70bcdb7b  already held
```

## Standing directives and `attend`

A directive is a line the machine runs on its own, on an interval, as a named
entity:

```
directive add <name> <every> as=<entity> <command…>     every: 30s, 5m, 1h, 1d or once
directive list | remove <name> | enable <name> | disable <name> | run [name]
```

```
$ build/aios --root R exec 'directive add tick 5s as=console echo tick > /tick.log' 'directive list'
[DIRECTIVE] tick — every 5s, as 0e17c7e8555a
  echo tick > /tick.log
  it runs whenever this machine is idle; nothing has to be typed again
Standing directives (register state 6409fa8999e4, adopted from this kernel)
  NAME            EVERY   RUNS  NEXT       AS            LAST
  tick            5s      0     due        0e17c7e8555a
      echo tick > /tick.log

$ build/aios --root R attend 3
attending — kernel bde2bf6ed3f9, console 0e17c7e8555a, for 3s
[tick]   (0 ms, as 0e17c7e8555a)
attended for 3s
```

Directives run while `shell` waits for input, during `attend`, and in the
resident machine. `attend 0 "<cmd>" …` runs the given lines first and then
attends until stopped.

## The resident machine

`scripts/aos.sh` keeps one machine running on this host. The machine is
`aios --root $AOS_ROOT shell`, started by `scripts/aos-session.sh`, which
reads its input from a named pipe and writes everything it says to a log.
`aos.sh` writes to that pipe and reads the log back.

```
$ scripts/aos.sh help
aos — the front door to a resident AIOS on a hosted substrate (macOS or
Linux). The bare-metal machine has metal/aos-vm.sh; this drives the machine
that runs directly on this host.

  aos up                     start the machine (through launchd/systemd once installed)
  aos down                   stop it
  aos restart                stop, start, same root
  aos status                 what is running, which root, which mind, uptime
  aos send "<line>"          type one line, print what the machine said
  aos ask "<question>"       ask the resident mind and wait for the answer
  aos console                attach: what you type goes to the machine
  aos think "<job>"          hand the mind a long job without waiting (watch: aos log)
  aos log [n]                the last n lines the machine said (default 40)
  aos mind <provider> <model>   set the resident mind, then restart
  aos mind off               no resident mind
  aos root <path>            which root to run on (its identity lives there)
  aos build                  configure and build the binary this front door runs
  aos install [--dry-run]    keep it alive and start it at login (launchd on
                             macOS, a systemd --user unit on Linux)
  aos uninstall              stop supervising it
  aos config                 print the settings file
  aos desktop [--tui]        boot the full native OS graphical workstation desktop
  aos gui                    alias for aos desktop

Settings live in ~/.config/aos.conf: AOS_ROOT AOS_RUN AOS_BIN AOS_PROVIDER
AOS_MODEL AOS_PROVIDERS AOS_PATH AOS_BRIDGE. On Linux the unit is
$XDG_CONFIG_HOME/systemd/user/$AOS_UNIT.service (AOS_UNIT defaults to aos).
```

### Settings and state

| | Default | Override |
|---|---|---|
| settings file | `~/.config/aos.conf` | `AOS_CONF` |
| root | `~/aos/root` | `AOS_ROOT` in the settings file, or `aos root <path>` |
| run dir | `~/aos/run` | `AOS_RUN` |
| input pipe | `$AOS_RUN/in` | |
| everything the machine said | `$AOS_RUN/session.log` | |
| launcher errors | `$AOS_RUN/session.err` (launchd: `launchd.out`, `launchd.err`; systemd: `systemd.out`, `systemd.err`) | |

The settings file holds `AOS_ROOT AOS_RUN AOS_BIN AOS_PROVIDER AOS_MODEL
AOS_PROVIDERS AOS_PATH AOS_BRIDGE`. Every `aos` subcommand that saves rewrites the file
with exactly those keys. `AOS_BRIDGE`, when it names a file, is the bridge
`aos-session.sh` hands the machine (ahead of the one beside the source).

```
$ AOS_CONF=/tmp/aios-docs-console/aos.conf scripts/aos.sh root /tmp/aios-docs-console/resident-root
root: /tmp/aios-docs-console/resident-root (a new root means a new kernel identity)
$ AOS_CONF=/tmp/aios-docs-console/aos.conf scripts/aos.sh config
AOS_ROOT=/tmp/aios-docs-console/resident-root
AOS_RUN=/tmp/aios-docs-console/home/aos/run
AOS_BIN=''
AOS_PROVIDER=''
AOS_MODEL=''
AOS_PROVIDERS=''
AOS_PATH=''
AOS_BRIDGE=''
```

`aos build` runs `cmake -S . -B build` and `cmake --build build`, then records
`build/aios` as `AOS_BIN`. With no `AOS_BIN`, the script uses `build/aios` or
`build-mac/aios` beside the source.

### Up, send, log, down

```
$ scripts/aos.sh up
up (pid 491758) · root /tmp/aios-docs-console/resident-root · mind none

$ scripts/aos.sh status
root       /tmp/aios-docs-console/resident-root
binary     /repos/aios/build/aios
mind       none
supervised no (run: aos install)
state      up (pid 491758)
           System Overview
             Kernel ID       : 158d97ffa0873f1c000cb890b4ff4e55
             Console ID      : 6582df752a25805f000cb890b58f4e55  (console entity)
             Root            : /tmp/aios-docs-console/resident-root
             …

$ scripts/aos.sh send whoami
you are entity 6582df752a25805f000cb890b58f4e55 (console); the kernel 158d97ffa087 is another entity

$ scripts/aos.sh send 'ls /'
(empty)
this is the machine's own tree at /tmp/aios-docs-console/resident-root/fs, and nothing has been written into it yet
  /host          the substrate's filesystem, as this machine sees it
  git repos      the repositories this kernel holds (under /tmp/aios-docs-console/resident-root/repos)
```

`send` waits up to 60 seconds for the line's output. For longer work, raise
the limit with `AOS_SEND_WAIT=<seconds>`. Only one exchange runs at a time,
enforced by a lock directory at `$AOS_RUN/exchange.lock`.

`restart` brings the machine back on the same root, so it keeps the same
identity:

```
$ scripts/aos.sh restart
down
up (pid 492664) · root /tmp/aios-docs-console/resident-root · mind none
$ scripts/aos.sh send whoami
you are entity 6582df752a25805f000cb890b58f4e55 (console); the kernel 158d97ffa087 is another entity
$ scripts/aos.sh down
down
```

`session.log` marks each start, so the log shows where every restart happened:

```
=== session start 2026-10-01 17:59:28 · root /tmp/aios-docs-console/resident-root · mind none/none
Kernel 158d97ffa087 · console entity 6582df752a25 · root /tmp/aios-docs-console/resident-root · type help or ask a question directly
```

`aos console` tails the log and writes what you type into the pipe. Ctrl-C
leaves without stopping the machine. `aos desktop [--tui]` starts the machine
if it is down, then runs the desktop on the same root.

### Supervising it

**macOS (launchd).** `aos install` writes
`~/Library/LaunchAgents/com.qbcsys.aios.plist` with `RunAtLoad` and
`KeepAlive` set, records your interactive `PATH` as `AOS_PATH` (so the bridge
can find `node`), and bootstraps it. After that, `up`/`down` load and unload
the agent, and `aos uninstall` removes it. The root is left alone. This was not
driven for this page (no macOS host).

**Linux (systemd --user).** `aos install` writes
`${XDG_CONFIG_HOME:-~/.config}/systemd/user/aos.service` (`Restart=always`,
`WantedBy=default.target`, `AOS_CONF` named outright), records `AOS_PATH`,
then runs `systemctl --user daemon-reload` and `enable --now`. `up`/`down` then
go through `systemctl --user start`/`stop` (so `down` is not undone by
`Restart=always`), `status` reports the unit and its state, and `aos uninstall`
disables and removes it. `AOS_UNIT` changes the unit name, so a second machine
(or a test) can be supervised beside the first. `aos install --dry-run` prints
the unit and the commands without installing anything (`AOS_UNIT_OUT=<file>`
writes it to a file instead, for `systemd-analyze --user verify`). To start it
at boot without a login session, `loginctl enable-linger $USER`.

Driven on arch with a scratch unit name (2026-10-01):

```
$ AOS_UNIT=aos-fixcli-test AOS_CONF=/tmp/aios-fix-cli/c2.conf scripts/aos.sh install
Created symlink ~/.config/systemd/user/default.target.wants/aos-fixcli-test.service → ~/.config/systemd/user/aos-fixcli-test.service.
installed and up (pid 588197) · ~/.config/systemd/user/aos-fixcli-test.service · restarts on exit and at login
$ … status
supervised yes (~/.config/systemd/user/aos-fixcli-test.service, active, restarts on exit and at login)
state      up (pid 588197)
$ kill 588197; sleep 4; … status
state      up (pid 588480)                       # systemd brought it back
$ … down        → down, unit inactive
$ … uninstall   → no longer supervised (unit file and wants link gone)
```

Run `aos build` (or set `AOS_BIN`) first: `aos-session.sh` exits with status
78 when `AOS_BIN` is not set to an executable.

## The intelligence slot (optional)

The resident entity labelled `mind` is the slot. Whatever is bound there
answers `ask`, writes `dream`s and grows `organ`s. **These three need a real
model. Nothing else on this page does.**

At boot the console binds the first of these that works:
1. `AIOS_MIND_KIND=qwen` (or `AIOS_MIND_URL` set): an OpenAI-shaped HTTP endpoint.
2. `AIOS_MIND_KIND=plugin` with `AIOS_MIND_PATH`: a native plugin.
3. `wire`: a model attached on a serial line (bare metal).
4. The assay bridge, `node intelligence/assay/bridge.cjs`. It is found via
   `AIOS_BRIDGE`, then `AOS_BRIDGE` in the settings file, then relative to the
   working directory or the root.
5. `echo`: a reference that computes nothing and hands its input back.

So the slot is never empty. Its contents depend on where you start from. A
console started outside the source tree gets `echo`. The resident machine
(`aos-session.sh` changes into the tree and sets `AIOS_BRIDGE`) gets the bridge,
which with no model configured is unusable:

```
$ build/aios --root R exec 'mind status'            # started from /tmp
AIOS resident mind
  Entity        : 9e180e63debc
  Bound engine  : echo  (0 cycle(s) run)
  Configured    : see AOS_PROVIDER / AOS_MODEL in the machine's aos.conf
  Change it     : mind set <provider> [model]   e.g. mind set claude-cli sonnet

$ scripts/aos.sh send 'mind status'                 # resident machine
  Bound engine  : external:assay:unconfigured/?  (0 cycle(s) run)
```

### `mind`

```
mind status
mind set <provider> [model]          writes AOS_PROVIDER/AOS_MODEL into aos.conf and hot-swaps
mind provider <p> | mind model <m>
mind bind <e> echo | wire | plugin <path> | extern <cmd…> | qwen [url] [model] | jepa   [out=<channel>]
mind swap <e> …                      same kinds, replacing what is bound
mind cycle <e> [n] | mind unbind <e>
```

```
$ build/aios --root R exec 'mind bind mind echo'
bound echo to 9e180e63debc
```

For `qwen` (also `net`, `ollama`, `llama`, …) with no URL, the console asks
the endpoints it knows which one serves the model. An API key is read from
`<root>/mind.key` and never taken from the command line.

`aios models` (a mode, not a console command) lists what is reachable right
now:

```
$ build/aios --root R models
models this machine can reach right now:
  gemma3:4b  4.3B  3.1 GiB  via ollama on this machine
  llava:7b  7B  4.4 GiB  via ollama on this machine
  spark  via openai on devops  [resident]
  …
```

With the resident machine, `aos mind <provider> <model>` writes the settings
file and restarts. `aos mind off` clears the setting.

### `ask`

```
ask [show=1] [cycles=N] [mind=<label>] <request…>
```

The mind replies with `run: <command>` lines, which the console runs *as the
mind entity*, with the mind's own authority. It finishes with an
`answer: …` line. The default limit is 12 cycles. `show=1` (or
`AIOS_TRACE=1`) shows each step. Each finished exchange is stored in the
object store as `conversation_memory`.

With no model bound, `ask` does not give an answer. The `echo` reference hands
its input (the briefing) straight back, so `ask` refuses rather than acting on
the briefing's own template line or storing it as a memory:

```
$ build/aios --root R exec 'mind bind mind echo' 'ask hello'
bound echo to e36a6bcd927e
…
FAILED: unsupported: the resident 'mind' is bound to the echo reference, which computes nothing and handed the 7488-byte briefing back; bind a model to ask it anything (mind set <provider> [model], or mind bind mind qwen|plugin|extern …)
```

With the bridge and nothing configured it fails outright:

```
error: io-error: the mind failed during cycle 1: think: peer error: no model configured: ~/.assay/config.json or AIOS_MODEL_PROVIDER/AIOS_MODEL/AIOS_MODEL_HOST/AIOS_MODEL_KEY
```

`aos ask "<question>"` is `aos send "ask <question>"`.

### Lines that are not commands

A line the dispatcher does not recognise is not simply rejected. First the
machine's own small language model (`lm` shows it; it lives in
`<root>/language-model.bin`) tries to read it as a command. If it is confident
(under 1.0 nats/token), it runs that command. Otherwise the line goes to the
bound mind as an `ask`:

```
$ scripts/aos.sh send frobnicate-xyz
this machine's own model is not confident (5.53593 nats/token for "display show three pyramids in the desert"), so this goes to the bound mind
…
error: io-error: the mind failed during cycle 1: think: peer error: no model configured: …
```

With no model to hand it to (nothing bound but `echo`), the line is what it
looks like, an unknown command:

```
$ build/aios --root R exec 'frobnicate the widgets'          # started outside the tree: echo
FAILED: not-found: unknown command: frobnicate — `help` lists the commands; no model is bound to answer free text (mind set <provider> [model])
```

With the bridge bound it goes to the bridge as before (and with no model
configured behind it, the bridge's own error comes back).

### `dream`, `organ`, `face`

| Command | Needs a model? |
|---|---|
| `dream <what you want>` | yes: the mind writes the face, the machine looks at it, corrects it, and puts it on the glass |
| `dream list` · `kinds` · `show` · `face`/`source` · `look` · `open` · `revisit` · `remove <name>` | only `revisit` (it re-runs the dream) |
| `organ grow <name> <what it must do>` | yes, plus `c++` on the host: the mind writes C++, the machine builds it with `c++ -std=c++20 -O2 -shared -fPIC` and loads it |
| `organ list` · `call <name> <request>` · `source <name>` · `remove <name>` | no |
| `face reference` · `look` · `text` · `source` · `open <name\|file>` | no |

```
$ build/aios --root R exec 'dream list' 'dream kinds' 'organ list'
this machine has not dreamed yet — `dream <what you want>`
Kinds of records dreams keep on this machine
  conversation_memory  x4  kept by this machine itself  fields: intelligence crypto.hash text  (plus _id, _t, _kind on every record)
no organs grown yet — `organ grow <name> <what it must do>`
```

A face (an app written in the machine's own face language) can be written by
hand and checked with no model. `face reference` prints the language
(63 lines). Here is a small one:

```
face counter
title "Counter"
state n = 0
on bump: inc n
column gap=10
  text "count {n}" size=20
  button "Add one" action=bump
```

```
$ build/aios --root R exec "face look /tmp/aios-docs-console/counter.aos action:bump action:bump"
face: counter  title: Counter
laid out 4 thing(s) over 12 frame(s) on a 1880x940 pane
looked at the glass: every word understood, nothing cut, nothing off the pane, every glyph drawable
the face, as text:
    Counter
    count 2
                                                       Add one
```

Packaged apps and the face language in depth are in [apps.md](apps.md).

## Command groups

This is every first word the dispatcher (`run_line` in `cli/commands.cpp`)
accepts, with its aliases. Run one with no arguments to get its usage line.

**Machine and session**

| Command | |
|---|---|
| `help` · `man` | kernel commands · familiar shell commands |
| `info` · `sysstat` · `subsystems` (`tasks`) | identity, topology, fabric; health; what the machine can do, asked live |
| `whoami` (`id`) · `identity` | the console entity · what this machine says it is |
| `devices` · `storage` · `network` · `bus` (`pci`) · `attest` (`tpm`) | compute devices, volumes, interfaces, PCI, root of trust |
| `daemon` · `watchdog` · `quota` | userland services · self-healing watchdog · per-entity compute budgets |
| `directive` · `sleep <s>` · `set` · `echo` · `expect <text>` | standing orders · pause · bind last value · print · assert on the last read |
| `checkpoint` · `restore` · `rewind` | entity snapshots |
| `build [test\|gate\|args…]` · `host <cmd…>` | run the host build / a host command (needs kernel execute) |
| `reboot` (`restart`) · `poweroff` (`shutdown`, `halt`) | ask the substrate; refused with a reason if it will not |

**Authority, entities, memory**: `authority`, `cap`, `entity`, `mem`, `state`,
`kv`, `channel`, `store`. These are covered above or in `help`. `kv` is held in
state objects inside the current process, so a later `exec` does not see it.

**Compute and models**, see [compute.md](compute.md)

| Command | |
|---|---|
| `compute` | compute kernels and `compute serve` |
| `tensor` · `graph` · `sat` | typed tensors, compute graphs, the SAT solver |
| `jepa` (`world-model`) · `lm` (`language`) | the substrate world model · the machine's own small language model |
| `grain` | the machine's own language (`grain eval 1+2` → `3`) |
| `mind` · `qwen` (`llm`, `spark`) · `ask` · `dream` · `organ` · `face` | the intelligence slot (above) |

**Database**, see [asql.md](asql.md): `sql` (`asql`, `mariadb`, `db`).

**Network and fleet**, see [network.md](network.md)

| Command | |
|---|---|
| `net check\|repair\|correct` | the network self-check |
| `node` · `fleet` · `p2p` (`dht`, `mesh`) · `gateway` | peer kernels, fleet load and membership, discovery, the HTTP gateway |
| `ssh` · `git` | native SSH-2 client · native git, including clone/fetch/push over ssh:// and git:// |
| `npm` · `pip` (`pip3`, `python`) · `cargo` (`crates`) · `go` (`gomod`) | packages over the kernel's own TLS |
| `browser` (`unblink`) · `sense` (`look`) · `feed` (`rss`) · `horizon` | the built-in web engine, reading pages, feeds, watched sources |
| `chain` · `wallet` · `contract` (`solve`) · `btc` | Bitcoin/Monero peers and wallet. Running `chain` or `contract` with no arguments connects to public peers |
| `ingress` (`receive`, `upload`) | payloads received over the network |

**Applications and display**, see [apps.md](apps.md)

| Command | |
|---|---|
| `app` (`apps`) · `js` (`javascript`) | installed and packaged apps · JavaScript |
| `mail` · `contacts` · `calendar` (`cal`, `agenda`) · `task` (`taskledger`) | personal data apps under the root |
| `display` (`screen`, `show`) · `scene` · `keys` (`keybindings`, `keymap`) | what is on the glass · desktop scene · key bindings |
| `gui` (`ui`, `console`, `desktop`) | desktop info; `gui desktop [--tui]` runs it |
| `voice` (`speech`, `talk`, `listen`) | speech in and out, if the host has engines |
| `tx` (`transaction`) · `patch` · `edit` | atomic multi-file patch transactions · in-place edit |
| `elf <path>` | what this machine would load to run a program |

**Bare metal**, see [metal.md](metal.md): `install` (`dualboot`), `host`,
`reboot`, `poweroff`.

## Environment variables

| Variable | Read by | Effect |
|---|---|---|
| `AIOS_ROOT` | `aios` | default for `--root` |
| `AIOS_NO_ACCEL=1` | kernel | skip GPU bring-up (faster start; `devices` shows CPU only) |
| `AIOS_TRACE=1` | `ask`, `dream` | show each step of the mind's work |
| `AIOS_HOST_TOOL` | `aios` | binary that `host …` runs; default runs the arguments directly |
| `AIOS_FLEET_HUB`, `AIOS_FLEET_KEY` | `aios` | `fleet join` the hub at every start |
| `AIOS_FLEET_SLOTS` | `aios` | job slots this kernel offers the fleet |
| `AIOS_MIND_KIND`, `AIOS_MIND_URL`, `AIOS_MIND_MODEL`, `AIOS_MIND_PATH` | console | what to bind into the slot at boot |
| `AIOS_BRIDGE` | console | path to `bridge.cjs` |
| `AIOS_MODEL_PROVIDER`, `AIOS_MODEL` | bridge | which model the bridge calls (set from `AOS_PROVIDER`/`AOS_MODEL` by `aos-session.sh`) |
| `AOS_CONF` | `aos.sh`, `aos-session.sh`, console | settings file path |
| `AOS_RUN` | `aos.sh`, `scene` | run directory |
| `AOS_SEND_WAIT` | `aos.sh` | seconds `send`/`ask` wait for an answer |
