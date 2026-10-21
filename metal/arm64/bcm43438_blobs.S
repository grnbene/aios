/* Broadcom / Cypress BCM43438 Firmware and NVRAM Configuration Blobs
 * Embedded for freestanding AArch64 bare-metal kernel.
 */
.section .rodata
.balign 16

.globl bcm43438_firmware_start
bcm43438_firmware_start:
.incbin "metal/arm64/firmware/bcm43430-sdio.bin"
.globl bcm43438_firmware_end
bcm43438_firmware_end:

.balign 16
.globl bcm43438_nvram_start
bcm43438_nvram_start:
.incbin "metal/arm64/firmware/bcm43430-nvram.txt"
.globl bcm43438_nvram_end
bcm43438_nvram_end:

/* Regulatory (CLM) data for firmware 7.45.98.118. That firmware is built
 * "noclminc-clm_min": it ships no country tables and will not bring the
 * radio up until this is loaded via the "clmload" iovar (see dhd_load_clm).
 * Source: linux-firmware cypress/cyfmac43430-sdio.clm_blob (ClmImport
 * 1.36.3, matching the firmware's own ClmImport string). */
.balign 16
.globl bcm43438_clm_start
bcm43438_clm_start:
.incbin "metal/arm64/firmware/bcm43430-sdio.clm_blob"
.globl bcm43438_clm_end
bcm43438_clm_end:
