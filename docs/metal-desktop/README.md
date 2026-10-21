# The bare-metal desktop, as photographed off video memory

Every image here is a QEMU `screendump` of the framebuffer the AIOS kernel
composited itself while booted on the bare metal — no host window toolkit, no
web view. They are produced by `metal/drive-desktop.py`, which boots the ISO,
types `desktop` on the serial command surface, then operates the machine
through the real PS/2 mouse and keyboard.

Reproduce:

    python3 metal/tools/mkfont.py          # only if the type changed
    bash metal/build.sh
    python3 metal/drive-desktop.py build/metal/aios-metal.iso build/metal

| shot | what it proves |
|---|---|
| `desktop-01-opened.png` | the kernel set the adapter to 1920x1080x32 itself and composited the workspace: four panes, laid out for the mode it chose |
| `desktop-02-pointer.png` | the PS/2 mouse moved and the pointer is at exactly the requested (300, 240) |
| `desktop-03-dragged.png` | a window dragged by its titlebar, measured +120 / +72 px |
| `desktop-04-command.png` | `info` typed on the real keyboard, run on the same `cli::Session` the serial console uses, its answer read back off the framebuffer in the kernel's own 8x16 font |
| `desktop-06-chip-click.png` | the `devices` chip clicked with the mouse, and the device table it produced |
| `desktop-07-wheel.png` | the wheel scrolled the terminal backlog (4-byte IntelliMouse packets) |
| `desktop-08-f2-machine.png` | F2 raised the Machine pane: the keyboard drives the window manager, and the tab rule and dock follow focus. Its BUSES card is a live PCI configuration-space scan |
| `desktop-05-back-to-console.png` | Escape gave the screen back to the text console |
| `desktop-09-virtio-gpu.png` | the same desktop on a machine with a **virtio-gpu**, where nothing in the boot path could give the kernel a framebuffer at all — this one the kernel brought up itself, through the device's own queues |

## The look

The palette and the layout grammar are the assay console's, ported token for
token out of `console/web/src/styles.css` — the shipped Slate dark set:

| console token | value | where it lands here |
|---|---|---|
| `--bg` | `#0a0a0a` | the desktop |
| `--panel` | `#171717` | a window's surface and its titlebar |
| `--panel-2` | `#101010` | every card inside a window |
| `--sidebar` | `#070707` | the top bar and the dock |
| `--text` / `--text-dim` / `--text-faint` | `#f4f4f4` / `#b2b2b2` / `#8e8e8e` | value, body, label |
| `--line` | 8% white | card hairlines |
| `--border` / `--border-strong` | `#3d3d3d` / `#767676` | an unfocused frame / the focused one |
| `--accent` | `#e6e6e6` | the active tab's rule, the terminal cursor |
| `--ok` / `--warn` / `--err` | `#42d77d` / `#f5a623` / `#ff6b6b` | status, and nothing else |

Same moves, too: cards with a rounded corner and an upper-case tracked label
over every value, tabs marked by a 2px rule rather than a pill, one filled
button per pane and the rest a hairline ghost, hover on everything clickable.

Three panes, one per thing the machine can answer for — **Shell** (F1),
**Machine** (F2), **Entities** (F3). What used to be here as
well: a "Subsystem Matrix" of sixteen hard-coded `TASK-0xx  [x] Ready` rows, a
"Reasoning Studio" holding two sentences about an ABI, and a launcher column of
`[*]` `[#]` glyph tiles duplicating the top bar. None of them asked the kernel
anything, so none of them survived.

## Type, geometry and mode

Three things separate this from a framebuffer demo:

**The type is real.** `metal/tools/mkfont.py` rasterises DejaVu Sans, Sans Bold
and Sans Mono with FreeType at build time and emits 8-bit coverage per pixel
(`include/aios/userland/font_atlas.hpp`); the compositor blends that coverage
against whatever is under the glyph. There is no font engine in ring 0 and no
1-bit ROM font on screen — proportional UI text, a bold weight for what is
current, small tracked capitals for labels, and a real mono for anything the
machine typed or measured. `metal.font` fails the build if the checked-in atlas
is not what the generator produces.

**The geometry is antialiased.** Rounded corners, pills and window frames are
drawn from coverage, gradients are ordered-dithered so a 300px card has no
banding, the desktop carries the console's two soft radial washes, and window
shadows darken the pixels actually under them.

**The mode is the kernel's own choice.** The loader hands over whatever the
firmware's mode list held — under OVMF that is 1024x768 and nothing better. The
kernel then programs the display adapter directly through the Bochs DISPI
registers (`metal_display_best_mode` in `metal/hal_metal.cpp`), takes 1920x1080
at 32bpp, and moves the text console's grid with it. `metal.desktop` asserts
that it composites onto the mode it set, not the one it was given.

**The devices are read, not assumed.** The Machine pane's BUSES card is a PCI
configuration-space scan through 0xCF8/0xCFC: slot, class, vendor and device,
and which driver in this kernel has claimed each one — `aios virtio-gpu`,
`aios virtio-blk`, `aios dispi mode-set`, or `loader framebuffer` when the
kernel did not have to do anything. On a hosted machine the
same pane is filled from that OS's own view of its buses.
