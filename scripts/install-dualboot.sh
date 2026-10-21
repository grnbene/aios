#!/usr/bin/env bash
# ==============================================================================
# AIOS Bare-Metal Dual Boot & Disk Installer
# Installs AIOS bare-metal kernel and configures GRUB / UEFI for dual booting
# ==============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
KERNEL_ELF="$REPO_ROOT/build/metal/aios-metal.elf"
ISO_IMG="$REPO_ROOT/build/metal/aios-metal.iso"
GRUB_AIOS_FILE="/etc/grub.d/42_aios"

# Colors
RED="\033[0;31m"
GREEN="\033[0;32m"
CYAN="\033[0;36m"
YELLOW="\033[1;33m"
BOLD="\033[1m"
NC="\033[0m"

log_info() { echo -e "${CYAN}[AIOS INSTALL]${NC} $1"; }
log_ok()   { echo -e "${GREEN}[AIOS INSTALL]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[AIOS INSTALL]${NC} $1"; }
log_err()  { echo -e "${RED}[AIOS INSTALL]${NC} $1"; }

show_status() {
    echo -e "\n${BOLD}=== AIOS Bare-Metal Installation & Dual-Boot Status ===${NC}"
    
    # Check compiled kernel
    if [ -f "$KERNEL_ELF" ]; then
        SIZE=$(du -h "$KERNEL_ELF" | cut -f1)
        MOD=$(stat -c "%y" "$KERNEL_ELF" | cut -d"." -f1)
        log_ok "Compiled Kernel: $KERNEL_ELF ($SIZE, built $MOD)"
    else
        log_warn "Compiled Kernel: NOT BUILT (run ./metal/build.sh)"
    fi

    # Check installed kernel in /boot
    if [ -f "$BOOT_KERNEL_DEST" ]; then
        BSIZE=$(du -h "$BOOT_KERNEL_DEST" | cut -f1)
        BMOD=$(stat -c "%y" "$BOOT_KERNEL_DEST" | cut -d"." -f1)
        log_ok "Installed /boot Kernel: $BOOT_KERNEL_DEST ($BSIZE, installed $BMOD)"
    else
        log_warn "Installed /boot Kernel: NOT INSTALLED"
    fi

    # Check GRUB dual-boot configuration
    if [ -f "$GRUB_AIOS_FILE" ]; then
        log_ok "GRUB Config: $GRUB_AIOS_FILE is active"
    else
        log_warn "GRUB Config: $GRUB_AIOS_FILE NOT PRESENT"
    fi

    # Check if entries exist in /boot/grub/grub.cfg
    local cfg found=""
    for cfg in "$BOOT_DIR/grub/grub.cfg" "$BOOT_DIR/grub2/grub.cfg"; do
        if [ -f "$cfg" ] && grep -qE "(AI OS|AIOS) — Bare Metal" "$cfg" 2>/dev/null; then found=$cfg; fi
    done
    if [ -n "$found" ]; then
        log_ok "Bootloader Status: Dual-boot entry is ACTIVE in $found"
    else
        log_warn "Bootloader Status: Dual-boot entry NOT active in $BOOT_DIR/grub/grub.cfg or $BOOT_DIR/grub2/grub.cfg"
    fi

    # Disks and Partitions summary
    echo -e "\n${BOLD}Detected Storage Devices:${NC}"
    lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS | grep -v loop || true
    echo ""
}

# --- dry run -----------------------------------------------------------------
# `--dry-run` (or AIOS_DUALBOOT_DRY_RUN=1) changes nothing: it prints the GRUB
# entry text it would install and every privileged command it would run. The
# detection it uses can be pinned for a test, so a separate /boot or a
# grub2-mkconfig distro can be checked on a machine that is neither:
#   AIOS_BOOT_DIR        where the kernel goes            (default /boot)
#   AIOS_BOOT_FS_TARGET  mountpoint of the filesystem holding AIOS_BOOT_DIR
#   AIOS_BOOT_FS_SOURCE  its device;  AIOS_BOOT_FS_UUID / AIOS_BOOT_FS_TYPE
#   AIOS_BOOT_FS_ROOT    the subvolume it is mounted from (btrfs), e.g. /@
#   AIOS_GRUB_TOOL       update-grub | grub2-mkconfig | grub-mkconfig
#   AIOS_GRUB_CFG        the grub.cfg a *-mkconfig tool writes
DRY_RUN=${AIOS_DUALBOOT_DRY_RUN:-0}
BOOT_DIR=${AIOS_BOOT_DIR:-/boot}
BOOT_KERNEL_DEST="$BOOT_DIR/aios-metal.elf"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
run() {   # a privileged command: printed in a dry run, executed otherwise
    if [ "$DRY_RUN" = 1 ]; then echo "would run: $SUDO $*"; else $SUDO "$@"; fi
}

