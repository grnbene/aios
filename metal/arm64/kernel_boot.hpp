#pragma once
/* The one aios::Kernel this board runs - see kernel_boot.cpp. */
namespace aios { class Kernel; }
namespace aios_arm64 {
aios::Kernel& kernel();
}
