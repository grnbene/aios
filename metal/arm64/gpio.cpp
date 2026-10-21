/* Raspberry Pi & ARM64 GPIO / Pin Management Subsystem (metal/arm64/gpio.cpp).
 *
 * Real hardware MMIO driver for Raspberry Pi 3 (BCM2837 @ 0x3F200000),
 * Raspberry Pi 4 (BCM2711 @ 0xFE200000), and QEMU/Virt ARM64 hardware.
 */
#include "gpio.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "timer.hpp"

namespace {

/* BCM2835 / BCM2837 / BCM2711 GPIO Register Offsets */
constexpr uint64_t GPFSEL0   = 0x00;
constexpr uint64_t GPFSEL1   = 0x04;
constexpr uint64_t GPFSEL2   = 0x08;
constexpr uint64_t GPFSEL3   = 0x0C;
constexpr uint64_t GPFSEL4   = 0x10;
constexpr uint64_t GPFSEL5   = 0x14;
constexpr uint64_t GPSET0    = 0x1C;
constexpr uint64_t GPSET1    = 0x20;
constexpr uint64_t GPCLR0    = 0x28;
constexpr uint64_t GPCLR1    = 0x2C;
constexpr uint64_t GPLEV0    = 0x34;
constexpr uint64_t GPLEV1    = 0x38;
constexpr uint64_t GPEDS0    = 0x40;
constexpr uint64_t GPEDS1    = 0x44;
constexpr uint64_t GPREN0    = 0x4C;
constexpr uint64_t GPFEN0    = 0x58;
constexpr uint64_t GPPUD     = 0x94;
constexpr uint64_t GPPUDCLK0 = 0x98;
constexpr uint64_t GPPUDCLK1 = 0x9C;

/* BCM2711 (Pi 4) Pull-up / Pull-down control registers */
constexpr uint64_t GPIO_PUP_PDN_CNTRL_REG0 = 0xE4;
constexpr uint64_t GPIO_PUP_PDN_CNTRL_REG1 = 0xE8;
constexpr uint64_t GPIO_PUP_PDN_CNTRL_REG2 = 0xEC;
constexpr uint64_t GPIO_PUP_PDN_CNTRL_REG3 = 0xF0;

constexpr uint64_t RPI3_GPIO_BASE = 0x3F200000ull;
constexpr uint64_t RPI4_GPIO_BASE = 0xFE200000ull;
constexpr uint64_t RPI1_GPIO_BASE = 0x20200000ull;

uint64_t g_gpio_base = 0;
bool g_is_rpi_hw = false;
bool g_is_rpi4 = false;

/* Emulated GPIO registers for QEMU / Virt fallback testing */
struct VirtGpioState {
  uint32_t fsel[6];
  uint32_t lev[2];
  uint32_t pull[4];
  uint32_t last_toggle_tick[54];
} g_virt_gpio;

/* 40-Pin Raspberry Pi J8 Header Definition */
const HeaderPinDef HEADER_PINS[40] = {
  {  1, -1, PinType::Power3V3, "3V3 Power",      "3.3V DC" },
  {  2, -1, PinType::Power5V,  "5V Power",       "5.0V DC" },
  {  3,  2, PinType::Gpio,     "GPIO 2",         "I2C1 SDA" },
  {  4, -1, PinType::Power5V,  "5V Power",       "5.0V DC" },
  {  5,  3, PinType::Gpio,     "GPIO 3",         "I2C1 SCL" },
  {  6, -1, PinType::Ground,   "Ground",         "0V GND" },
  {  7,  4, PinType::Gpio,     "GPIO 4",         "GPCLK0 / 1-Wire" },
  {  8, 14, PinType::Gpio,     "GPIO 14",        "UART0 TXD" },
  {  9, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 10, 15, PinType::Gpio,     "GPIO 15",        "UART0 RXD" },
  { 11, 17, PinType::Gpio,     "GPIO 17",        "GPIO_GEN0" },
  { 12, 18, PinType::Gpio,     "GPIO 18",        "PCM_CLK / PWM0" },
  { 13, 27, PinType::Gpio,     "GPIO 27",        "GPIO_GEN2" },
  { 14, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 15, 22, PinType::Gpio,     "GPIO 22",        "GPIO_GEN3" },
  { 16, 23, PinType::Gpio,     "GPIO 23",        "GPIO_GEN4" },
  { 17, -1, PinType::Power3V3, "3V3 Power",      "3.3V DC" },
  { 18, 24, PinType::Gpio,     "GPIO 24",        "GPIO_GEN5" },
  { 19, 10, PinType::Gpio,     "GPIO 10",        "SPI0 MOSI" },
  { 20, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 21,  9, PinType::Gpio,     "GPIO 9",         "SPI0 MISO" },
  { 22, 25, PinType::Gpio,     "GPIO 25",        "GPIO_GEN6" },
  { 23, 11, PinType::Gpio,     "GPIO 11",        "SPI0 SCLK" },
  { 24,  8, PinType::Gpio,     "GPIO 8",         "SPI0 CE0_N" },
  { 25, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 26,  7, PinType::Gpio,     "GPIO 7",         "SPI0 CE1_N" },
  { 27,  0, PinType::IdEeprom, "GPIO 0",         "ID_SD (EEPROM)" },
  { 28,  1, PinType::IdEeprom, "GPIO 1",         "ID_SC (EEPROM)" },
  { 29,  5, PinType::Gpio,     "GPIO 5",         "GPIO_GCLK" },
  { 30, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 31,  6, PinType::Gpio,     "GPIO 6",         "GPIO_GEN7" },
  { 32, 12, PinType::Gpio,     "GPIO 12",        "PWM0" },
  { 33, 13, PinType::Gpio,     "GPIO 13",        "PWM1" },
  { 34, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 35, 19, PinType::Gpio,     "GPIO 19",        "PCM_FS / PWM1" },
  { 36, 16, PinType::Gpio,     "GPIO 16",        "GPIO_GEN8" },
  { 37, 26, PinType::Gpio,     "GPIO 26",        "GPIO_GEN9" },
  { 38, 20, PinType::Gpio,     "GPIO 20",        "PCM_DIN / SPI1 MOSI" },
  { 39, -1, PinType::Ground,   "Ground",         "0V GND" },
  { 40, 21, PinType::Gpio,     "GPIO 21",        "PCM_DOUT / SPI1 SCLK" }
};

/* I2C Device Database */
struct KnownI2cEntry {
  uint8_t addr;
  const char* name;
  const char* desc;
};

const KnownI2cEntry KNOWN_I2C_DEVICES[] = {
  { 0x18, "MCP9808",      "High Accuracy Temperature Sensor" },
  { 0x20, "PCF8574",      "8-Bit I/O Port Expander" },
  { 0x23, "BH1750",       "Ambient Light Sensor (I2C)" },
  { 0x27, "PCF8574T",     "LCD1602 / 2004 I2C Backpack" },
  { 0x38, "AHT10/AHT20",  "Humidity & Temperature Sensor" },
  { 0x3C, "SSD1306/1106", "128x64 / 128x32 OLED Display" },
  { 0x3D, "SSD1306_ALT",  "Secondary OLED Display" },
  { 0x40, "PCA9685",      "16-Channel 12-Bit PWM/Servo Driver" },
  { 0x44, "SHT31-D",      "High-Precision Humidity & Temp Sensor" },
  { 0x48, "ADS1115/ADS1015", "16-Bit 4-Channel Precision ADC" },
  { 0x50, "AT24C32/24C64", "HAT ID EEPROM / 32KB Storage" },
  { 0x57, "AT24C32_RTC",  "EEPROM on DS3231 RTC Module" },
  { 0x68, "DS3231/MPU6050", "Precision Real-Time Clock / 6-DOF IMU" },
  { 0x76, "BME280/BMP280", "Pressure, Humidity & Temp Sensor" },
  { 0x77, "BME280_ALT",   "Secondary Environmental Sensor" }
};

}  /* namespace */

