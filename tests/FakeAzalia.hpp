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

#pragma once

#include <stdint.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

// The audio endpoints of the display engine as registers: an index/data pair per endpoint (INDEX at 0x17a8 + 4 * pin, DATA the next one), the indexed registers behind them kept per
// endpoint, and the default configuration (index 0x56) answering with a port connectivity per endpoint (bits 31:30: 1 is "none"). Every write of an indexed register is logged.
struct FakeAzalia final : IVcnRegisters {
  static constexpr uint32_t kFirst = 0x17a8, kLast = 0x17c5, kDefaultConfig = 0x56;
  uint32_t index[8] = {};
  uint32_t connectivity[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  std::map<uint64_t, uint32_t> regs;       // pin << 32 | index
  struct Write {
    uint32_t pin, index, value;
  };
  std::vector<Write> writes;

  static bool owns(uint32_t r) { return r >= kFirst && r <= kLast && ((r - kFirst) & 3) < 2; }
  uint32_t value(uint32_t pin, uint32_t idx) const {
    const auto it = regs.find(uint64_t{pin} << 32 | idx);
    return it == regs.end() ? 0 : it->second;
  }
  void set(uint32_t pin, uint32_t idx, uint32_t v) { regs[uint64_t{pin} << 32 | idx] = v; }

  uint32_t read(uint32_t r) override {
    if (!owns(r)) return 0;
    const uint32_t pin = (r - kFirst) / 4;
    if (((r - kFirst) & 3) == 0) return index[pin];
    return index[pin] == kDefaultConfig ? (connectivity[pin] << 30) | 0x00123456u : value(pin, index[pin]);
  }
  void write(uint32_t r, uint32_t v) override {
    if (!owns(r)) return;
    const uint32_t pin = (r - kFirst) / 4;
    if (((r - kFirst) & 3) == 0) {
      index[pin] = v;
      return;
    }
    set(pin, index[pin], v);
    writes.push_back({pin, index[pin], v});
  }
};

}  // namespace hardware
