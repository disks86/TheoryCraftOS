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

// The EDID of the monitor on a connector, as the video BIOS's tables wire it: the DCB outputs of the connector name an entry of the I2C
// table, which is a bit-banged DDC bus (I2C at 0x50 with the E-DDC segment pointer at 0x30, NvI2cBus), a DisplayPort AUX channel (I2C over
// AUX, NvAuxChannel), or both on one hybrid pad (a DP++ connector). A DisplayPort output is read over AUX first and over DDC when that fails
// (an HDMI/DVI monitor on a dual-mode port); the others over DDC. The bytes are what src/Hardware/Monitor/Edid's parse_edid() takes.
//
// I2C table entry types, by the numbers nouveau uses (dcb.h DCB_I2C_*): 5 NVIO_BIT (a bus: `drive`, sharing the pad of AUX channel `share`),
// 6 NVIO_AUX (an AUX channel: `aux`, or `drive` when the parser left `aux` unset), 0x80 PMGR (DCB 4.1: a bus `drive` and/or a channel `aux`).
// Types 0 and 4 (NV04/NV4E bit-banging through the CRTC registers) are the pre-Tesla chips', which this driver does not do.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvAux.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2c.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace nv {

// How one entry of the I2C table is reached.
struct I2cRoute {
  uint8_t drive = 0xff;   // the bit-banged bus, 0xff none
  uint8_t pad = 0xff;     // the hybrid pad the bus shares with an AUX channel, 0xff none
  uint8_t aux = 0xff;     // the AUX channel, 0xff none
  bool has_bus() const { return drive != 0xff; }
  bool has_aux() const { return aux != 0xff; }
};
// Decodes I2C table entry `index` (an index into BoardInfo::i2c); nothing set when there is no such entry or it is of a type not handled.
I2cRoute i2c_route(const BoardInfo& board, uint8_t index);

// The route of connector `connector` (a position in BoardInfo::connectors, which DcbOutput::connector refers to), and whether it has a
// DisplayPort output.
struct ConnectorRoute {
  I2cRoute route;
  bool displayport = false;
  bool found = false;      // some DCB output names this connector with a usable I2C entry
};
ConnectorRoute connector_route(const BoardInfo& board, uint32_t connector);

enum class EdidSource : uint8_t { None, Ddc, Aux };

// Reads the EDID of connector `connector` into *out (replacing it). Which way it came, or None when there is no route or nothing answered
// with a valid base block.
[[nodiscard]] EdidSource read_connector_edid(INvRegisters& regs, const Chip& chip, const BoardInfo& board, uint32_t connector, ByteBuffer* out);

}  // namespace nv
}  // namespace hardware