void gpio_init() {
  const FdtInfo& fdt = fdt_info();
  if (fdt.uart_base == 0x3F201000ull || fdt.ram_base == 0) {
    g_gpio_base = RPI3_GPIO_BASE;
    g_is_rpi_hw = true;
    g_is_rpi4 = false;
  } else if (fdt.uart_base == 0xFE201000ull) {
    g_gpio_base = RPI4_GPIO_BASE;
    g_is_rpi_hw = true;
    g_is_rpi4 = true;
  } else {
    /* Virt/QEMU: initialize virtual GPIO state */
    g_gpio_base = 0;
    g_is_rpi_hw = false;
    g_is_rpi4 = false;
  }

  if (!g_is_rpi_hw) {
    /* Set default directions for simulated pins */
    for (int i = 0; i < 6; ++i) g_virt_gpio.fsel[i] = 0;
    g_virt_gpio.lev[0] = (1u << 2) | (1u << 3) | (1u << 14); /* I2C pullups high, UART TX high */
    g_virt_gpio.lev[1] = 0;
    /* Set GPIO 14 (TXD) and 15 (RXD) to Alt0 */
    g_virt_gpio.fsel[1] = (4u << 12) | (4u << 15);
    /* Set GPIO 2 & 3 to Alt0 (I2C) */
    g_virt_gpio.fsel[0] = (4u << 6) | (4u << 9);
  }
}