# The filesystem the kernel file lands on, and the path GRUB must ask for on it.
# GRUB's `search --fs-uuid` makes that filesystem the root, so the path is
# relative to ITS mountpoint: /boot/aios-metal.elf when /boot is a directory of
# the root filesystem, /aios-metal.elf when /boot is a partition of its own, and
# prefixed with the subvolume when it is a btrfs subvolume.
detect_boot_fs() {
    local target="" source="" fstype="" fsroot=""
    if command -v findmnt >/dev/null 2>&1; then
        read -r target source fstype fsroot < <(findmnt -n -o TARGET,SOURCE,FSTYPE,FSROOT --target "$BOOT_DIR" 2>/dev/null | head -1) || true
    fi
    BOOT_FS_TARGET=${AIOS_BOOT_FS_TARGET:-${target:-/}}
    BOOT_FS_SOURCE=${AIOS_BOOT_FS_SOURCE:-${source%%\[*}}
    if [ -z "$BOOT_FS_SOURCE" ]; then BOOT_FS_SOURCE=$(df "$BOOT_DIR" | tail -1 | awk '{print $1}'); fi
    BOOT_FS_TYPE=${AIOS_BOOT_FS_TYPE:-${fstype:-$(blkid -s TYPE -o value "$BOOT_FS_SOURCE" 2>/dev/null || true)}}
    BOOT_FS_UUID=${AIOS_BOOT_FS_UUID:-$(blkid -s UUID -o value "$BOOT_FS_SOURCE" 2>/dev/null || true)}
    BOOT_FS_ROOT=${AIOS_BOOT_FS_ROOT:-${fsroot:-/}}
    local rel="${BOOT_KERNEL_DEST#"${BOOT_FS_TARGET%/}"}"
    [ "${rel#/}" = "$rel" ] && rel="/$rel"
    if [ "$BOOT_FS_TYPE" = btrfs ] && [ "$BOOT_FS_ROOT" != "/" ]; then rel="${BOOT_FS_ROOT%/}$rel"; fi
    GRUB_KERNEL_PATH="$rel"
}

# Debian/Ubuntu ship update-grub; Fedora, RHEL, openSUSE and Arch ship
# grub2-mkconfig / grub-mkconfig, which must be told where grub.cfg lives.
detect_grub_tool() {
    GRUB_TOOL=${AIOS_GRUB_TOOL:-}
    if [ -z "$GRUB_TOOL" ]; then
        for t in update-grub grub2-mkconfig grub-mkconfig; do
            if command -v "$t" >/dev/null 2>&1; then GRUB_TOOL=$t; break; fi
        done
    fi
    [ -n "$GRUB_TOOL" ] || { log_err "no GRUB configuration tool found (update-grub, grub2-mkconfig or grub-mkconfig)"; exit 1; }
    GRUB_CFG=${AIOS_GRUB_CFG:-}
    if [ -z "$GRUB_CFG" ]; then
        case "$GRUB_TOOL" in
            *grub2-mkconfig) GRUB_CFG="$BOOT_DIR/grub2/grub.cfg" ;;
            *) if [ -d "$BOOT_DIR/grub2" ] && [ ! -d "$BOOT_DIR/grub" ]; then GRUB_CFG="$BOOT_DIR/grub2/grub.cfg"; else GRUB_CFG="$BOOT_DIR/grub/grub.cfg"; fi ;;
        esac
    fi
}
regen_grub() {
    case "$GRUB_TOOL" in
        *update-grub) log_info "Regenerating GRUB configuration with update-grub..."; run "$GRUB_TOOL" ;;
        *) log_info "Regenerating GRUB configuration with $GRUB_TOOL -o $GRUB_CFG..."; run "$GRUB_TOOL" -o "$GRUB_CFG" ;;
    esac
}

grub_entry_text() {
    cat << EOFGRUB
#!/bin/sh
exec tail -n +3 \$0
# ==============================================================================
# AIOS Bare-Metal Dual Boot Menu Entries
# kernel: $BOOT_KERNEL_DEST on $BOOT_FS_SOURCE ($BOOT_FS_TYPE, mounted at $BOOT_FS_TARGET)
# ==============================================================================

menuentry "AIOS — Bare Metal (Interactive Desktop)" --class aios --class os {
    insmod part_gpt
    insmod part_msdos
    insmod $GRUB_FS_MOD
    insmod all_video
    insmod vbe
    insmod gfxterm
    set gfxmode=1920x1080x32,1600x900x32,1440x900x32,1280x1024x32,1280x800x32,1024x768x32,auto
    set gfxpayload=keep
    search --no-floppy --fs-uuid --set=root $BOOT_FS_UUID
    multiboot2 $GRUB_KERNEL_PATH
}

menuentry "AIOS — Bare Metal (Acceptance Diagnostic & Self-Test)" --class aios --class os {
    insmod part_gpt
    insmod part_msdos
    insmod $GRUB_FS_MOD
    search --no-floppy --fs-uuid --set=root $BOOT_FS_UUID
    multiboot2 $GRUB_KERNEL_PATH accept
}
EOFGRUB
}

