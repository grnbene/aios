#pragma once
/* ARM Generic Interrupt Controller, v2 and v3.
 *
 * Which one is present is a property of the machine, not of the kernel: QEMU
 * virt defaults to GICv2 under TCG and can be asked for GICv3, and real silicon
 * varies. The version is taken from the device tree and both drivers are here,
 * because a GICv3-only kernel silently takes no interrupts on a v2 box.
 */
#include <cstddef>
#include <cstdint>

extern "C" {
void metal_gic_init();
void metal_gic_init_percpu();
void metal_gic_enable_int(uint32_t intid);
void metal_gic_disable_int(uint32_t intid);
uint32_t metal_gic_ack_int();
void metal_gic_eoi_int(uint32_t intid);
void metal_gic_send_sgi(uint32_t intid, uint32_t target_cpu);
int metal_gic_version();
}
