#pragma once
/* Raspberry Pi & ARM64 GPIO / Pin Management Subsystem (metal/arm64/gpio.hpp).
 *
 * Provides real hardware MMIO control for Raspberry Pi 3 (BCM2837),
 * Raspberry Pi 4 (BCM2711), and QEMU/Virt ARM64 hardware:
 *   - Real 40-Pin J8 Header mapping, pin direction, read, write, toggle, pull configuration.
 *   - Live level sampling, frequency/pulse transition measurement.
 *   - Software bit-banged I2C bus device scanner and address identifier.
 *   - Deep pin inspection, peripheral presence detection, and diagnostic acknowledgement.
 *   - ANSI & Graphical Header pinout rendering.
 */
#include <cstddef>
#include <cstdint>

enum class GpioMode : uint8_t {
  Input  = 0,
  Output = 1,
  Alt0   = 4,
  Alt1   = 5,
  Alt2   = 6,
  Alt3   = 7,
  Alt4   = 3,
  Alt5   = 2,
  Unknown = 255
};

enum class GpioPull : uint8_t {
  None = 0,
  Down = 1,
  Up   = 2
};

enum class PinType : uint8_t {
  Gpio,
  Power3V3,
  Power5V,
  Ground,
  IdEeprom
};

struct HeaderPinDef {
  uint8_t physical_pin;   /* 1..40 */
  int8_t  bcm_gpio;       /* 0..27, or -1 if power/gnd */
  PinType type;
  const char* default_name;
  const char* alt0_name;
};

struct GpioPinStatus {
  uint8_t  physical_pin;  /* 1..40 */
  int8_t   bcm_gpio;      /* 0..27, or -1 */
  PinType  type;
  const char* name;
  const char* alt0_name;
  GpioMode mode;
  bool     level;         /* true = High (3.3V), false = Low (0V) */
  GpioPull pull;
  uint32_t transition_hz; /* Measured pulse / transition rate in Hz */
  const char* detected_device; /* Inferred peripheral / bus */
};

struct GpioI2cDevice {
  uint8_t addr;
  const char* name;
  const char* description;
};

/* Initialize GPIO subsystem, detect SoC base address (Pi3: 0x3F200000, Pi4: 0xFE200000). */
void gpio_init();

/* Hardware & Platform detection */
bool gpio_is_live_hardware();
uint64_t gpio_mmio_base();
const char* gpio_platform_name();

/* Pin Configuration & I/O */
bool gpio_set_mode(unsigned bcm_pin, GpioMode mode);
GpioMode gpio_get_mode(unsigned bcm_pin);

bool gpio_write(unsigned bcm_pin, bool high);
bool gpio_read(unsigned bcm_pin);
bool gpio_toggle(unsigned bcm_pin);
bool gpio_set_pull(unsigned bcm_pin, GpioPull pull);
void gpio_pulse(unsigned bcm_pin, uint32_t high_us, uint32_t low_us, uint32_t count);

/* Physical Pin Conversion */
int8_t gpio_phys_to_bcm(uint8_t phys_pin);
uint8_t gpio_bcm_to_phys(uint8_t bcm_pin);
const HeaderPinDef* gpio_get_header_def(uint8_t phys_pin);

/* Inspection, Telemetry & Detection */
void gpio_sample_pin_status(uint8_t phys_pin, GpioPinStatus& out);
size_t gpio_sample_all_pins(GpioPinStatus out[40]);
uint32_t gpio_measure_pin_freq(unsigned bcm_pin, uint32_t sample_window_us = 5000);

/* Real bit-banged I2C Bus Scanner */
size_t gpio_i2c_scan_bus(unsigned sda_pin, unsigned scl_pin, GpioI2cDevice out_devices[], size_t max_devices);
const char* gpio_i2c_lookup_device(uint8_t addr);
/* True when the last scan found SDA/SCL held low (no pull-ups, or a device holding the bus). */
bool gpio_i2c_bus_stuck();

/* CLI Shell Reporting */
void gpio_render_pinout_ansi();
void gpio_report_list();
void gpio_report_pin(unsigned bcm_pin);
void gpio_inspect_and_acknowledge();