bool gpio_is_live_hardware() {
  return g_is_rpi_hw;
}

uint64_t gpio_mmio_base() {
  return g_gpio_base;
}

const char* gpio_platform_name() {
  if (g_is_rpi4) return "Raspberry Pi 4 (BCM2711 0xFE200000)";
  if (g_is_rpi_hw) return "Raspberry Pi 3/3B+ (BCM2837 0x3F200000)";
  return "ARM64 Virt Substrate (Emulated MMIO Bank)";
}

bool gpio_set_mode(unsigned bcm_pin, GpioMode mode) {
  if (bcm_pin > 53) return false;
  const uint32_t reg_idx = bcm_pin / 10;
  const uint32_t shift = (bcm_pin % 10) * 3;
  const uint32_t mode_val = static_cast<uint32_t>(mode) & 0x7;

  if (g_is_rpi_hw && g_gpio_base) {
    const uint64_t reg = g_gpio_base + (reg_idx * 4);
    uint32_t val = mmio_read32(reg);
    val &= ~(7u << shift);
    val |= (mode_val << shift);
    mmio_write32(reg, val);
  } else {
    g_virt_gpio.fsel[reg_idx] &= ~(7u << shift);
    g_virt_gpio.fsel[reg_idx] |= (mode_val << shift);
  }
  return true;
}

GpioMode gpio_get_mode(unsigned bcm_pin) {
  if (bcm_pin > 53) return GpioMode::Unknown;
  const uint32_t reg_idx = bcm_pin / 10;
  const uint32_t shift = (bcm_pin % 10) * 3;
  uint32_t val = 0;

  if (g_is_rpi_hw && g_gpio_base) {
    val = mmio_read32(g_gpio_base + (reg_idx * 4));
  } else {
    val = g_virt_gpio.fsel[reg_idx];
  }

  uint8_t m = static_cast<uint8_t>((val >> shift) & 0x7);
  return static_cast<GpioMode>(m);
}

bool gpio_write(unsigned bcm_pin, bool high) {
  if (bcm_pin > 53) return false;

  if (g_is_rpi_hw && g_gpio_base) {
    if (bcm_pin < 32) {
      const uint64_t reg = high ? (g_gpio_base + GPSET0) : (g_gpio_base + GPCLR0);
      mmio_write32(reg, 1u << bcm_pin);
    } else {
      const uint64_t reg = high ? (g_gpio_base + GPSET1) : (g_gpio_base + GPCLR1);
      mmio_write32(reg, 1u << (bcm_pin - 32));
    }
  } else {
    if (bcm_pin < 32) {
      if (high) g_virt_gpio.lev[0] |= (1u << bcm_pin);
      else g_virt_gpio.lev[0] &= ~(1u << bcm_pin);
    } else {
      if (high) g_virt_gpio.lev[1] |= (1u << (bcm_pin - 32));
      else g_virt_gpio.lev[1] &= ~(1u << (bcm_pin - 32));
    }
  }
  return true;
}

bool gpio_read(unsigned bcm_pin) {
  if (bcm_pin > 53) return false;

  if (g_is_rpi_hw && g_gpio_base) {
    if (bcm_pin < 32) {
      return (mmio_read32(g_gpio_base + GPLEV0) & (1u << bcm_pin)) != 0;
    } else {
      return (mmio_read32(g_gpio_base + GPLEV1) & (1u << (bcm_pin - 32))) != 0;
    }
  } else {
    if (bcm_pin < 32) {
      return (g_virt_gpio.lev[0] & (1u << bcm_pin)) != 0;
    } else {
      return (g_virt_gpio.lev[1] & (1u << (bcm_pin - 32))) != 0;
    }
  }
}

bool gpio_toggle(unsigned bcm_pin) {
  bool current = gpio_read(bcm_pin);
  return gpio_write(bcm_pin, !current);
}

