"""Which firmware the bare-metal media can actually boot on this host.

grub-mkrescue only writes a BIOS El Torito record when grub's i386-pc modules
are installed; without them the ISO is UEFI-only and must be booted with OVMF.
The drives ask here rather than assuming BIOS.
"""
import os

BIOS_GRUB = "/usr/lib/grub/i386-pc"
OVMF_CANDIDATES = (
    "/usr/share/ovmf/OVMF.fd",
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/edk2/x64/OVMF.fd",
    "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
    "/usr/local/share/qemu/edk2-x86_64-code.fd",
)

def has_bios_grub():
    return os.path.isdir(BIOS_GRUB)

def ovmf():
    for p in OVMF_CANDIDATES:
        if os.path.exists(p):
            return p
    return None

def firmware_args(force_uefi=False):
    """QEMU args for booting the media: empty for BIOS, -bios or -drive for UEFI."""
    if force_uefi or not has_bios_grub():
        p = ovmf()
        if p:
            if "code.fd" in p or "CODE" in p:
                return ["-drive", f"if=pflash,format=raw,readonly=on,file={p}"]
            return ["-bios", p]
    return []

