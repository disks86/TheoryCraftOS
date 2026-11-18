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

// What the video BIOS says about the board (nouveau's nvkm/subdev/bios: the BIT table and the DCB, I2C, GPIO and connector tables). The
// parser (NvBios) fills these from the image; the display, I2C, thermal and GPIO code reads them.
#pragma once
#include <stdint.h>

namespace hardware {
namespace nv {

// DCB output types (dcb.h DCB_OUTPUT_*).
enum class DcbOutputType : uint8_t { Analog = 0, Tv = 1, Tmds = 2, Lvds = 3, Dp = 6, Wfd = 8, Eol = 14, Unused = 15 };

struct DcbOutput {
  DcbOutputType type = DcbOutputType::Unused;
  uint8_t i2c_index = 0;            // into the I2C table (0xf: none)
  uint8_t heads = 0;                // the heads that may drive it (bit per head)
  uint8_t connector = 0;            // into the connector table
  uint8_t bus = 0;
  uint8_t location = 0;             // 0 on chip, 1 off chip (an external encoder)
  uint8_t or_mask = 0;              // the output resources (SOR/PIOR/DAC) it can use (bit per OR)
  uint8_t sorconf_link = 0;         // TMDS/DP/LVDS: which links of the SOR (1 A, 2 B, 3 both)
  uint8_t dp_max_link_rate = 0;     // DP: in 0.27 Gb/s units (6 RBR, 0xa HBR, 0x14 HBR2, 0x1e HBR3), 0 unknown
  uint8_t dp_max_lanes = 0;         // DP: 1, 2 or 4
  uint32_t raw[2] = {};             // the entry's two words as read
  // (added by NvBios) DCB 4.x: the external encoder's address when location != 0; nouveau's dcb_outp_hasht/hashm keys, which the DP info
  // and other per-output BIOS tables are matched by: hasht = extdev << 8 | location << 4 | type, hashm = heads << 8 | link << 6 | or.
  uint8_t extdev = 0;
  uint16_t hasht = 0, hashm = 0;
};

enum class I2cPortType : uint8_t {
  NvIo = 0, Pcf8584 = 1, NvBit = 4, Aux = 6, AuxCh = 5, Pmgr = 0x80, Unused = 0xff,
  // (added by NvBios) nouveau's names for the same values (dcb.h DCB_I2C_*): 0 is the NV04-style bit-banged port in the CRTC registers
  // (drive/sense are CRTC indices), 4 the NV4E bit-banged one, 5 the NVIO bit-banged port (not an AUX channel, whatever AuxCh suggests),
  // 6 the NVIO AUX channel, 0x80 the PMGR-managed port of DCB 4.1 (drive = the I2C port, aux = the AUX port).
  Nv04Bit = 0, Nv4eBit = 4, NvioBit = 5, NvioAux = 6,
};
struct I2cPort {
  I2cPortType type = I2cPortType::Unused;
  uint8_t drive = 0, sense = 0;     // the CRTC registers / pad numbers, by type
  uint8_t share = 0xff;             // the AUX channel shared with (DP), 0xff none
  uint8_t aux = 0xff;               // the AUX channel, 0xff none
  uint8_t raw[4] = {};
};

struct GpioEntry {
  uint8_t line = 0xff;              // the GPIO pin
  uint8_t function = 0xff;          // DCB_GPIO_* (0x01 panel power, 0x09 fan PWM, 0x3d fan sense (some boards 0x29), 0x51 hpd...), 0xff unused
  uint8_t log0 = 0, log1 = 0;       // the levels that mean off and on
  bool output = false;
  bool param = false;               // (added by NvBios) nouveau's dcb_gpio_func.param (the entry's top bit); `output` is not in the table nouveau parses: left false
};

struct ConnectorEntry {
  uint8_t type = 0xff;              // DCB_CONNECTOR_* (0x00 VGA, 0x30 DVI-I, 0x31 DVI-D, 0x40 LVDS, 0x46 DP, 0x47 eDP, 0x61 HDMI ...)
  uint8_t index = 0;
  uint8_t hpd = 0xff;               // the hot plug GPIO function (0xff none)
  // (added by NvBios) nouveau's nvbios_connE: location (bits 3:0 of byte 1), the HPD lines as a mask (`hpd` is the GPIO function of the lowest),
  // the DP AUX lines mask, and the entry as read.
  uint8_t location = 0;
  uint8_t hpd_mask = 0;
  uint8_t dp_mask = 0;
  uint8_t raw[4] = {};
};

struct BoardInfo {
  static constexpr uint32_t kMaxOutputs = 16, kMaxI2c = 16, kMaxGpio = 32, kMaxConnectors = 16;
  uint8_t bios_version[5] = {};     // major.chip.minor... as the image has it
  uint8_t dcb_version = 0;          // 0x20, 0x30, 0x40, 0x41
  DcbOutput outputs[kMaxOutputs];
  uint32_t output_count = 0;
  I2cPort i2c[kMaxI2c];
  uint32_t i2c_count = 0;
  uint8_t i2c_default[2] = {0xff, 0xff};   // the default indices for DDC and the "other" bus
  GpioEntry gpio[kMaxGpio];
  uint32_t gpio_count = 0;
  ConnectorEntry connectors[kMaxConnectors];
  uint32_t connector_count = 0;
  bool truncated = false;           // some table had more entries than fit or ran past the image
};

}  // namespace nv
}  // namespace hardware