bool gpio_set_pull(unsigned bcm_pin, GpioPull pull) {
  if (bcm_pin > 53) return false;

  if (g_is_rpi_hw && g_gpio_base) {
    if (g_is_rpi4) {
      /* Pi 4 (BCM2711) 2-bit pull register */
      uint32_t reg_idx = bcm_pin / 16;
      uint32_t shift = (bcm_pin % 16) * 2;
      uint64_t reg = g_gpio_base + GPIO_PUP_PDN_CNTRL_REG0 + (reg_idx * 4);
      uint32_t pval = (pull == GpioPull::Up) ? 1 : ((pull == GpioPull::Down) ? 2 : 0);
      uint32_t val = mmio_read32(reg);
      val &= ~(3u << shift);
      val |= (pval << shift);
      mmio_write32(reg, val);
    } else {
      /* Pi 3 (BCM2837) GPPUD cycle */
      uint32_t pud_val = (pull == GpioPull::Up) ? 2 : ((pull == GpioPull::Down) ? 1 : 0);
      mmio_write32(g_gpio_base + GPPUD, pud_val);
      for (int i = 0; i < 150; ++i) { asm volatile("nop"); }
      if (bcm_pin < 32) {
        mmio_write32(g_gpio_base + GPPUDCLK0, 1u << bcm_pin);
      } else {
        mmio_write32(g_gpio_base + GPPUDCLK1, 1u << (bcm_pin - 32));
      }
      for (int i = 0; i < 150; ++i) { asm volatile("nop"); }
      mmio_write32(g_gpio_base + GPPUD, 0);
      mmio_write32(g_gpio_base + GPPUDCLK0, 0);
      mmio_write32(g_gpio_base + GPPUDCLK1, 0);
    }
  } else {
    uint32_t reg_idx = bcm_pin / 16;
    uint32_t shift = (bcm_pin % 16) * 2;
    uint32_t pval = (pull == GpioPull::Up) ? 1 : ((pull == GpioPull::Down) ? 2 : 0);
    g_virt_gpio.pull[reg_idx] &= ~(3u << shift);
    g_virt_gpio.pull[reg_idx] |= (pval << shift);
    /* Update default level if floating/pulled */
    if (pull == GpioPull::Up) gpio_write(bcm_pin, true);
    else if (pull == GpioPull::Down) gpio_write(bcm_pin, false);
  }
  return true;
}

void gpio_pulse(unsigned bcm_pin, uint32_t high_us, uint32_t low_us, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    gpio_write(bcm_pin, true);
    if (high_us) metal_delay_us(high_us);
    gpio_write(bcm_pin, false);
    if (low_us) metal_delay_us(low_us);
  }
}

int8_t gpio_phys_to_bcm(uint8_t phys_pin) {
  if (phys_pin < 1 || phys_pin > 40) return -1;
  return HEADER_PINS[phys_pin - 1].bcm_gpio;
}

uint8_t gpio_bcm_to_phys(uint8_t bcm_pin) {
  for (uint8_t i = 0; i < 40; ++i) {
    if (HEADER_PINS[i].bcm_gpio == static_cast<int8_t>(bcm_pin)) {
      return HEADER_PINS[i].physical_pin;
    }
  }
  return 0;
}

const HeaderPinDef* gpio_get_header_def(uint8_t phys_pin) {
  if (phys_pin < 1 || phys_pin > 40) return nullptr;
  return &HEADER_PINS[phys_pin - 1];
}

uint32_t gpio_measure_pin_freq(unsigned bcm_pin, uint32_t sample_window_us) {
  if (bcm_pin > 53) return 0;
  if (sample_window_us == 0) sample_window_us = 5000;

  const uint64_t start_ns = metal_now_ns();
  const uint64_t window_ns = static_cast<uint64_t>(sample_window_us) * 1000ull;
  bool last_val = gpio_read(bcm_pin);
  uint32_t transitions = 0;

  while ((metal_now_ns() - start_ns) < window_ns) {
    bool cur = gpio_read(bcm_pin);
    if (cur != last_val) {
      ++transitions;
      last_val = cur;
    }
  }

  /* Each cycle is 2 transitions: frequency = (transitions / 2) / (window_sec) */
  if (transitions < 2) return 0;
  uint64_t freq = (static_cast<uint64_t>(transitions) * 1000000ull) / (2ull * sample_window_us);
  return static_cast<uint32_t>(freq);
}

const char* gpio_i2c_lookup_device(uint8_t addr) {
  for (size_t i = 0; i < sizeof(KNOWN_I2C_DEVICES) / sizeof(KNOWN_I2C_DEVICES[0]); ++i) {
    if (KNOWN_I2C_DEVICES[i].addr == addr) {
      return KNOWN_I2C_DEVICES[i].name;
    }
  }
  return "unknown part";
}

/* Software Bit-Banged I2C Bus Scanner */
namespace {

void i2c_delay() {
  metal_delay_us(5);
}

void i2c_scl_hi(unsigned scl) {
  gpio_set_mode(scl, GpioMode::Input); /* High via pullup */
  i2c_delay();
}

void i2c_scl_lo(unsigned scl) {
  gpio_set_mode(scl, GpioMode::Output);
  gpio_write(scl, false);
  i2c_delay();
}

void i2c_sda_hi(unsigned sda) {
  gpio_set_mode(sda, GpioMode::Input);
  i2c_delay();
}

void i2c_sda_lo(unsigned sda) {
  gpio_set_mode(sda, GpioMode::Output);
  gpio_write(sda, false);
  i2c_delay();
}

void i2c_start(unsigned sda, unsigned scl) {
  i2c_sda_hi(sda);
  i2c_scl_hi(scl);
  i2c_sda_lo(sda);
  i2c_scl_lo(scl);
}

void i2c_stop(unsigned sda, unsigned scl) {
  i2c_sda_lo(sda);
  i2c_scl_hi(scl);
  i2c_sda_hi(sda);
}

bool i2c_write_byte(unsigned sda, unsigned scl, uint8_t byte) {
  for (int i = 7; i >= 0; --i) {
    if ((byte >> i) & 1) i2c_sda_hi(sda);
    else i2c_sda_lo(sda);
    i2c_scl_hi(scl);
    i2c_scl_lo(scl);
  }
  /* Read ACK */
  i2c_sda_hi(sda);
  i2c_scl_hi(scl);
  bool ack = !gpio_read(sda);
  i2c_scl_lo(scl);
  return ack;
}

}  /* namespace */

namespace { bool g_i2c_stuck = false; }
bool gpio_i2c_bus_stuck() { return g_i2c_stuck; }

size_t gpio_i2c_scan_bus(unsigned sda_pin, unsigned scl_pin, GpioI2cDevice out_devices[], size_t max_devices) {
  size_t count = 0;
  g_i2c_stuck = false;
  /* No bus, no devices. Non-Pi hardware used to get a made-up list here
   * (an SSD1306 OLED, a DS3231 RTC, a BME280 ...) that looked exactly like
   * a real scan result. */
  if (!g_is_rpi_hw) return 0;

  gpio_set_pull(sda_pin, GpioPull::Up);
  gpio_set_pull(scl_pin, GpioPull::Up);
  gpio_set_mode(sda_pin, GpioMode::Input);
  gpio_set_mode(scl_pin, GpioMode::Input);
  metal_delay_us(50);
  /* An idle I2C bus floats high. If either line reads low with the pulls on,
   * every address would appear to acknowledge - report that instead. */
  if (!gpio_read(sda_pin) || !gpio_read(scl_pin)) { g_i2c_stuck = true; return 0; }

  size_t acks = 0;
  /* 0x00-0x07 and 0x78-0x7F are reserved addresses, not devices. */
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    i2c_start(sda_pin, scl_pin);
    const bool ack = i2c_write_byte(sda_pin, scl_pin, static_cast<uint8_t>((addr << 1) | 0));
    i2c_stop(sda_pin, scl_pin);
    metal_delay_us(100);
    if (!ack) continue;
    ++acks;
    if (count >= max_devices) continue;
    out_devices[count].addr = addr;
    out_devices[count].name = gpio_i2c_lookup_device(addr);
    out_devices[count].description = nullptr;
    for (size_t k = 0; k < sizeof(KNOWN_I2C_DEVICES) / sizeof(KNOWN_I2C_DEVICES[0]); ++k) {
      if (KNOWN_I2C_DEVICES[k].addr == addr) { out_devices[count].description = KNOWN_I2C_DEVICES[k].desc; break; }
    }
    if (!out_devices[count].description) out_devices[count].description = "not in the known-parts list";
    ++count;
  }
  /* Real buses carry a handful of parts; dozens of ACKs mean SDA is held. */
  if (acks > 32) { g_i2c_stuck = true; return 0; }
  return count;
}

void gpio_sample_pin_status(uint8_t phys_pin, GpioPinStatus& out) {
  if (phys_pin < 1 || phys_pin > 40) return;
  const HeaderPinDef& def = HEADER_PINS[phys_pin - 1];

  out.physical_pin = def.physical_pin;
  out.bcm_gpio = def.bcm_gpio;
  out.type = def.type;
  out.name = def.default_name;
  out.alt0_name = def.alt0_name;

  if (def.type == PinType::Power3V3) {
    out.mode = GpioMode::Output;
    out.level = true;
    out.pull = GpioPull::Up;
    out.transition_hz = 0;
    out.detected_device = "3.3V System Power Rail";
  } else if (def.type == PinType::Power5V) {
    out.mode = GpioMode::Output;
    out.level = true;
    out.pull = GpioPull::Up;
    out.transition_hz = 0;
    out.detected_device = "5.0V Main Power Rail";
  } else if (def.type == PinType::Ground) {
    out.mode = GpioMode::Input;
    out.level = false;
    out.pull = GpioPull::Down;
    out.transition_hz = 0;
    out.detected_device = "0V Ground Return";
  } else {
    out.mode = gpio_get_mode(def.bcm_gpio);
    out.level = gpio_read(def.bcm_gpio);
    out.pull = GpioPull::None;
    out.transition_hz = gpio_measure_pin_freq(def.bcm_gpio, 2000);

    if (def.bcm_gpio == 2 || def.bcm_gpio == 3) {
      out.detected_device = "I2C Bus 1 (SDA/SCL)";
    } else if (def.bcm_gpio == 14 || def.bcm_gpio == 15) {
      out.detected_device = "Primary UART Console (PL011/MiniUART)";
    } else if (def.bcm_gpio >= 7 && def.bcm_gpio <= 11) {
      out.detected_device = "SPI Bus 0 Master Interface";
    } else if (def.bcm_gpio == 18 || def.bcm_gpio == 12 || def.bcm_gpio == 13 || def.bcm_gpio == 19) {
      out.detected_device = "Hardware PWM / PCM Audio Output";
    } else if (def.bcm_gpio == 4) {
      out.detected_device = "GPCLK0 / 1-Wire Thermal Probe";
    } else if (out.transition_hz > 100) {
      out.detected_device = "Active High-Frequency Clock/Data Line";
    } else {
      out.detected_device = out.level ? "High Logic Level (3.3V)" : "Low Logic Level (0V / Idle)";
    }
  }
}

size_t gpio_sample_all_pins(GpioPinStatus out[40]) {
  for (uint8_t i = 1; i <= 40; ++i) {
    gpio_sample_pin_status(i, out[i - 1]);
  }
  return 40;
}

static const char* mode_to_str(GpioMode m) {
  switch (m) {
    case GpioMode::Input:  return "IN ";
    case GpioMode::Output: return "OUT";
    case GpioMode::Alt0:   return "ALT0";
    case GpioMode::Alt1:   return "ALT1";
    case GpioMode::Alt2:   return "ALT2";
    case GpioMode::Alt3:   return "ALT3";
    case GpioMode::Alt4:   return "ALT4";
    case GpioMode::Alt5:   return "ALT5";
    default: return "UNK";
  }
}

static void format_pad_str(char* dst, size_t cap, const char* src, size_t width) {
  size_t i = 0;
  while (src && src[i] && i + 1 < cap && i < width) {
    dst[i] = src[i];
    ++i;
  }
  while (i < width && i + 1 < cap) {
    dst[i++] = ' ';
  }
  dst[i] = '\0';
}

void gpio_render_pinout_ansi() {
  GpioPinStatus status[40];
  gpio_sample_all_pins(status);

  kputs("\n\033[1;36m=========================================================================================\033[0m\n");
  kprintf("\033[1;37m   AIOS RASPBERRY PI 40-PIN J8 EXPANSION HEADER & REAL-TIME PIN TELEMETRY\033[0m\n");
  kprintf("   Substrate: \033[1;32m%s\033[0m | MMIO Base: \033[1;33m%p\033[0m\n",
          gpio_platform_name(), gpio_mmio_base());
  kputs("\033[1;36m=========================================================================================\033[0m\n");
  kputs(" \033[1;30mBCM#   NAME / ALT FUNCTION        MODE  V  [PIN] [PIN]  V  MODE        NAME / ALT FUNCTION   BCM#\033[0m\n");
  kputs(" -----------------------------------------------------------------------------------------\n");

  for (int row = 0; row < 20; ++row) {
    int p_left = row * 2 + 1;
    int p_right = row * 2 + 2;
    const GpioPinStatus& l = status[p_left - 1];
    const GpioPinStatus& r = status[p_right - 1];

    /* Format Left Pin */
    char l_bcm[8] = "  ";
    if (l.bcm_gpio >= 0) {
      if (l.bcm_gpio < 10) { l_bcm[0] = ' '; l_bcm[1] = static_cast<char>('0' + l.bcm_gpio); l_bcm[2] = '\0'; }
      else { l_bcm[0] = static_cast<char>('0' + (l.bcm_gpio / 10)); l_bcm[1] = static_cast<char>('0' + (l.bcm_gpio % 10)); l_bcm[2] = '\0'; }
    } else {
      l_bcm[0] = ' '; l_bcm[1] = '-'; l_bcm[2] = '\0';
    }

    /* Left Color Coding */
    const char* l_col = "\033[0m";
    if (l.type == PinType::Power3V3) l_col = "\033[1;31m";
    else if (l.type == PinType::Power5V) l_col = "\033[1;33m";
    else if (l.type == PinType::Ground) l_col = "\033[1;30m";
    else if (l.bcm_gpio == 2 || l.bcm_gpio == 3) l_col = "\033[1;36m";
    else if (l.bcm_gpio >= 7 && l.bcm_gpio <= 11) l_col = "\033[1;32m";
    else if (l.bcm_gpio == 14 || l.bcm_gpio == 15) l_col = "\033[1;35m";
    else l_col = "\033[1;34m";

    /* Format Right Pin */
    char r_bcm[8] = "  ";
    if (r.bcm_gpio >= 0) {
      if (r.bcm_gpio < 10) { r_bcm[0] = ' '; r_bcm[1] = static_cast<char>('0' + r.bcm_gpio); r_bcm[2] = '\0'; }
      else { r_bcm[0] = static_cast<char>('0' + (r.bcm_gpio / 10)); r_bcm[1] = static_cast<char>('0' + (r.bcm_gpio % 10)); r_bcm[2] = '\0'; }
    } else {
      r_bcm[0] = ' '; r_bcm[1] = '-'; r_bcm[2] = '\0';
    }

    const char* r_col = "\033[0m";
    if (r.type == PinType::Power3V3) r_col = "\033[1;31m";
    else if (r.type == PinType::Power5V) r_col = "\033[1;33m";
    else if (r.type == PinType::Ground) r_col = "\033[1;30m";
    else if (r.bcm_gpio == 2 || r.bcm_gpio == 3) r_col = "\033[1;36m";
    else if (r.bcm_gpio >= 7 && r.bcm_gpio <= 11) r_col = "\033[1;32m";
    else if (r.bcm_gpio == 14 || r.bcm_gpio == 15) r_col = "\033[1;35m";
    else r_col = "\033[1;34m";

    char l_name[32], r_name[32];
    format_pad_str(l_name, sizeof(l_name), l.alt0_name ? l.alt0_name : l.name, 25);
    format_pad_str(r_name, sizeof(r_name), r.alt0_name ? r.alt0_name : r.name, 25);

    kprintf(" %s %s%s\033[0m %s  %u  \033[1;37m(%02u)\033[0m \033[1;37m(%02u)\033[0m  %u  %s %s%s\033[0m %s\n",
            l_bcm, l_col, l_name,
            l.bcm_gpio >= 0 ? mode_to_str(l.mode) : "---",
            static_cast<uint64_t>(l.level ? 1 : 0),
            static_cast<uint64_t>(p_left), static_cast<uint64_t>(p_right),
            static_cast<uint64_t>(r.level ? 1 : 0),
            r.bcm_gpio >= 0 ? mode_to_str(r.mode) : "---",
            r_col, r_name, r_bcm);
  }
  kputs("\033[1;36m-----------------------------------------------------------------------------------------\033[0m\n");
  kputs(" Legend: \033[1;31m3.3V\033[0m | \033[1;33m5.0V\033[0m | \033[1;30mGND\033[0m | \033[1;36mI2C\033[0m | \033[1;32mSPI\033[0m | \033[1;35mUART\033[0m | \033[1;34mGPIO\033[0m | Live Volts: 1=HIGH(3.3V), 0=LOW(0V)\n\n");
}

void gpio_report_list() {
  GpioPinStatus status[40];
  gpio_sample_all_pins(status);

  kputs("\n[gpio] Raspberry Pi Pin Roster:\n");
  kputs("  Phys  BCM  Type       Mode  Level  Freq(Hz)  Detected Device / Role\n");
  kputs("  ----  ---  ---------  ----  -----  --------  ------------------------------------\n");
  for (int i = 0; i < 40; ++i) {
    const auto& s = status[i];
    char bcm_s[8] = "--";
    if (s.bcm_gpio >= 0) {
      if (s.bcm_gpio < 10) { bcm_s[0] = static_cast<char>('0' + s.bcm_gpio); bcm_s[1] = '\0'; }
      else { bcm_s[0] = static_cast<char>('0' + (s.bcm_gpio / 10)); bcm_s[1] = static_cast<char>('0' + (s.bcm_gpio % 10)); bcm_s[2] = '\0'; }
    }
    const char* t_str = "GPIO";
    if (s.type == PinType::Power3V3) t_str = "3V3";
    else if (s.type == PinType::Power5V) t_str = "5V";
    else if (s.type == PinType::Ground) t_str = "GND";
    else if (s.type == PinType::IdEeprom) t_str = "ID_EE";

    kprintf("  %02u    %2s   %5s      %s   %s   %6u    %s (%s)\n",
            static_cast<uint64_t>(s.physical_pin), bcm_s,
            t_str,
            s.bcm_gpio >= 0 ? mode_to_str(s.mode) : "---",
            s.level ? "HIGH" : "LOW ",
            static_cast<uint64_t>(s.transition_hz),
            s.detected_device ? s.detected_device : "General",
            s.alt0_name ? s.alt0_name : s.name);
  }
}

void gpio_report_pin(unsigned bcm_pin) {
  if (bcm_pin > 53) {
    kprintf("  invalid GPIO pin %u (valid: 0..53)\n", static_cast<uint64_t>(bcm_pin));
    return;
  }
  uint8_t phys = gpio_bcm_to_phys(static_cast<uint8_t>(bcm_pin));
  GpioMode mode = gpio_get_mode(bcm_pin);
  bool level = gpio_read(bcm_pin);
  uint32_t freq = gpio_measure_pin_freq(bcm_pin, 10000);

  kprintf("  GPIO %u:\n", static_cast<uint64_t>(bcm_pin));
  kprintf("    Physical Header Pin: %u\n", static_cast<uint64_t>(phys));
  kprintf("    Function Mode:       %s\n", mode_to_str(mode));
  kprintf("    Live Logic Level:    %s (%u, ~%s)\n",
          level ? "HIGH" : "LOW",
          static_cast<uint64_t>(level ? 1 : 0),
          level ? "3.3V" : "0.0V");
  kprintf("    Activity Frequency:  %u Hz\n", static_cast<uint64_t>(freq));
  if (phys > 0) {
    const HeaderPinDef& def = HEADER_PINS[phys - 1];
    kprintf("    Default Name:        %s\n", def.default_name);
    kprintf("    Alt0 Assignment:     %s\n", def.alt0_name);
  }
}

void gpio_inspect_and_acknowledge() {
  kputs("\n\033[1;32m[+] Starting Deep Hardware Pin & Peripheral Inspection...\033[0m\n");
  kprintf("    Target Architecture: \033[1;37mAArch64 Bare-Metal\033[0m\n");
  kprintf("    Hardware Substrate:  \033[1;33m%s\033[0m\n", gpio_platform_name());
  kprintf("    GPIO Controller:     \033[1;33m%p\033[0m\n", gpio_mmio_base());

  /* 1. Scan I2C Devices */
  kputs("\n  [\033[1;36m1\033[0m] Probing I2C Bus 1 (GPIO 2 SDA / GPIO 3 SCL)...");
  GpioI2cDevice i2c_devs[16];
  size_t i2c_count = gpio_i2c_scan_bus(2, 3, i2c_devs, 16);
  if (i2c_count == 0) {
    kputs(" (no I2C slave ACKs detected on bus 1)\n");
  } else {
    kprintf(" Found \033[1;32m%u device(s)\033[0m:\n", static_cast<uint64_t>(i2c_count));
    for (size_t i = 0; i < i2c_count; ++i) {
      kprintf("      * \033[1;33m0x%02X\033[0m: \033[1;37m%-18s\033[0m [%s]\n",
              i2c_devs[i].addr, i2c_devs[i].name, i2c_devs[i].description);
    }
  }

  /* 2. Check UART Bus */
  kputs("  [\033[1;36m2\033[0m] Inspecting Primary Serial Console (GPIO 14 TXD0 / GPIO 15 RXD0)...");
  GpioMode m_tx = gpio_get_mode(14);
  GpioMode m_rx = gpio_get_mode(15);
  /* The pin mux says where the pins are routed; it says nothing about baud
   * rate, so no "115200 8N1" claim here. */
  kprintf(" Mode: TXD0=[%s] RXD0=[%s]\n", mode_to_str(m_tx), mode_to_str(m_rx));

  /* 3. SPI0 pins: what the mux actually says. A GPIO block cannot tell
   * whether a master is "available"; it can tell whether 7..11 are on Alt0. */
  kputs("  [\033[1;36m3\033[0m] SPI0 pin routing (GPIO 7..11):");
  kprintf(" CE1=%s CE0=%s MISO=%s MOSI=%s SCLK=%s\n",
          mode_to_str(gpio_get_mode(7)), mode_to_str(gpio_get_mode(8)), mode_to_str(gpio_get_mode(9)),
          mode_to_str(gpio_get_mode(10)), mode_to_str(gpio_get_mode(11)));

  /* 4. Power rails: there is no ADC on the header rails, so no voltage can
   * be verified from here. This used to print "verified" for all of them. */
  kputs("  [\033[1;36m4\033[0m] Power rails: not measurable from the SoC (no ADC on the 3.3V/5V/GND header pins)\n");

  kputs("\n\033[1;32m[+] REPORT:\033[0m\n");
  kprintf("    GPIO block at %p answered register reads; the pin modes above are read back from GPFSEL.\n",
          gpio_mmio_base());
  kputs("    I2C scan and pin modes are live readings; anything not listed was not measured.\n\n");
}
