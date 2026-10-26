# The desktop and the applications

This page covers the desktop AIOS draws for itself and the applications that ship in the tree: the web pane, git, mail, contacts, calendar, the editor with its transactional multi-file edit, and the package clients for npm, PyPI, crates.io and Go modules.

Everything here is one binary, `build/aios`. You can use each application in two ways: as a pane on the desktop, or as a command group at the command surface (`aios <command> ...`, `aios exec '<line>' ...`, or a line typed into the Shell pane).

The examples below were run on an x86_64 Linux host with the binaries already in `build/`. Each run used a throwaway root under `/tmp/aios-docs-apps/` and `AIOS_NO_ACCEL=1`. Without that variable every CLI run spends about 0.65 s bringing up the GPU. Output is real but trimmed.

```sh
export AIOS_NO_ACCEL=1
A=/repos/aios/build/aios
R=/tmp/aios-docs-apps/root        # any empty directory; --root defaults to $AIOS_ROOT or ./aios-root
```

> **Where state lives.** Every application keeps its data under the root you pass with `--root`: `repos/`, `mail/`, `contacts/`, `calendar/`, `store/`, `run/`. A root is runtime state. Never commit one. `.gitignore` ignores `/aios-root*/`, `/mail/`, `/contacts/` and `/calendar/` at the top of the repository, but a root with any other name inside the tree is not ignored. Keep roots outside the checkout.

---

## 1. The desktop

### Starting it on a host

```sh
$A --root $R desktop                    # fullscreen at the display's real mode
$A --root $R desktop --windowed 1440x900
$A --root $R desktop --tui              # text-mode fallback, no window
```

`gui` is an alias for `desktop` when it is the first argument to `aios`. It opens a real window, so do not run `aios gui` when you only want information. To get the information, run `gui` with no arguments *inside* a session, for example `aios exec gui`.

How the hosted backend starts (`src/userland/compositor.cpp`, `run_desktop_gui`):

- **SDL2** supplies the window and input. If the build has no SDL2, or SDL cannot start, the desktop falls back to the text UI.
- **Vulkan** is tried first. The window is created with `SDL_WINDOW_VULKAN` and `GpuPresenter` (`gpu_present.cpp`) paints the background in a shader. If the GPU path is refused, the window is recreated with a plain SDL renderer. On this host the Vulkan path came up and then failed to make a swapchain:
  ```
  display: NVIDIA GeForce GTX 1080 Ti through Vulkan
  display: io-error: vkCreateSwapchainKHR refused
  ```
- Set `AIOS_NO_GPU_DISPLAY=1` to skip Vulkan. `--dump` skips it too, because the dump has to be the whole frame painted on the CPU.
- F11 (or Ctrl/Super+Shift+F) toggles fullscreen.

On bare metal the same compositor draws into the machine's own framebuffer, with PS/2 or USB input. That path is covered in `docs/metal-desktop/README.md` and is not covered here.

Other desktop flags, from `cli/main.cpp`:

| flag | effect |
|---|---|
| `--size WxH` | frame size; also works with `--headless` |
| `--scene beach\|cyber\|orb`, `--beach`, `--cyber`, `--orb` | the animated background, via `AOS_DESKTOP_SCENE` |
| `--no-panels`, `--collapsed` | hide the start-page panels, or fold them down to their headers |
| `--open URL` | open a URL in the web pane at start |

### Headless rendering

With `--headless` there is no display: the desktop paints into its framebuffer for N frames, applies scripted input at the frames you name, and writes a PPM file.

```sh
$A --root $R desktop --headless --frames 3 --dump /tmp/aios-docs-apps/desk.ppm
# headless: 3 frame(s) at 1280x800 -> /tmp/aios-docs-apps/desk.ppm
python3 -c "from PIL import Image; Image.open('/tmp/aios-docs-apps/desk.ppm').save('/tmp/aios-docs-apps/desk.png')"
```

The default size is 1280x800, and `--size 1920x1080` changes it. `--frames N` sets the length of the run with or without `--dump`; without `--dump` the last frame is written to `/tmp/aos-desktop.ppm`. A `--do` step scheduled past the last frame lengthens the run to reach it, and says so: `headless: a step is scheduled at frame 7; running 8 frames instead of 2`.

Scripted input is `--do FRAME:KIND:ARG`:

