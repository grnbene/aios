#!/usr/bin/env python3
"""Generate docs/reference.md and docs/tools.md from the source itself.

    scripts/gen-reference.py            write both files
    scripts/gen-reference.py --check    exit 1 if either file is out of date, or a command is undocumented

The command reference comes from the console's dispatcher (cli/commands.cpp) and the table of what each command
and subcommand does (cli/command_docs.cpp), which `help`, `help <cmd>` and `man` print too. The check compares
the two: a subcommand a handler accepts that the table does not describe, a described subcommand no handler
accepts, or a dispatched command with no entry, fails it. The tools reference is read from CMakeLists.txt and each
program's own header comment. cli/command_aliases.inc, which `man` and `help` read, comes from the dispatcher.
Rerun it after changing a command or a tool.
"""
import glob, os, re, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMDS = os.path.join(HERE, "cli", "commands.cpp")
DOCS = os.path.join(HERE, "cli", "command_docs.cpp")

# Commands left out of the help on purpose.
UNDOCUMENTED = {"miner"}
# Handlers that hand the whole line to another.
FOLLOW = {"cmd_grain": "run_grain_command"}


def read(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def unescape(s):
    return s.replace('\\"', '"').replace("\\n", " ").replace("\\t", " ").strip()


def blank_literals(t):
    """The source with comments, char literals and string contents blanked (the quotes and newlines kept), so that
    braces can be counted and positions still line up with the original."""
    out, i, n = [], 0, len(t)
    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            j = n if j < 0 else j
        elif t.startswith("/*", i):
            j = t.find("*/", i) + 2
        elif t.startswith('R"(', i):
            j = t.find(')"', i) + 2
        elif t[i] in "'\"":
            q, j = t[i], i + 1
            while t[j] != q:
                j += 2 if t[j] == "\\" else 1
            out.append(q + re.sub(r"[^\n]", " ", t[i + 1:j]) + q)
            i = j + 1
            continue
        else:
            out.append(t[i])
            i += 1
            continue
        out.append(re.sub(r"[^\n]", " ", t[i:j]))
        i = j
    return "".join(out)


def handlers():
    """handler name -> (file, start line, body) for every console handler in cli/."""
    out = {}
    for f in sorted(glob.glob(os.path.join(HERE, "cli", "*.cpp"))):
        src = read(f)
        blank = blank_literals(src)
        for m in re.finditer(r"\n(?:static )?(?:void|Result<void>|aios::Result<void>|bool) ((?:cmd_[a-z0-9_]+)|[a-z_]+_command)\s*\([^)]*\)\s*\{", blank):
            i, depth = m.end(), 1
            while depth and i < len(blank):
                depth += {"{": 1, "}": -1}.get(blank[i], 0)
                i += 1
            out.setdefault(m.group(1), (os.path.relpath(f, HERE), line_of(src, m.start() + 1), src[m.end():i]))
    return out


def first_words(body):
    """The words a handler compares its first argument against: a[1] itself, and any variable it is copied into."""
    names = {"sub", "sub0", "verb"}
    for m in re.finditer(r"\b(?:const\s+)?(?:std::string|auto)\s*&?\s+(\w+)\s*=\s*[^;]*\ba\[1\]", body):
        names.add(m.group(1))
    words = set()
    for v in names | {r"a\[1\]"}:
        v = v if v.startswith("a\\") else re.escape(v)
        words.update(re.findall(r"(?<![\w.])" + v + r'\s*==\s*"([^"]+)"', body))
        words.update(re.findall(r'"([^"]+)"\s*==\s*' + v + r"\b", body))
    for m in re.finditer(r"\bsubs\[\]\s*=\s*\{([^}]*)\}", body):   # a list the first argument is looked up in
        words.update(re.findall(r'"([^"]+)"', m.group(1)))
    return words


def canonical(rows, documented):
    """Rows merged by handler: [(name, aliases, target, line, inline)], where a command whose handler another
    documented command already reaches (btc and chain) is that command's alias."""
    owner = {}   # handler -> the documented command that reaches it
    for names, target, ln, inline in rows:
        if target != "(inline)" and names[0] in documented:
            owner.setdefault(target, names[0])
    out = []
    for names, target, ln, inline in rows:
        key = names[0] if names[0] in documented or target not in owner else owner[target]
        row = next((o for o in out if o[0] == key), None)
        if row is None:
            row = [key, [], target, ln, inline]
            out.append(row)
        elif key == names[0] and row[4] != inline:
            row[4] += inline   # the same command dispatched again on a further condition (host load, host)
        if key == names[0] and row[3] != ln and "&&" in row[4].split("\n")[0] and "&&" not in inline.split("\n")[0]:
            row[2], row[3] = target, ln   # the plain dispatch of a command, not the conditional one before it
        for n in names:
            if n != key and n not in row[1]:
                row[1].append(n)
    return out


def dispatcher(src):
    """[(names, target, line, inline body)] in dispatch order. A command dispatched with a further condition
    (`cmd == "net" && a[1] == ...`) is listed under its name."""
    rows = []
    ms = list(re.finditer(r'(?:if|else if) \(((?:cmd == "[a-z0-9_-]+"(?: \|\| )?)+)(?:\)| &&)\s*(.{0,160})', src))
    for k, m in enumerate(ms):
        names = re.findall(r'cmd == "([a-z0-9_-]+)"', m.group(1))
        body = m.group(2)
        t = re.search(r"\b((?:cmd_|[a-z_]+_command\b)[a-z0-9_]*)\s*\(", body)
        end = ms[k + 1].start() if k + 1 < len(ms) else len(src)
        rows.append((names, t.group(1) if t else "(inline)", line_of(src, m.start()), src[m.start():end]))
    return rows


def command_docs():
    """[(name, section, line, what, [(words, usage, what)], line no)] from cli/command_docs.cpp."""
    src = read(DOCS)
    s = r'"((?:[^"\\]|\\.)*)"'
    docs = []
    for i, ln in enumerate(src.split("\n"), 1):
        m = re.match(r"\s*\{" + r",\s*".join([s] * 4) + r",\s*\{\s*$", ln)
        if m:
            docs.append([unescape(x) for x in m.groups()] + [[], i])
            continue
        m = re.match(r"\s*\{" + r",\s*".join([s] * 3) + r"\},\s*$", ln)
        if m and docs:
            docs[-1][4].append(tuple(unescape(x) for x in m.groups()))
    return docs


def check_docs(rows, funcs, docs, src):
    """What the table and the handlers disagree on, as lines to print."""
    problems = []
    by_name = {d[0]: d for d in docs}
    grain_app = re.search(r"bool grain_app_word\(.*?\{\s*static const char\* words\[\] = \{(.*?)\};", src, re.S)
    rows = canonical(rows, set(by_name) | UNDOCUMENTED)
    for key, aliases, target, ln, inline in rows:
        if key in UNDOCUMENTED:
            continue
        d = by_name.get(key)
        if not d:
            problems.append(f"cli/commands.cpp:{ln}: `{key}` is dispatched but has no entry in cli/command_docs.cpp")
            continue
        target = FOLLOW.get(target, target)
        if target in funcs:
            accepted = first_words(funcs[target][2])
        else:
            accepted = set(re.findall(r'a\[1\] == "([^"]+)"', inline))
        if key == "app" and grain_app:
            accepted |= set(re.findall(r'"([^"]+)"', grain_app.group(1)))
        described = set()
        for words, usage, what in d[4]:
            described.update(words.split("|"))
        for w in sorted(accepted - described):
            problems.append(f"cli/command_docs.cpp:{d[5]}: `{key} {w}` is accepted by its handler but not described")
        if accepted:
            for w in sorted(described - accepted):
                problems.append(f"cli/command_docs.cpp:{d[5]}: `{key} {w}` is described but its handler does not accept it")
    dispatched = {r[0] for r in rows}
    for d in docs:
        if d[0] not in dispatched:
            problems.append(f"cli/command_docs.cpp:{d[5]}: `{d[0]}` has an entry but is not dispatched")
    return problems


def gen_reference():
    src = read(CMDS)
    funcs = handlers()
    docs = command_docs()
    by_name = {d[0]: d for d in docs}
    rows = canonical(dispatcher(src), set(by_name) | UNDOCUMENTED)
    aliases = {r[0]: r[1] for r in rows}
    body = []
    total_subs = 0
    for d in docs:
        name, section, line, what, subs, _ = d
        body += [f"## `{name}`", ""]
        if aliases.get(name):
            body += ["Also: " + ", ".join(f"`{n}`" for n in aliases[name]), ""]
        r = next((r for r in rows if r[0] == name), None)
        if r:
            loc = f"`cli/commands.cpp:{r[3]}`"
            target = FOLLOW.get(r[2], r[2])
            if target in funcs:
                loc += f" → `{target}` (`{funcs[target][0]}:{funcs[target][1]}`)"
            body += ["Dispatched at " + loc + ".", ""]
        body += [f"**{line}** - {what} *({section.title()})*", ""]
        if subs:
            total_subs += len(subs)
            body += ["| subcommand | usage | what it does |", "|---|---|---|"]
            for words, usage, swhat in subs:
                ws = " ".join(f"`{w}`" for w in words.split("|"))
                body.append(f"| {ws} | `{usage.replace('|', '&#124;')}` | {swhat.replace('|', '&#124;')} |")
            body.append("")
    shell = sorted(set(re.findall(r'(?:cmd|c|name|w) == "([a-z0-9_-]+)"', read(os.path.join(HERE, "cli", "shell.cpp")))))
    out = ["# Command reference", "",
           "Generated by `scripts/gen-reference.py` from `cli/commands.cpp` and `cli/command_docs.cpp`; do not edit by",
           "hand. Every command the console dispatches, what it and each of its subcommands do, and where its handler",
           "is. `help <cmd>` prints the same in the console. For how to use them, see the guides listed in the README.", "",
           f"{len(docs)} commands, {total_subs} subcommands, and {len(shell)} familiar shell commands.", "",
           "The familiar shell (`cli/shell.cpp`, see docs/console.md): " + " ".join(f"`{n}`" for n in shell), "",
           "Commands: " + " ".join(f"[`{d[0]}`](#{d[0]})" for d in sorted(docs)), ""]
    return "\n".join(out + body).rstrip() + "\n"


def gen_aliases():
    """cli/command_aliases.inc: alias -> the command it names, for `man` and `help`."""
    out = ["// Generated by scripts/gen-reference.py from cli/commands.cpp; do not edit by hand.",
           "// Each alias the dispatcher accepts, and the command it names."]
    documented = {d[0] for d in command_docs()} | UNDOCUMENTED
    for name, aliases, target, ln, inline in canonical(dispatcher(read(CMDS)), documented):
        for alias in aliases:
            out.append('{"%s", "%s"},' % (alias, name))
    return "\n".join(out) + "\n"


def gen_tools():
    cm = read(os.path.join(HERE, "CMakeLists.txt"))
    tests = {}
    for m in re.finditer(r"add_test\(NAME (\S+) COMMAND ([^\s)]+)", cm):
        tests.setdefault(m.group(2).replace("${CMAKE_BINARY_DIR}/", ""), []).append(m.group(1))
    out = ["# Programs the build produces", "",
           "Generated by `scripts/gen-reference.py` from `CMakeLists.txt` and each program's own header",
           "comment; do not edit by hand. The drives exercise a subsystem the way an operator does; the",
           "checks assert on it; the benches measure it. `ctest` runs the ones registered as tests.", "",
           "| program | source | registered test | what it is |", "|---|---|---|---|"]
    # Targets inside if(FALSE)/if(OFF) blocks are never built, so a plain
    # regex over the whole file would advertise them as real programs. Walk the
    # file line by line instead, tracking whether we are inside a disabled block.
    cm_lines = cm.split("\n")
    disabled = 0
    targets = []
    for ln in cm_lines:
        stripped = ln.strip()
        mcond = re.match(r"if\s*\(\s*([A-Za-z0-9_.\"! ]+?)\s*\)", stripped)
        if mcond:
            cond = mcond.group(1).strip().strip('"')
            if cond in ("FALSE", "OFF", "0"):
                disabled += 1
        elif stripped.startswith("endif"):
            disabled = max(0, disabled - 1)
        m = re.match(r"add_executable\(([A-Za-z0-9_-]+)\s+([^)]+)\)", stripped)
        if m:
            targets.append((m.group(1), m.group(2).split(), disabled > 0))
    for name, srcs, not_built in targets:
        if name == "aios-cli":
            shown = "aios"
        else:
            shown = name
        first = next((s for s in srcs if s.endswith((".cpp", ".c"))), "")
        desc = ""
        p = os.path.join(HERE, first)
        if first and os.path.exists(p):
            # A program's description is its file header: a column-0 comment run that a
            # blank line or a #directive follows. A run that sits directly on a declaration
            # (a helper struct's comment) describes that declaration, not the program.
            src_lines = read(p).split("\n")[:60]
            lines, run, i = [], [], 0
            while i < len(src_lines):
                ln = src_lines[i]
                s = ln.strip()
                if ln.startswith(("//", "/*", " *")):
                    if not s.startswith(("// SPDX", "//#")):
                        t = re.sub(r"^(//+|/\*+|\*+/?)\s?", "", s).rstrip("*/ ").strip()
                        if t:
                            run.append(t)
                    i += 1
                    continue
                if run and (not s or s.startswith("#")):
                    lines = run
                    break
                run = []
                if s and not s.startswith(("#", "using ", "namespace ")):
                    break   # real code reached with no header seen
                i += 1
            desc = re.sub(r"\s+", " ", " ".join(lines))
            if len(desc) > 260:
                desc = desc[:257].rsplit(" ", 1)[0] + " …"
        t = ", ".join(f"`{x}`" for x in tests.get(name, []))
        if not_built:
            note = "**not built** (disabled in `CMakeLists.txt`)"
            if not desc:
                desc = note
            else:
                desc = f"{desc} -- {note}"
        out.append(f"| `{shown}` | `{first}` | {t} | {desc.replace('|', '/') or '(no header comment)'} |")
    return "\n".join(out) + "\n"


def main():
    check = "--check" in sys.argv
    stale = False
    problems = check_docs(dispatcher(read(CMDS)), handlers(), command_docs(), read(CMDS))
    for p in problems:
        print(p)
    if problems:
        stale = True
    for rel, text in (("docs/reference.md", gen_reference()), ("docs/tools.md", gen_tools()),
                      ("cli/command_aliases.inc", gen_aliases())):
        p = os.path.join(HERE, rel)
        old = read(p) if os.path.exists(p) else None
        if old == text:
            continue
        if check:
            print(f"{rel} is out of date: run scripts/gen-reference.py")
            stale = True
        else:
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
            print(f"wrote {rel}")
    sys.exit(1 if stale else 0)


if __name__ == "__main__":
    main()
