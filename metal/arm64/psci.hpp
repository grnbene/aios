#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {
uint32_t metal_psci_version();
int metal_psci_cpu_on(uint64_t target_mpidr, uint64_t entry_point, uint64_t context_id);
int metal_psci_affinity_info(uint64_t target_mpidr);
[[noreturn]] void metal_psci_system_off();
[[noreturn]] void metal_psci_system_reset();

void metal_spinlock_lock(volatile uint32_t* lock);
void metal_spinlock_unlock(volatile uint32_t* lock);
bool metal_spinlock_try(volatile uint32_t* lock);
}