| kind | arg | example |
|---|---|---|
| `type` | text | `--do 3:type:"open /tmp/x.cpp"` |
| `key` | `Enter Escape Backspace Tab Delete ArrowUp/Down/Left/Right Home End PageUp PageDown F1`–`F12` | `--do 1:key:F4` |
| `ctrl` | a letter | `--do 2:ctrl:l` |
| `click`, `move`, `down`, `up` | `x,y` | `--do 5:click:526,416` |
| `wheel` | notches, + is up | `--do 6:wheel:-3` |
| `cmd` | a command line, sent to the desktop's own command submit | |
| `draw` | a drawing directive | `--do 2:draw:stroke 10%,20% 40%,60% close` |
| `shot` | a `.png` path, written mid-run | `--do 6:shot:/tmp/a.png` |

To paint only the start-page canvas, with no workspace, and time it, use `canvas-dump`:

```
$ $A --root $R canvas-dump 640x400 /tmp/aios-docs-apps/canvas.ppm 5
canvas 640x400 -> /tmp/aios-docs-apps/canvas.ppm
frames=5  first=76.63ms  min=0.27ms  median=0.33ms  max=76.63ms  mean=16.18ms  -> 3019.2 fps
```

### What you see

The first frame is the start page. Along the top is a bar with **AIOS CORE**, the bound mind, the number of capabilities, CPU/GPU, RAM and the clock. Over the moonlit beach scene sit the panels **Ledger**, **Authority**, **Horizon**, **Standing orders**, **Rewind** and **Identities**. At the bottom are counters and a dock.

The dock icons, left to right (`TASKBAR_APPS`, compositor.cpp:11354), are: Terminal, Editor, MariaDB (the database studio), Web, Mail, Contacts, Calendar, Apps, Studio (the inspector), and Machine.

### Keys

`aios keys` prints the live table, shown here with some rows merged. Bindings are kept in `<root>/run/keys`, which the compositor re-reads twice a second. `keys actions` lists every action a key can be bound to.

```
KEY           ACTION            WHAT IT DOES
F1            pane.shell        raise the shell
F2            pane.machine      raise the machine pane
F3            pane.tasks        raise the tasks pane
F4            pane.editor       raise the editor
F5            pane.database     raise the database studio
F6            pane.web          raise the web pane
F7            pane.mail         raise the mail client
F8            pane.contacts     raise the contacts card manager
F9            pane.calendar     raise the calendar schedule
F10           pane.apps         raise the face applications studio
F11           pane.inspector    raise the universal in-desktop behavioral inspector
Escape        mind.stop         stop whatever the machine is running
Ctrl+R        machine.restart   restart the machine
Ctrl+P        palette.toggle    open or toggle the fuzzy command palette launcher
Ctrl+W        window.close      close the focused window
Space / p / h face.cycle        walk the desk, the folded panels and the bare scene
1 / 2 / 3     scene.*           ocean / cyber / orb
r             machine.power     open the power panel, which asks before it acts
F12 / d       draw.toggle       the drawing surface
s             desktop.screenshot keep what the screen is showing as a png
```

Other keys handled in the compositor:

- In the SDL window, F1 toggles between the start page and the workspace (compositor.cpp:16300).
- Ctrl+Left, Ctrl+Right and Ctrl+Up snap the focused window. Ctrl+1 through Ctrl+0 switch workspaces.
- Alt+Tab or Ctrl+Tab opens the window switcher. A GNOME host takes Alt+Tab for itself, so use Ctrl+Tab there.

Each pane, driven headless (`--do 1:key:F4` and so on) with its frame dumped:

- F1 shows a Shell window with an `info ps devices free tasks reboot clear` chip row.
- F4 shows the Editor.
- F6 shows the Web pane with an empty-state screen.
- F7 shows AetherMail.
- F8 shows Contacts.
- F9 shows Calendar.
- F10 shows Apps.

### Face applications (F10) and the face language

A *face* is a declarative `.aos` document: headers, `let`/`state` bindings, handlers and an indented widget tree. The compositor evaluates and draws it every frame. Four ship in `apps/`: `order_desk.aos`, `task_tracker.aos`, `system_monitor.aos` and `notes.aos`. The F10 pane shows them as tabs.

