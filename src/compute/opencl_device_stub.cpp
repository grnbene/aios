// register_opencl_devices on a host with no OpenCL SDK headers to build the
// real backend (opencl_device.cpp) against: Kernel::init calls it
// unconditionally, so the symbol has to exist everywhere, and an honest empty
// report is what every other host gives a machine with no accelerator it can
// see (AcceleratorReport's own convention — see device.hpp).
#include "aios/compute/device.hpp"

namespace aios {

AcceleratorReport register_opencl_devices(DeviceFabric&, MemoryFabric&, hal::Host&, IdGenerator&) {
  return AcceleratorReport{{}, "OpenCL is not built on this host (no CL/cl.h at build time)"};
}

}  // namespace aios