install_dualboot() {
    log_info "Starting AIOS Dual-Boot Installation...$([ "$DRY_RUN" = 1 ] && echo ' (dry run: nothing is changed)')"

    # Ensure kernel is built
    if [ ! -f "$KERNEL_ELF" ]; then
        if [ "$DRY_RUN" = 1 ]; then log_warn "Kernel not built at $KERNEL_ELF (a real run builds it first)"
        else log_info "Kernel not found at $KERNEL_ELF. Compiling now..."; "$REPO_ROOT/metal/build.sh"; fi
    fi

    detect_boot_fs
    detect_grub_tool
    if [ -z "$BOOT_FS_UUID" ]; then
        log_err "Failed to detect the UUID of $BOOT_FS_SOURCE, which holds $BOOT_DIR"
        exit 1
    fi
    log_info "Detected $BOOT_DIR on $BOOT_FS_SOURCE (UUID=$BOOT_FS_UUID, FS=$BOOT_FS_TYPE, mounted at $BOOT_FS_TARGET); GRUB path $GRUB_KERNEL_PATH; regenerate with $GRUB_TOOL"

    # Copy kernel to /boot
    log_info "Copying kernel image to $BOOT_KERNEL_DEST..."
    run cp "$KERNEL_ELF" "$BOOT_KERNEL_DEST"
    run chmod 644 "$BOOT_KERNEL_DEST"

    # Also copy to EFI partition if available
    if [ -d "$BOOT_DIR/efi/EFI" ]; then
        run mkdir -p "$BOOT_DIR/efi/EFI/AIOS"
        run cp "$KERNEL_ELF" "$BOOT_DIR/efi/EFI/AIOS/aios-metal.elf" || true
    fi

    # Determine GRUB filesystem module
    GRUB_FS_MOD="ext2"
    if [ "$BOOT_FS_TYPE" = "btrfs" ]; then GRUB_FS_MOD="btrfs"; fi
    if [ "$BOOT_FS_TYPE" = "xfs" ]; then GRUB_FS_MOD="xfs"; fi
    if [ "$BOOT_FS_TYPE" = "vfat" ] || [ "$BOOT_FS_TYPE" = "fat" ]; then GRUB_FS_MOD="fat"; fi

    # Create GRUB custom menuentry script
    log_info "Creating GRUB menu entry script at $GRUB_AIOS_FILE..."
    if [ "$DRY_RUN" = 1 ]; then
        echo "would write $GRUB_AIOS_FILE:"; grub_entry_text | sed 's/^/    | /'
    else
        grub_entry_text | $SUDO tee "$GRUB_AIOS_FILE" > /dev/null
    fi
    run chmod +x "$GRUB_AIOS_FILE"

    regen_grub

    log_ok "Dual-Boot installation $([ "$DRY_RUN" = 1 ] && echo 'dry run' || echo complete)!"
    log_ok "When rebooting, select 'AIOS — Bare Metal (Interactive Desktop)' from the boot menu."
}

uninstall_dualboot() {
    log_info "Removing AIOS Dual-Boot configuration..."
    detect_grub_tool
    if [ -f "$GRUB_AIOS_FILE" ]; then
        run rm -f "$GRUB_AIOS_FILE"
        log_info "Removed $GRUB_AIOS_FILE"
    fi
    if [ -f "$BOOT_KERNEL_DEST" ]; then
        run rm -f "$BOOT_KERNEL_DEST"
        log_info "Removed $BOOT_KERNEL_DEST"
    fi
    if [ -d "$BOOT_DIR/efi/EFI/AIOS" ]; then
        run rm -rf "$BOOT_DIR/efi/EFI/AIOS"
    fi
    regen_grub
    log_ok "AIOS dual-boot removed cleanly."
}

install_to_disk() {
    TARGET_DEV="$1"
    if [ ! -b "$TARGET_DEV" ]; then
        log_err "Device $TARGET_DEV is not a valid block device."
        exit 1
    fi

    log_warn "WARNING: This will format/flash $TARGET_DEV as an AIOS boot disk."

    if [ ! -f "$ISO_IMG" ]; then
        log_info "Building ISO image first..."
        "$REPO_ROOT/metal/build.sh"
    fi

    log_info "Writing bootable ISO image to $TARGET_DEV..."
    run dd if="$ISO_IMG" of="$TARGET_DEV" bs=4M status=progress conv=fsync
    run sync
    log_ok "Successfully flashed AIOS to $TARGET_DEV!"
}

ARGS=()
for a in "$@"; do if [ "$a" = "--dry-run" ]; then DRY_RUN=1; else ARGS+=("$a"); fi; done
set -- "${ARGS[@]+"${ARGS[@]}"}"
case "${1:-status}" in
    status)
        show_status
        ;;
    dualboot|install|host)
        install_dualboot
        ;;
    uninstall|remove)
        uninstall_dualboot
        ;;
    disk|target|flash)
        if [ -z "${2:-}" ]; then
            log_err "Usage: $0 disk <block_device> (e.g. /dev/sdc or /dev/sda)"
            exit 1
        fi
        install_to_disk "$2"
        ;;
    *)
        echo "Usage: $0 [--dry-run] {status|dualboot|uninstall|disk <dev>}"
        exit 1
        ;;
esac
