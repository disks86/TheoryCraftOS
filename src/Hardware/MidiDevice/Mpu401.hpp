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

// MIDI output on the hardware: an MPU-401 compatible port (the MIDI port of ISA / PCI sound cards and of the emulated ones: a data port at
// `base` and a status / command port at base + 1, 0x330 being the usual base). It is put in UART mode, where every byte written is sent
// on at once, so the messages go straight to whatever is plugged in - a hardware synthesizer, or the card's own wavetable synth.
//
// The port is reached through an IMidiPortIo (the x86 I/O instructions on the machine, a fake in the tests). initialize() looks for the
// interface first: a status port that reads all ones (nothing decodes the address), or that never acknowledges the reset, is not an MPU-401
// and is left alone.
#pragma once
#include "Kernel/Sync.hpp"
#include "Midi/MidiTypes.hpp"

namespace hardware {

class IMidiPortIo {
 public:
  virtual uint8_t read8(uint16_t port) = 0;
  virtual void write8(uint16_t port, uint8_t value) = 0;

 protected:
  constexpr IMidiPortIo() = default;
  ~IMidiPortIo() = default;
};

class Mpu401Output final : public midi::IMidiSink {
 public:
  static constexpr uint16_t kUsualBase = 0x330;
  static constexpr uint32_t kSpinLimit = 200000;   // status reads to wait for the interface

  Mpu401Output(IMidiPortIo& io, uint16_t base) : io_(io), base_(base) {}

  // Resets the interface and puts it in UART mode; false if no MPU-401 answers at the base (nothing is changed then).
  bool initialize();
  bool present() const { return present_; }
  uint16_t base() const { return base_; }

  void send(uint8_t status, uint8_t data1, uint8_t data2) override;
  void reset() override;

 private:
  bool wait_ready_to_write();
  bool wait_data_ready();
  bool write_command(uint8_t command);
  bool write_data(uint8_t value);
  bool expect_ack();

  IMidiPortIo& io_;
  uint16_t base_;
  bool present_ = false;
  kernel::Mutex mutex_;
};

}  // namespace hardware
