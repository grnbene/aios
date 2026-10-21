#!/usr/bin/env python3
"""Ask the mind what to type next into the AOS console."""
import subprocess, sys

import os
INSTRUCTION = os.environ.get("AIOS_INSTRUCTION", "You are running inside AOS. Explore the machine and figure out what you can do.")

MECHANICS = """
You interact with the machine only by typing lines into its console.
Below is everything that has appeared on that console so far, including what you typed.

Reply with between 1 and 3 command lines to type next — nothing else.
No commentary, no numbering, no markdown, no backticks. Just the raw lines.
"""

FINAL = """
Below is the full console session from your exploration.
Write a short report for the engineer who built this machine, covering:
what this machine is, what you found you could do on it, what surprised you,
and what you could NOT do that you would want to. Be concrete and honest.
"""

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "next"
    transcript = open("/tmp/aios-explore/transcript.txt").read()
    body = (FINAL if mode == "report" else MECHANICS) + "\n=== CONSOLE ===\n" + transcript
    p = subprocess.run(["claude", "-p", INSTRUCTION + "\n" + body, "--output-format", "text"],
                       capture_output=True, text=True, timeout=600)
    out = (p.stdout or p.stderr).strip()
    if mode != "report":
        # Relay only real command lines; the mind occasionally adds a sentence.
        verbs = {"info","devices","help","quit","authority","entity","cap","mem","state","tensor","graph","channel",
                 "checkpoint","restore","node","mind","store","set","expect","host","echo","man","ls","cat",
                 "cd","pwd","mkdir","rm","rmdir","cp","mv","tree","stat","find","grep","wc","head","tail",
                 "touch","ps","free","uname","nproc","hostname","whoami","id","date","uptime","env","export",
                 "unset","clear","history","which","sleep","lsblk","df","dmesg"}
        lines = []
        for l in out.splitlines():
            l = l.strip().strip("`").rstrip()
            if not l: continue
            first = l.lstrip("!").split()[0] if l.split() else ""
            if first in verbs: lines.append(l)
        out = "\n".join(lines[:3])
    print(out)

main()
