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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2cEdid.hpp"

namespace hardware {
namespace nv {

namespace {
// dcb.h DCB_I2C_* (I2cPortType::NvioBit, NvioAux, Pmgr)
constexpr uint8_t kTypeNvioBit = 5, kTypeNvioAux = 6, kTypePmgr = 0x80;
constexpr uint8_t kPmgrUnused = 0x1f;
}  // namespace

I2cRoute i2c_route(const BoardInfo& board, uint8_t index) {
  I2cRoute r;
  const uint32_t count = board.i2c_count < BoardInfo::kMaxI2c ? board.i2c_count : BoardInfo::kMaxI2c;
  if (index >= count) return r;
  const I2cPort& port = board.i2c[index];
  switch (static_cast<uint8_t>(port.type)) {
    case kTypeNvioBit:
      r.drive = port.drive;
      if (port.share != 0xff) r.pad = port.share;
      break;
    case kTypeNvioAux:
      r.aux = port.aux != 0xff ? port.aux : port.drive;
      break;
    case kTypePmgr:
      if (port.drive != 0xff && port.drive < kPmgrUnused) r.drive = port.drive;
      if (port.aux != 0xff && port.aux < kPmgrUnused) r.aux = port.aux;
      if (r.drive != 0xff) r.pad = port.share != 0xff && port.share < kPmgrUnused ? port.share : r.aux;
      break;
    default:
      break;
  }
  return r;
}

ConnectorRoute connector_route(const BoardInfo& board, uint32_t connector) {
  ConnectorRoute c;
  const uint32_t outputs = board.output_count < BoardInfo::kMaxOutputs ? board.output_count : BoardInfo::kMaxOutputs;
  for (uint32_t i = 0; i < outputs; ++i) {
    const DcbOutput& o = board.outputs[i];
    if (o.type == DcbOutputType::Unused || o.type == DcbOutputType::Eol || o.connector != connector || o.i2c_index == 0xf) continue;
    const I2cRoute r = i2c_route(board, o.i2c_index);
    if (!r.has_bus() && !r.has_aux()) continue;
    const bool dp = o.type == DcbOutputType::Dp;
    if (!c.found || (dp && !c.displayport)) {
      c.route = r;
      c.displayport = dp;
      c.found = true;
    }
  }
  return c;
}

EdidSource read_connector_edid(INvRegisters& regs, const Chip& chip, const BoardInfo& board, uint32_t connector, ByteBuffer* out) {
  const ConnectorRoute c = connector_route(board, connector);
  if (!c.found) return EdidSource::None;
  const I2cVariant variant = i2c_variant_for(chip);
  if (c.displayport && c.route.has_aux()) {
    NvAuxChannel aux(regs, variant, c.route.aux);
    if (aux.valid() && aux.read_edid(out)) return EdidSource::Aux;
  }
  if (c.route.has_bus()) {
    NvI2cBus bus(regs, variant, c.route.drive, c.route.pad);
    if (bus.valid() && i2c_edid::read(bus, out)) return EdidSource::Ddc;
  }
  return EdidSource::None;
}

}  // namespace nv
}  // namespace hardware
