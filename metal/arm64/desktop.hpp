#pragma once
/* Graphical Desktop Subsystem for AArch64 Bare-Metal (metal/arm64/desktop.hpp).
 * Provides a rich, responsive windowed and tabbed graphical workspace directly on the
 * framebuffer with system telemetry, Broadcom BCM43438 Wi-Fi network inspector,
 * live interactive shell terminal, and process manager.
 */
#include <cstddef>
#include <cstdint>

/* Run the interactive graphical desktop loop on the framebuffer. */
void desktop_run();

/* Initialize desktop data structures. */
void desktop_init();

/* Returns true if the desktop is currently active on the framebuffer. */
bool desktop_is_active();

/* Force paint a full desktop frame. */
void desktop_paint();
