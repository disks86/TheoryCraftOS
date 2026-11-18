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

// The Framework Laptop 16's LED Matrix Input Module (USB 32AC:0020): 9 columns by 34 rows of white LEDs on an RP2040, reached as a USB
// serial port (CDC-ACM). Every command is the magic bytes 0x32 0xAC, a command number and its parameters; a few answer with a short
// report. Layouts follow Framework's open firmware (inputmodule-rs) as remembered: the numbers and parameter layouts are in one table in
// the .cpp; the least certain are marked there.
//
// The encoders are pure functions (tested on the host); LedMatrix drives one module over any ISerialPort.
#pragma once
#include <stdint.h>
#include "Hardware/SerialPort/ISerialPort.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/SharedPointer.hpp"

namespace hardware {
namespace ledmatrix {

constexpr uint32_t kColumns = 9, kRows = 34, kPixels = kColumns * kRows;
constexpr uint32_t kBlackWhiteBytes = (kPixels + 7) / 8;   // 39: one bit a pixel, row by row

enum class Command : uint8_t {
  Brightness = 0x00,
  Pattern = 0x01,
  Bootloader = 0x02,
  Sleep = 0x03,
  Animate = 0x04,
  Panic = 0x05,
  DrawBlackWhite = 0x06,
  StageColumn = 0x07,
  FlushColumns = 0x08,
  Version = 0x20,
};
enum class Pattern : uint8_t { Percentage = 0, Gradient = 1, DoubleGradient = 2, LotusSideways = 3, ZigZag = 4, AllOn = 5, Panic = 6, LotusTopDown = 7 };

// A command with its parameters (at most 64 bytes); returns the bytes written to `out`, 0 if they do not fit.
uint32_t encode(Command command, ConstByteView parameters, ByteView out);
// Packs a picture of kRows x kColumns brightness values (row by row; anything non-zero is on) into the 39 bytes DrawBlackWhite takes.
[[nodiscard]] bool pack_black_white(ConstByteView pixels, ByteView out);
// The firmware version from the 3-byte answer to Version: major, minor, patch; false if it is not one.
[[nodiscard]] bool decode_version(ConstByteView answer, uint8_t* major, uint8_t* minor, uint8_t* patch);

class LedMatrix {
 public:
  explicit LedMatrix(SharedPointer<ISerialPort> port) : port_(port) {}
  // Opens the port (the baud rate means nothing to a USB serial port; 115200 is what the module's tools use).
  [[nodiscard]] bool open();
  [[nodiscard]] bool set_brightness(uint8_t level);              // 0..255
  [[nodiscard]] bool show_pattern(Pattern pattern, uint8_t parameter = 0);   // (Percentage: 0..100)
  [[nodiscard]] bool show_percentage(uint8_t percent) { return show_pattern(Pattern::Percentage, percent > 100 ? 100 : percent); }
  [[nodiscard]] bool draw(ConstByteView pixels);                 // kPixels brightness values: on/off
  [[nodiscard]] bool draw_greyscale(ConstByteView pixels);       // kPixels brightness values, column by column through StageColumn + FlushColumns
  [[nodiscard]] bool set_sleep(bool sleeping);
  [[nodiscard]] bool set_animate(bool on);
  // Asks for the firmware version; poll_version() reads the answer once the serial service has brought it in.
  [[nodiscard]] bool request_version();
  [[nodiscard]] bool poll_version(uint8_t* major, uint8_t* minor, uint8_t* patch);

 private:
  bool send(Command command, ConstByteView parameters);
  SharedPointer<ISerialPort> port_;
};

}  // namespace ledmatrix
}  // namespace hardware
