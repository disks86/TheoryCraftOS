/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Apple's gmux (graphics multiplexer) of the dual-GPU MacBook Pros, after Linux drivers/platform/x86/apple-gmux.c: an ACPI device (_HID
// "APP000B") whose _CRS gives an I/O port window. It moves the built-in panel, its DDC lines and (on some models) the external port between
// the integrated and the discrete GPU, cuts the discrete GPU's power, and drives the panel's backlight.
//
// Two register layouts are spoken here:
//  - classic (port I/O, 2008-2010 models): register N is the port base+N; 32-bit registers are read with one 32-bit in, written as four bytes.
//  - indexed (2011 and later, before the T2): the registers sit behind a mailbox. A read writes the register number to base+0xD0, waits for the
//    handshake bit (bit 0 of base+0xD4) and reads the value from base+0xC2; a write puts the value in base+0xC2.. first and the register number
//    in base+0xD4. The layout is recognised by its scratch registers at base+0xCC..0xCE (0xAA, 0x55 written there read back as 0x55AA) after
//    the classic version registers read all ones.
// Not done: the MMIO gmux of the T2-era models (the _CRS has a memory range instead; find_resource() reports it and the caller does not use
// it), the GPE interrupt the gmux raises when a switch or a power change finished (the driver does not wait for it), and the DMI quirk lists
// Linux keeps (which models can switch the external port: the caller says so).
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Backlight/IBacklight.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp"
#include "Hardware/HardwareMonitor/PortIo.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The I/O ports the gmux needs: the byte ports of IPortIo and one 32-bit read (the classic layout's 32-bit registers and the indexed layout's
// value register are read with a single 32-bit in).
class IGmuxPorts : public IPortIo {
 public:
  virtual uint32_t read32(uint16_t port) = 0;

 protected:
  constexpr IGmuxPorts() = default;
  ~IGmuxPorts() = default;
};

// The in/out instructions.
class X86GmuxPorts final : public IGmuxPorts {
 public:
  uint8_t read8(uint16_t port) override;
  void write8(uint16_t port, uint8_t value) override;
  uint32_t read32(uint16_t port) override;
};

// What the gmux device's _CRS describes: an I/O window (the classic and indexed gmux) or a memory range (the T2-era MMIO gmux, not driven).
struct AppleGmuxResource {
  bool has_io = false;
  uint16_t io_base = 0, io_length = 0;
  bool has_memory = false;
  uint64_t memory_base = 0;
  uint64_t memory_length = 0;
};

// Reads the first I/O range (I/O port, fixed I/O, Word or DWord I/O address space descriptors) and the first memory range (32-bit fixed
// memory, DWord or QWord memory) of a resource template. False if the template is malformed (a descriptor that runs past the end) or has
// neither range. The bytes come from firmware: every length is checked.
[[nodiscard]] bool apple_gmux_parse_resources(ConstByteView crs, AppleGmuxResource* out);

enum class AppleGmuxLayout : uint8_t { None, Classic, Indexed };

class AppleGmux final : public IGpuMux, public IGpuPlatformPower, public IBacklight {
 public:
  // Register numbers (offsets from the I/O base), as apple-gmux.c names them.
  static constexpr uint16_t kVersionMajor = 0x04, kVersionMinor = 0x05, kVersionRelease = 0x06;
  static constexpr uint16_t kSwitchDisplay = 0x10, kSwitchGetDisplay = 0x11, kInterruptEnable = 0x14, kInterruptStatus = 0x16;
  static constexpr uint16_t kSwitchDdc = 0x28, kSwitchExternal = 0x40, kSwitchGetExternal = 0x41, kDiscretePower = 0x50;
  static constexpr uint16_t kMaxBrightness = 0x70, kBrightness = 0x74;
  static constexpr uint16_t kIndexValue = 0xC2, kIndexRead = 0xD0, kIndexWrite = 0xD4;
  static constexpr uint16_t kIndexScratch = 0xCC;                    // 0xCC, 0xCD, 0xCE: the indexed layout's detection registers
  static constexpr uint16_t kMinIoLength = kBrightness + 4;          // the classic window (GMUX_MIN_IO_LEN)
  static constexpr uint16_t kMinIndexedIoLength = kIndexWrite + 1;   // the indexed window reaches the write-index port
  static constexpr uint32_t kBrightnessMask = 0x00FFFFFF;
  static constexpr uint32_t kHandshakePolls = 200;                    // as Linux: 200 polls 100 us apart (20 ms)
  static constexpr uint32_t kHandshakeDelayUs = 100;

