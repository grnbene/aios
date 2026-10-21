#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {
void metal_timer_init(uint32_t hz);
void metal_timer_reload();
void metal_timer_stop();
uint64_t metal_now_ns();
uint64_t metal_cntpct();
uint64_t metal_cntfrq();
uint32_t metal_timer_intid();
void metal_delay_us(uint64_t us);
void metal_timer_init_percpu();   /* a secondary core: arm its own timer, leave the shared clock alone */
}
