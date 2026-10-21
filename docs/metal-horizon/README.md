# The horizon, on the metal

Two screendumps of the emulated VGA framebuffer — QEMU's `screendump`, not a
window on anyone's desktop. The kernel set the mode, composited every pixel and
rasterised every glyph itself.

Regenerate both:

    bash metal/build.sh
    python3 metal/drive-horizon.py

| shot | what it shows |
|---|---|
| `horizon-01-empty.png` | the canvas before the machine has looked at anything. The horizon column carries its own emptiness honestly. |
| `horizon-02-mapped.png` | after a **standing directive** scanned three times with nobody typing: the two headlines out of six that touched this machine's own commits, each with a bar for what it is worth and the machine's own terms it matched. |

What makes the second shot mean something is where the terms came from. The
machine wrote three commits of its own — *"virtio scanout brought up on the
metal"*, *"virtio scanout flushed at the end of every frame"*, *"virtio queue
notify address computed from the queue"* — and nothing else was ever typed at
it. It read a page of six headlines, four of them about coffee, football,
pottery and rainfall, and kept the two that touch `virtio`, `scanout` and
`queue`. Nobody gave it a keyword.

The drive checks all of that over the serial console, and checks the glass by
measuring the horizon column's ink before and after the scan.
