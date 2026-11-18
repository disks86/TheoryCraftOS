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

#include "Hardware/Platform/Framework/LedMatrix.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace ledmatrix {

// The protocol (inputmodule-rs, as remembered): magic 0x32 0xAC, then the command. Brightness: one byte. Pattern: the pattern number, then
// for Percentage the percentage. Sleep/Animate: one byte, 0 or 1 (no byte: a question, answered with one byte). DrawBlackWhite: 39 bytes.
// StageColumn: the column, then 34 brightness bytes; FlushColumns: none. Version: no parameters, answered with major, minor, patch
// (the patch byte's meaning - patch or pre-release flag - is the least certain part).
namespace {
constexpr uint8_t kMagic0 = 0x32, kMagic1 = 0xAC;
constexpr uint32_t kMaxParameters = 64;
}  // namespace

uint32_t encode(Command command, ConstByteView parameters, ByteView out) {
  if (parameters.size() > kMaxParameters) return 0;
  ByteWriter w(out);
  w.u8(kMagic0);
  w.u8(kMagic1);
  w.u8(static_cast<uint8_t>(command));
  w.bytes(parameters);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

bool pack_black_white(ConstByteView pixels, ByteView out) {
  if (pixels.size() != kPixels || out.size() < kBlackWhiteBytes) return false;
  out.first(kBlackWhiteBytes).fill(0);
  for (uint32_t i = 0; i < kPixels; ++i)
    if (pixels[i]) out[i / 8] = static_cast<uint8_t>(out[i / 8] | (1u << (i % 8)));
  return true;
}

bool decode_version(ConstByteView a, uint8_t* major, uint8_t* minor, uint8_t* patch) {
  if (a.size() < 3) return false;
  *major = a[0];
  *minor = a[1];
  *patch = a[2];
  return true;
}

bool LedMatrix::open() {
  if (!port_) return false;
  SerialConfig config;
  config.baud = 115200;
  return port_->state() == SerialState::Open || port_->open(config);
}

bool LedMatrix::send(Command command, ConstByteView parameters) {
  if (!port_ || port_->state() != SerialState::Open) return false;
  uint8_t message[3 + kMaxParameters];
  const uint32_t n = encode(command, parameters, ByteView(message));
  if (n == 0 || port_->bytes_writable() < n) return false;
  return port_->write(ConstByteView(message).first(n)) == n;   // (whole commands only: it fit, so all of it was taken)
}

bool LedMatrix::set_brightness(uint8_t level) {
  const uint8_t p[1] = {level};
  return send(Command::Brightness, ConstByteView(p));
}

bool LedMatrix::show_pattern(Pattern pattern, uint8_t parameter) {
  const uint8_t p[2] = {static_cast<uint8_t>(pattern), parameter};
  return send(Command::Pattern, ConstByteView(p).first(pattern == Pattern::Percentage ? 2 : 1));
}

bool LedMatrix::draw(ConstByteView pixels) {
  uint8_t packed[kBlackWhiteBytes];
  if (!pack_black_white(pixels, ByteView(packed))) return false;
  return send(Command::DrawBlackWhite, ConstByteView(packed));
}

bool LedMatrix::draw_greyscale(ConstByteView pixels) {
  if (pixels.size() != kPixels) return false;
  for (uint32_t column = 0; column < kColumns; ++column) {
    uint8_t p[1 + kRows];
    p[0] = static_cast<uint8_t>(column);
    for (uint32_t row = 0; row < kRows; ++row) p[1 + row] = pixels[row * kColumns + column];
    if (!send(Command::StageColumn, ConstByteView(p))) return false;
  }
  return send(Command::FlushColumns, ConstByteView());
}

bool LedMatrix::set_sleep(bool sleeping) {
  const uint8_t p[1] = {static_cast<uint8_t>(sleeping ? 1 : 0)};
  return send(Command::Sleep, ConstByteView(p));
}

bool LedMatrix::set_animate(bool on) {
  const uint8_t p[1] = {static_cast<uint8_t>(on ? 1 : 0)};
  return send(Command::Animate, ConstByteView(p));
}

bool LedMatrix::request_version() {
  if (port_) port_->discard_input();
  return send(Command::Version, ConstByteView());
}

bool LedMatrix::poll_version(uint8_t* major, uint8_t* minor, uint8_t* patch) {
  if (!port_ || port_->bytes_readable() < 3) return false;
  uint8_t answer[3];
  if (port_->read(ByteView(answer)) != 3) return false;
  return decode_version(ConstByteView(answer), major, minor, patch);
}

}  // namespace ledmatrix
}  // namespace hardware
