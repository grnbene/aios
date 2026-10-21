# The canvas

What this machine looks like when it stops pretending to be a desktop.

`canvas-01-lifecycle.png` is a screendump of the real framebuffer — QEMU's
emulated video memory, 1920x1080, composited by the kernel itself — taken by
`metal/drive-canvas.py` after the machine had been given one standing directive
and then left alone at its prompt.

There are no windows and nothing to click. What is on the glass:

| Region | What it is |
|---|---|
| left column | every 128-bit identity this kernel holds — entities, states, capabilities, and what it knows — with the ones minted since the canvas opened lit brighter than the rest |
| authority | each entity, how many capabilities it holds, and a bar for the ceiling on the one that has a number (memory) |
| standing directives | the order, its interval, how many times it has run, a countdown bar to the next run, and what it said last time |
| what this machine knows | the object store's append log — here, two editions of the same page, because the page changed while nobody was looking |
| tasks | the process table, the console included, because the console is a task |
| lanes | identities, objects, entities, states, frames in and out on the machine's own TCP/IP, and context switches |

Regenerate with:

    bash metal/build.sh
    python3 metal/drive-canvas.py