  // `delay_us` sleeps between the polls of the indexed handshake (nullptr: no sleep, for tests). `external_switchable`: the external display
  // port is wired through the gmux on this model (Linux's DMI list), so switch_to() moves it with the panel.
  using DelayFn = void (*)(uint32_t microseconds);
  AppleGmux(IGmuxPorts& io, uint16_t io_base, uint16_t io_length, DelayFn delay_us = nullptr, bool external_switchable = false)
      : io_(io), base_(io_base), length_(io_length), delay_us_(delay_us), external_switchable_(external_switchable) {}

  // The present APP000B device of the namespace (its _STA, where it has one, says so), or nullptr.
  static acpi::Node* find_device(acpi::AmlInterpreter& vm);
  // Evaluates the device's _CRS and parses it. False if there is no _CRS, it is not a buffer or it does not parse.
  [[nodiscard]] static bool find_resource(acpi::AmlInterpreter& vm, acpi::Node* device, AppleGmuxResource* out);

  // Finds out which layout answers at the window and reads the version. False (and layout() None) if the window is too small or nothing
  // answers (all ones, the scratch registers do not keep what is written).
  [[nodiscard]] bool detect();
  AppleGmuxLayout layout() const { return layout_; }
  uint8_t version_major() const { return version_[0]; }
  uint8_t version_minor() const { return version_[1]; }
  uint8_t version_release() const { return version_[2]; }

  // IHardware (through IBacklight): initialize() is detect().
  const char* name() const override { return "Apple gmux"; }
  bool initialize() override { return detect(); }
  void shutdown() override {}

  // IGpuMux. switch_to() moves the DDC lines, the panel and (when switchable) the external port together, as Linux's gmux_switchto(). The
  // classic gmux can also move the DDC lines alone (to read the EDID through the other GPU); the indexed one moves them only with the panel.
  [[nodiscard]] bool active(GpuMuxTarget* out) override;
  [[nodiscard]] bool switch_to(GpuMuxTarget target) override;
  [[nodiscard]] bool switch_ddc(GpuMuxTarget target) override;
  // Where the DDC lines and the external port are connected now.
  [[nodiscard]] bool active_ddc(GpuMuxTarget* out);
  [[nodiscard]] bool active_external(GpuMuxTarget* out);
  bool external_switchable() const { return external_switchable_; }

  // IGpuPlatformPower: the discrete GPU's power through GMUX_PORT_DISCRETE_POWER (1 then 3 turns it on, 1 then 0 off).
  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::AppleGmux; }
  [[nodiscard]] bool power_off() override;
  [[nodiscard]] bool power_on() override;
  bool discrete_powered() const { return discrete_on_; }   // (what was last written; the gmux has no register to read it back)

  // The panel's backlight, in the gmux's own units (0..max_brightness()).
  [[nodiscard]] bool max_brightness(uint32_t* out);
  [[nodiscard]] bool brightness(uint32_t* out);
  [[nodiscard]] bool set_brightness(uint32_t value);
  // IBacklight, in percent of the maximum.
  uint32_t steps() const override { return max_brightness_ == 0 ? 0 : max_brightness_ + 1; }
  bool percent(uint8_t* out) override;
  bool set_percent(uint8_t percent) override;

  // The gmux registers, in whichever layout detect() found. False if nothing was detected or the indexed handshake timed out.
  [[nodiscard]] bool read8(uint16_t reg, uint8_t* out);
  [[nodiscard]] bool write8(uint16_t reg, uint8_t value);
  [[nodiscard]] bool read32(uint16_t reg, uint32_t* out);
  [[nodiscard]] bool write32(uint16_t reg, uint32_t value);
  uint32_t handshake_timeouts() const { return handshake_timeouts_; }

 private:
  uint16_t port(uint16_t reg) const { return static_cast<uint16_t>(base_ + reg); }
  bool window_fits(uint16_t length) const;
  bool is_indexed();
  bool index_wait_ready();
  bool index_wait_complete();
  bool write_switch(uint16_t reg, GpuMuxTarget target, uint8_t integrated, uint8_t discrete);
  bool read_switch(uint16_t reg, uint8_t integrated, GpuMuxTarget* out);
  void delay() const { if (delay_us_) delay_us_(kHandshakeDelayUs); }

  IGmuxPorts& io_;
  uint16_t base_, length_;
  DelayFn delay_us_;
  bool external_switchable_;
  AppleGmuxLayout layout_ = AppleGmuxLayout::None;
  uint8_t version_[3] = {};
  uint32_t max_brightness_ = 0;
  bool discrete_on_ = true;            // the firmware leaves the discrete GPU on
  uint32_t handshake_timeouts_ = 0;
  kernel::Mutex mutex_;                // one register access at a time: the indexed mailbox is a multi-step transaction
};

}  // namespace hardware
