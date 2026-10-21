#!/usr/bin/env python3
"""Claude Code hook for the agent stream (scripts/stream/streamd.py): PreToolUse and PostToolUse on every tool.

Tells the daemon before and after an edit, and hands the agent whatever the stream has for it (a unit of its that
another agent replaced, a compile break its write caused) as context on that tool call. Fails open: with the daemon
down, or slow, the agent's tool runs as if there were no hook.
"""
import json, os, sys, urllib.request

def main():
    try:
        info = json.load(sys.stdin)
    except Exception:
        return
    # The agent's claude process (the hook runs under it, maybe through a shell): the daemon idles its process tree.
    pid = os.getppid()
    for _ in range(4):
        try:
            with open(f"/proc/{pid}/comm") as f: comm = f.read().strip()
            if comm == "claude": info["claude_pid"] = pid; break
            with open(f"/proc/{pid}/stat") as f: pid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError):
            break
    try:
        req = urllib.request.Request("http://127.0.0.1:8790/hook", data=json.dumps(info).encode(),
                                     headers={"content-type": "application/json"})
        out = json.load(urllib.request.urlopen(req, timeout=3))
    except Exception:
        return
    notes = out.get("notices") or []
    if not notes:
        return
    text = "Stream (the shared live tree; other agents write it too):\n\n" + "\n\n".join(notes)
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": text}}))

if __name__ == "__main__":
    main()