The pane looks for each face first in `<root>/apps/` (so a machine can override one), then in `$AIOS_APPS_DIR`, then beside the binary (`<exe dir>/apps`, and `<exe dir>/../apps`, which is the repository's `apps/` for `build/aios`). The working directory is not consulted, so the pane works from any directory. If none has the file, the pane says `Error reading face file: apps/x.aos (looked in ...)` with every place it tried.

```sh
$A --root $R face reference              # the full language reference (widgets, statements, builtins)
$A --root $R face text apps/notes.aos    # the face laid out as text
$A --root $R face look apps/notes.aos    # lay it out and check it; fails while every record it reads is empty
```

`face` with no arguments prints its verbs: `reference | look | text | open | source`.

```
$ $A --root $R face look apps/notes.aos
face: notes  title: Notes
laid out 23 thing(s) over 12 frame(s) on a 1880x940 pane
records it read:  note -> 0 row(s)
  - NO RECORDS: this face read note (0) and every one came back empty — nothing on it came from this machine
error: corrupt: the face is not right yet
```

Related commands:

- `organ grow <name> <what it must do>` grows a native helper. On a fresh root `organ` reports `no organs grown yet`. `organs/lsp.cpp` and `organs/sysinfo.cpp` are the in-tree examples.
- `app list` / `app install assay` / `app open <name>` run packaged web apps as supervised services and open their interface in the web pane.

### Scene

`scene beach|cyber|orb` writes the desktop's scene file in the machine's run directory: `$AOS_RUN/scene` if `AOS_RUN` is set, otherwise `<root>/run/scene`, which is where the compositor reads it. `scene` alone prints the current scene and the path. The key-binding table, the browse request and the drawing directives (`keys`, `run/browse`, `run/draw`) follow the same rule, so a console and a desktop started from different directories share them. `sense frame` writes its screenshot to `<run dir>/desktop_now.png`, running `scrot` without a shell.

---

## 2. The web pane (Unblink)

The browse pane is the machine's own engine: HTML tokeniser and DOM (`web_dom.cpp`), CSS cascade (`web_css.cpp`), box layout (`web_layout.cpp`), a JavaScript engine (`src/js/`: parser, bytecode VM, garbage collector, ES5 through ES2022 builtins) bound to the page by `web_js.cpp` and `web_js/` (DOM, events, fetch/XHR, storage, canvas), SVG (`web_svg.cpp`), image decoding (`web_image.cpp`), and the pane itself (`web_pane.cpp`). There is no host browser underneath. HTTPS is spoken by the kernel's own TLS: example.com came back over `TLS 1.3 TLS_CHACHA20_POLY1305_SHA256`.

### From the command surface: `browser` (alias `unblink`)

```
$ $A --root $R browser http://127.0.0.1:13460/
fetching http://127.0.0.1:13460/ …
Docs test page
http://127.0.0.1:13460/  ·  cleartext  ·  15 box(es), 2 link(s), 337px tall
  ·  script: 1 block(s), 4 statement(s), 1 dom write(s)
     console: loaded

   Hello from a local page
    script ran
                                  [image]
     type here             Count
      second page
      example.com

links (browser follow <n>)
  [0]  second page                                   http://127.0.0.1:13460/page2.html
  [1]  example.com                                   https://example.com/

kept as object 47d2b10ea800
```

Each page you load is also kept in the object store as text plus an embedding, so the machine can be asked about it later.

The browser session lasts only as long as one process. Chain verbs in a single `exec`:

```sh
$A --root $R exec 'browser http://127.0.0.1:13460/' 'browser click 240 208' 'browser log' 'browser boxes'
```

| verb | does |
|---|---|
| `browser <url>` | fetch and lay out; `http://` is assumed when there is no scheme |
| `follow <n>` / `back` / `forward` / `reload` / `page` | navigate, or reprint the current page |
| `click <x> <y>` / `tap` | hit-test and dispatch `click` to the page's handlers |
| `type <x> <y> <text>` / `key <x> <y> <Enter\|Escape\|Backspace>` | type into an `<input>`/`<textarea>` or send it a key |
| `eval <js>` / `js` | evaluate in the page's own heap |
| `log` / `console` | what the page's scripts did and logged |
| `net` / `requests` | what the page asked the network for |
| `dom [filter]` / `tree` | the element tree after script has run |
| `boxes` / `layout` | every laid-out box with its element |

A click, driven:

```
at (240, 208): <button> #b
  1 handler(s) registered
  1 handler(s) ran, 1 dom change(s)
  laid out again: 15 box(es)
   Hello from a local page
    clicked 1
```

`AIOS_BROWSER_WIDTH=<px>` lays the page out at a real window width. The default is 836 px, which a page's media queries treat as a phone.

**`eval` takes the rest of the line verbatim.** Everything after `browser eval ` reaches the page exactly as typed, quotes and `=>` included, so no extra quoting is needed:

```sh
$A --root $R exec 'browser http://127.0.0.1:13460/' "browser eval document.getElementById('msg').textContent"
$A --root $R exec 'browser http://127.0.0.1:13460/' "browser eval [1,2,3].map(x => x * 2).join('-')"   # 2-4-6
```

`document.title` reads the page's `<title>`. After `browser type` the page is laid out again, so the text view shows what was typed rather than the field's placeholder.

### On the desktop

Open the pane with F6 or the globe icon, type a URL in the address bar and press Enter. The pane handles scrolling, Backspace for back, and Ctrl+L for the address bar. Headless:

```sh
$A --root $R desktop --headless --frames 14 --open http://127.0.0.1:13460/ \
  --do 5:click:526,416 --do 6:click:526,416 --do 7:click:388,414 --do 8:type:hello \
  --do 10:shot:/tmp/aios-docs-apps/web-click.png --dump /tmp/aios-docs-apps/web-click-end.ppm
```

In the shot, the page rendered with its CSS: a blue heading, a bordered card, and an SVG red square and green circle. After the two clicks the paragraph read `clicked 2`, the input held `hello` with a caret, and the status bar said `typing into <input>`.

The checked-in drive for this pane is `build/aios-web-pane-drive <root> <out-dir> [url ...]`. It types a URL, follows a link, scrolls, goes back, and writes a PPM per step.

---

## 3. Git

`git.cpp` and `git_remote.cpp` implement git in the kernel. Objects are loose (SHA-1, zlib), the index is a DIRC v2 file, and a repository written here passes stock `git fsck`. Each repository is registered with the kernel as state, so a checkpoint carries it.

**Where repositories live.** A bare name such as `git init notes` means `<root>/repos/notes`. A path containing `/`, or beginning with `.` or `~`, is used as given. `root/...` means the machine's root.

Verbs (from the usage line):

```
git init | use | repos | add | commit | log | status | branch [name] | switch <rev> | cat-file | ls-files | fsck
    | clone <url> | fetch | push [url] [ref] [force=1] | ls-remote <url>  [as=$entity]
```

Local work on a scratch repository:

```
$ $A --root $R exec "git init /tmp/aios-docs-apps/scratch-repo" "git add README.txt" "git add main.cpp" \
      "git commit first commit" "git status" "git log" "git branch topic" "git fsck"
[GIT INIT] object database at /tmp/aios-docs-apps/scratch-repo/.git on branch main (loose objects, SHA-1 ids, zlib streams — written by the kernel itself)
Staged 1 path(s) into /tmp/aios-docs-apps/scratch-repo/.git/index (DIRC v2)
  ce013625030ba8dba906f756967f9e9ca394464a  README.txt
[COMMIT] 18f488aaf3c58eea187f2791724e20b5e3405d66 on main (root commit)
On branch main — /tmp/aios-docs-apps/scratch-repo
  nothing to commit, work tree clean
[BRANCH] topic at 18f488aaf3c58eea187f2791724e20b5e3405d66 — `git switch topic` moves onto it
fsck /tmp/aios-docs-apps/scratch-repo/.git: 4 loose object(s) — 1 commit, 1 tree, 2 blob, 0 other
  every object inflated, hashed to its own name
```

Stock `git -C … log --oneline` printed `18f488a first commit`, and stock `git fsck` was clean.

`git add` takes a path relative to the current repository. The current repository is the last one you ran `init`, `use` or `clone` on. `git repos` lists every registered repository.

**Remotes: `git://`, `ssh://`, `user@host:path`, and `https://` / `http://`.**
HTTP(S) is git's smart-HTTP transport (protocol v0, stateless RPC: `GET
info/refs?service=…`, then one `POST` per round), carried by the kernel's own
TLS client and trust store (`<root>/ssl/ca-certificates.crt` first). The dumb
HTTP protocol and HTTP authentication (a username and token) are not supported. Driven against stock `git http-backend` behind a
local TLS server: clone (full and `depth=1`), `ls-remote`, `pull`, and `push`:

```
[GIT CLONE] https://localhost:13568/repo.git into /tmp/aios-fix-net/gitroot/repos/repo
  transport     : smart HTTP to localhost:13568 over TLS 1.3 TLS_AES_256_GCM_SHA384
  certificate   : localhost (trusted via localhost)
  pack          : 935 B, 15 object(s) — 1 delta-encoded, all reconstructed here
[GIT PUSH] /tmp/aios-fix-net/gitroot/repos/repo -> https://localhost:13568/repo.git
  remote says   : unpack ok, refs/heads/main ok
```

The work tree matched a stock `git clone` of the same URL, and `git fsck` was clean.
No credential helper and no HTTP authentication are supported: a server that answers 401 is reported as denied.

A local path also cannot be a remote. Clone and push were driven against a stock `git daemon --enable=receive-pack` on 127.0.0.1:13461:

```
$ $A --root $R git clone git://127.0.0.1:13461/up.git cloned
[GIT CLONE] git://127.0.0.1:13461/up.git into /tmp/aios-docs-apps/root/repos/cloned
  transport     : git daemon at 127.0.0.1:13461
  pack          : 196 B, 3 object(s) — 0 delta-encoded, all reconstructed here
  work tree     : 1 file(s) checked out

$ $A --root $R exec "git use cloned" "git add AIOS.md" "git commit added from aios" "git push"
[GIT PUSH] /tmp/aios-docs-apps/root/repos/cloned -> git://127.0.0.1:13461/up.git
  pack          : 290 B, 3 object(s) written and compressed by this kernel
  ref           : refs/heads/main  de2a3d44391f -> 78581675423e
  remote says   : unpack ok, refs/heads/main ok
```

On the server, stock git then showed `7858167 added from aios` on top of `de2a3d4 upstream commit`.

For SSH, `key=<path>` selects the private key on `clone` and `ls-remote`.

---

## 4. Mail, contacts and calendar

### Mail (`mail`, alias `email`; desktop F7, "AetherMail")

```
mail list [folder] | search [--server] <query> | read <id> | send <to> <subj> <body>
     | account <list|add|preset|set-pass|test|remove|set-default> | sync | vacuum | status | folders | flag | delete | archive
```

**Accounts.** Use `mail account add --email <addr> [--preset icloud|gmail|outlook|fastmail|yahoo|custom] [--name N] [--user U] [--pass P] [--imap host:port] [--smtp host:port]`. If you give no preset, one is chosen from the domain. Each preset has its own servers, all on IMAP 993 and SMTP 587, for example `imap.gmail.com` and `smtp.gmail.com`. Gmail needs an app password.

**Sync is IMAP and sending is SMTP, both through the host's `python3`.** `mail_client.cpp` writes a Python worker (`<root>/mail/tmp/mail_sync.py`, which uses `imaplib`) and runs it with `popen`. Sending runs an inline `smtplib` script. Port 465 uses implicit TLS. Any other port uses STARTTLS. Mail therefore needs `python3` on the host and does not work on bare metal.

The local index keeps envelopes indefinitely. Bodies are cached for 90 days, up to 250 MB (`mail vacuum --days N --cap MB`). `search --server` sends the query to the server; on Gmail that is X-GM-RAW.

Driven with a fake account pointed at closed local ports, so nothing reached a real server:

```
$ $A --root $M exec "mail account add --email docs@example.invalid --name Docs --pass not-a-real-pass --imap 127.0.0.1:13462 --smtp 127.0.0.1:13463" \
      "mail account test docs@example.invalid"; $A --root $M exec "mail sync" "mail folders"
[SAVED] Account added successfully: Docs (docs@example.invalid)
  Incoming IMAP: 127.0.0.1:13462
  Outgoing SMTP: 127.0.0.1:13463
Testing connectivity for docs@example.invalid (127.0.0.1 / 127.0.0.1)...
[FAIL] Incoming 127.0.0.1:13462 FAILED (io-error: connection refused (Connection refused)) • Outgoing 127.0.0.1:13463 FAILED (...)
FAILED: io-error: mail account test: docs@example.invalid did not pass
[SYNC] Mailbox check completed: Never synced • 0 unread • Sync failed: [Errno 111] Connection refused
AetherMail Mailboxes
  Inbox (inbox): 0 total, 0 unread
  ...
```

`mail account test` prints `[OK]` only when both servers answered and, when a password is set, the IMAP login succeeded. Otherwise it prints `[FAIL]` with the same detail and the command fails (non-zero exit).

The send, login-test and sync helpers are `python3` scripts run directly (no shell); they speak SMTP (STARTTLS and AUTH) and IMAP. Credentials and message text reach them on stdin, never on a command line.

### The keyring

Account passwords are not stored in the clear:

| file in `<root>/mail/` | holds |
|---|---|
| `accounts.idx` | servers, ports, usernames; no secrets |
| `keyring.key` | 32 random bytes generated once per install, mode 0600 |
| `keyring.vault` | the passwords, ChaCha20-Poly1305 (RFC 8439) under `keyring.key`, with a fresh nonce on every write |
| `envelope.db`, `sync.state`, `store/` | the message index, sync position and cached bodies |

Driven: after `account add`, the test password string appeared in none of the plain files. `keyring.key` was `-rw-------`.

**The keyring is in the root. Never commit a root, and never copy `keyring.key` anywhere you would not copy the passwords.** If you lose `keyring.key`, the vault cannot be opened again, and you set each password again with `mail account set-pass`. A vault written in the old V1 format, whose key was hardcoded in the source, is re-encrypted under `keyring.key` the first time it is read.

No shell is involved and no secret is on a command line. The helpers are started with `posix_spawnp` (argv straight to `python3`, never `sh -c`). Host and port are arguments. The user name, password, and for a send the subject and body, are written to the helper's stdin. Driven against a local fake SMTP/IMAP server: a subject of `hi'; touch /tmp/pwned; '` arrived verbatim and created no file, and `ps` during the login showed `mail_sync.py headers 127.0.0.1 13581 tester@example.test - INBOX ...`, with `-` where the password used to be.

### Contacts (`contacts`, alias `addressbook`; F8)

```
contacts list [group] | search <q> | show <id> | add <first> <last> <email> [phone] [company] | delete <id> | favorite <id> | groups | export | status
```

Cards are stored under `<root>/contacts/` (`contacts.idx` plus `cards/`). `export` prints vCard 3.0. There is no CardDAV sync: the address book is local only.

```
$ $A --root $M exec "contacts add Ada Lovelace ada@example.invalid 555-0100 Analytical" "contacts list" "contacts export"
[SAVED] Created contact Ada Lovelace (ct-101)
AetherContacts Address Book [all] (1)
    [ct-101] Ada Lovelace (Analytical) • ada@example.invalid • 555-0100
BEGIN:VCARD
VERSION:3.0
UID:ct-101
FN:Ada Lovelace
EMAIL;type=INTERNET;type=HOME:ada@example.invalid
...
```

### Calendar (`calendar`, aliases `cal`, `agenda`; F9)

```
calendar list | show <id> | add <title> <YYYY-MM-DD> <HH:MM> [duration_mins] [category] [location] | delete <id> | categories | export | status
```

Events are stored under `<root>/calendar/` (`calendar.idx` plus `events/`, one iCalendar file each). `export` prints a VCALENDAR. Like contacts, the calendar is local only.

```
$ $A --root $M calendar add Standup 2026-10-02 09:30 15 Work Room-1   # then: calendar list
  [ev-101] Oct 2, 2026 9:30 AM - 9:45 AM
     Standup @ Room-1 [Work]
```

The F9 month grid draws the month it names: the 1st in its real weekday column (Monday first), the month's real length, and four to six week rows. The new-event sheet starts on the day in view and saves the Date and Time fields as typed (`2026-09-18`, `14:00 - 16:00`).

`calendar export` writes each event as a complete VEVENT: `UID`, `DTSTAMP` (UTC, required by RFC 5545), `SUMMARY`, `LOCATION`, `DESCRIPTION`, `DTSTART`, `DTEND` (rolled over midnight/month/year as needed; `;VALUE=DATE` for all-day events) and `CATEGORIES`, with text escaped. The per-event `.ics` files use the same writer.

The checked-in drive for contacts and calendar is `build/aios-pim-drive <root> <out-dir>`. It now checks the grid the compositor painted against the C library's calendar for this month and the next twelve, that a new contact and a new event get ids and are saved as typed, and exits 1 on any failure. Steps it only photographs are marked `[SHOT]`, not `[PASS]`.

---

## 5. The editor

Open it with F4 or the `</>` dock icon. It has a file tree on the left, the buffer with syntax colouring, buffer tabs, and a toolbar: Save, Undo, Redo, Find, Preview, New, Parent dir, Build, Command.

- **The editor opens with the keyboard in the buffer.** Press Ctrl+L to reach its command line, or Escape to give up buffer focus. Typing `open <path>` while the buffer has focus inserts the text into the scratch buffer.
- Command-line verbs (compositor.cpp:4280): `open|e|edit <path>`, `save|w`, `new`, `close`, `reload`, `goto|line <n>`, `find|f`, `findprev|fp`, `replace|r`, `preview|read`, `theme`, `undo`, `redo`, `ls|cd`, `help`. A bare path opens that file.
- Ctrl+S saves, Ctrl+B builds, and Ctrl+P toggles the preview for documents that have one.

Driven headless:

```sh
$A --root $R desktop --headless --frames 8 --do 1:key:F4 --do 2:ctrl:l \
  --do 3:type:"open /tmp/aios-docs-apps/demo.cpp" --do 4:key:Enter \
  --do 6:shot:/tmp/aios-docs-apps/editor-open.png --dump /tmp/aios-docs-apps/editor-end.ppm
```

The shot showed `demo.cpp` as a tab with C++ highlighting, `C++ SAVED Ln 1, Col 1`, and the status line `opened /tmp/aios-docs-apps/demo.cpp (8 lines, 121 bytes)`.

### LSP

When a file opens, `LspClientManager` starts a language server for its language if one is on `PATH`:

| language | server |
|---|---|
| C/C++ | `clangd` |
| JS/TS | `typescript-language-server` |
| Python | `pylsp` |
| Go | `gopls` |
| Rust | `rust-analyzer` |

The editor's code binds:

- completion popups;
- F12 to go to definition;
- Shift+F12 to find references;
- F1 to hover;
- diagnostics in the gutter.

With the editor's buffer focused, F12, Shift+F12 and F1 go to the editor before the desktop's global bindings (`draw.toggle`, `pane.shell`), so they reach go-to-definition, references and hover. With no server for the file's language the status line says so, for example `no language server for C++ (clangd is not on PATH)`, and nothing is invented.

### Quick in-place edit from the command surface

`edit <file> <target> <replacement>` replaces **every** occurrence of `target`. Shell paths are rooted at `<root>/fs`; use `/host/...` for a host path. Files under `src/kernel/`, `src/capability/`, `include/aios/kernel/` and `include/aios/capability/` are refused.

```
$ $A --root $R exec "edit /host/tmp/aios-docs-apps/txdemo/b.txt three THREE"
edited /host/tmp/aios-docs-apps/txdemo/b.txt (1 replacement)
```

### Transactional multi-file edit: `tx`

`tx` takes a unified diff and applies it across several files atomically. The engine is `patch_transaction.cpp`.

```
tx plan <patch>    parse, show the manifest, dry-run every hunk against the files on disk
tx stage <patch>   dry-run and hold the result in memory (for this session only); the tree is not touched
tx show            the staged transaction: files, hunks accepted, +/- lines
tx commit          write every staged file to disk
tx abort           drop the staged transaction
tx apply <patch>   stage and commit in one step
```

The staged transaction exists only in memory, in the process that staged it. Run `stage` and `commit` in the same `exec`, or in the same shell or desktop session. A `tx commit` from a new process says `No active staged transaction`. Paths in the diff are relative to the current directory.

Driven on two scratch files (`a/a.txt` with `beta` changed to `BETA`, `a/b.txt` with `three` appended):

```
$ $A --root $R exec "tx plan change.patch" "tx stage change.patch" "tx show"
Transaction #402: Atomic Code Transaction
  [M] a.txt
  [M] b.txt
[SUCCESS] Pre-flight dry-run check PASSED (2 files clean, 2 hunks match substrate)
[STAGED] Transaction #402: Atomic Code Transaction (2 files, 2 hunks staged in memory)
Status: STAGED
Hunks: 2/2 accepted
Delta: +2 -1 lines
```

After that run both files were unchanged on disk. Staging and committing in one run:

```
$ $A --root $R exec "tx stage change.patch" "tx commit"
[COMMITTED] Transaction #402 atomically written to disk (2 files updated).
```

Both files then held the new text. Planning the same patch again rejects it, because the files no longer match:

```
[REJECTED] Pre-flight dry-run check FAILED: invalid: Dry-run mismatch in a.txt at line 2:
  Expected: "beta"
  Actual:   "BETA"
```

`tx review` prints the staged transaction hunk by hunk, with each hunk's accepted/rejected state. The editor has the interactive version: on its command line (Ctrl+L) type `review` to open the staged transaction as a review buffer, where Tab moves between hunks, Space accepts or rejects a hunk, Ctrl+Enter commits and Esc aborts. Staging lives in the process, so stage and review in the same session (the desktop's shell pane and its editor share one).

---

## 6. Package clients

Each client speaks its registry's protocol directly over the kernel's own TLS and checks every archive against the hash the registry published. **Without `dir=`, packages go into the current git repository registered with the kernel, or into the root if there is none. Pass `dir=` so they do not land in an unexpected place.** `deps=0` skips dependencies, and `as=$entity` runs the command under that entity's authority.

| command | verbs | lands in |
|---|---|---|
| `npm` | `view <pkg>`, `install [<pkg>[@range]] [registry=] [deps=0]` | `<dir>/node_modules/` |
| `pip` (`pip3`, `python`) | `view <pkg>`, `install <pkg>[==ver] [python=3.12] [platform=x86_64] [index=]` | `<dir>/` (default `<repo>/site-packages`) |
| `cargo` (`crates`) | `view <crate>`, `vendor <crate>[@ver]` | `<dir>/vendor/<crate>-<ver>/`, source only |
| `go` (`gomod`) | `list <module>`, `get\|download <mod>[@ver] [sums=0] [proxy=] [sumdb=] [key=]`, `mod verify` | `<dir>/vendor/` plus `<dir>/go.sum` |

Driven, one small public package each:

```
$ $A --root $R npm install is-number dir=/tmp/aios-docs-apps/pkgs/npm
[NPM INSTALL] is-number
  registry      : https://registry.npmjs.org over TLS 1.2 ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
  trusted via   : GTS Root R4 — every tarball checked against the hash the registry published
  into          : /tmp/aios-docs-apps/pkgs/npm/node_modules
  is-number                   7.0.0       4 files   3.6 KiB

$ $A --root $R pip install six dir=/tmp/aios-docs-apps/pkgs/py
[PIP INSTALL] six
  index         : https://pypi.org over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256
  chosen for    : CPython 3.12 on x86_64
  six                       1.17.0      6 files     10.8 KiB    six-1.17.0-py2.py3-none-any.whl

$ $A --root $R cargo vendor itoa dir=/tmp/aios-docs-apps/pkgs/crates
[CARGO VENDOR] itoa
  registry      : https://crates.io over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256
  itoa                    1.0.18      14 files    15.6 KiB
  total         : 1 crate(s), 15.6 KiB in 511.79 ms — source, verified; building it is a compiler's job
```

pip installs a wheel by unpacking it, which leaves `six.py` and `six-1.17.0.dist-info/` in the target directory. npm does not write a `package.json` or a lockfile.

### Go modules and the checksum database (`sumdb.cpp`)

`go get` checks each module against `sum.golang.org`. It reads the signed tree head and the log tiles, proves the module's record is in the log, and then writes `go.sum`:

```
$ $A --root $R go get rsc.io/sampler@v1.3.0 deps=0 dir=/tmp/aios-docs-apps/pkgs/go
[GO DOWNLOAD] rsc.io/sampler@v1.3.0
  proxy         : https://proxy.golang.org over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256
  checksum db   : https://sum.golang.org — go.sum database tree of 66250608 records, signed by sum.golang.org
  proved into   : zxxi2+CGsOLpNYPXgewWWwhnwptfPq6VktA8frVseFA= (7 log tiles read to prove it)
  into          : /tmp/aios-docs-apps/pkgs/go/vendor
  rsc.io/sampler                    v1.3.0                                7 files   14.0 KiB    h1:7uVkIFmeBqHfdjD+gZwtXXI+RODJ2Wc4O7MPEh/QiW4=
```

`go mod verify` re-hashes the vendor tree against `go.sum`. If a tracked file is altered after download, verification fails:

```
error: corrupt: rsc.io/sampler@v1.3.0: what is on disk hashes to h1:x/Z6r0ooKXN/eNdHG4FKSJvsBp2ABAbezODEluRjKs4=, and go.sum says h1:7uVkIFmeBqHfdjD+gZwtXXI+RODJ2Wc4O7MPEh/QiW4= — this tree has been altered since it was downloaded
```

`sums=0` skips the checksum database, and `sumdb=` / `key=` point at a different one.

---

## 7. Other surfaces mentioned here

- **Horizon** (`horizon watch <name> <url>`, `horizon scan`, and the Horizon panel). It reads feeds and keeps only what touches the subjects of this kernel's own commits. An HTML page as a source gave `0 considered`. Feeds are covered in [services.md](services.md).
- **Task ledger** (`task`, alias `taskledger`). On a fresh root it prints an empty `Task Ledger Directory`.

---
