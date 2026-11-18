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

#include "Hardware/UsbTypeC/UcsiProtocol.hpp"

namespace hardware::ucsi {

uint32_t bit_field(ConstByteView data, uint32_t offset, uint32_t width) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < width && i < 32; ++i) {
    const uint64_t bit = uint64_t{offset} + i;
    const uint64_t byte = bit / 8;
    if (byte >= data.size()) break;
    if (data[byte] & (1u << (bit % 8))) value |= 1u << i;
  }
  return value;
}

namespace {
uint32_t le16(ConstByteView d, uint32_t at) { return bit_field(d, at * 8, 16); }
uint32_t le32(ConstByteView d, uint32_t at) { return bit_field(d, at * 8, 32); }
uint32_t mul_div(uint32_t a, uint32_t b, uint32_t divisor) { return static_cast<uint32_t>(uint64_t{a} * b / divisor); }
}  // namespace

uint64_t control_get_pdos(uint32_t connector, bool partner, uint32_t offset, uint32_t count, bool source) {
  const uint32_t n = count == 0 ? 0 : (count > 4 ? 3 : count - 1);
  return control_word(kGetPdos, connector_field(connector) | (partner ? 1ull << 23 : 0) | (uint64_t{offset & 0xFF} << 24) | (uint64_t{n} << 32) |
                                     (source ? 1ull << 34 : 0));
}

uint64_t control_get_alternate_modes(AltModeRecipient recipient, uint32_t connector, uint32_t offset, uint32_t count) {
  const uint32_t n = count <= 1 ? 0 : 1;
  return control_word(kGetAlternateModes, (uint64_t{static_cast<uint8_t>(recipient) & 7u} << 16) | (uint64_t{connector & 0x7F} << 24) |
                                              (uint64_t{offset & 0xFF} << 32) | (uint64_t{n} << 40));
}

bool decode_capability(ConstByteView data, uint16_t version, Capability* out) {
  *out = Capability();
  if (data.size() < 8) return false;
  out->attributes = le32(data, 0);
  out->connectors = static_cast<uint8_t>(bit_field(data, 32, 7));
  out->features = version >= kVersion2_0 ? bit_field(data, 40, 24) : bit_field(data, 40, 8);
  out->alt_modes = static_cast<uint8_t>(bit_field(data, 64, 8));
  out->bc_version = static_cast<uint16_t>(le16(data, 10));
  out->pd_version = static_cast<uint16_t>(le16(data, 12));
  out->typec_version = static_cast<uint16_t>(le16(data, 14));
  return true;
}

bool decode_connector_capability(ConstByteView data, UsbTypeCConnectorCaps* out) {
  *out = UsbTypeCConnectorCaps();
  if (data.size() < 2) return false;
  const uint32_t mode = data[0], flags = data[1];
  out->dfp = mode & 0x01;
  out->ufp = mode & 0x02;
  out->drp = mode & 0x04;
  out->audio_accessory = mode & 0x08;
  out->debug_accessory = mode & 0x10;
  out->usb2 = mode & 0x20;
  out->usb3 = mode & 0x40;
  out->alt_modes = mode & 0x80;
  out->provider = flags & 0x01;
  out->consumer = flags & 0x02;
  out->swap_to_dfp = flags & 0x04;
  out->swap_to_ufp = flags & 0x08;
  out->swap_to_source = flags & 0x10;
  out->swap_to_sink = flags & 0x20;
  return true;
}

bool decode_connector_status(ConstByteView data, uint16_t version, UsbTypeCConnector* out, uint16_t* change) {
  if (data.size() < 9) return false;
  *change = static_cast<uint16_t>(bit_field(data, 0, 16));
  const uint32_t mode = bit_field(data, 16, 3);
  out->connected = bit_field(data, 19, 1) != 0;
  const bool provider = bit_field(data, 20, 1) != 0;
  out->power_mode = mode == 0 ? UsbTypeCPowerMode::None : mode == 1 ? UsbTypeCPowerMode::UsbDefault : mode == 2 ? UsbTypeCPowerMode::BatteryCharging
                  : mode == 3 ? UsbTypeCPowerMode::PowerDelivery : mode == 4 ? UsbTypeCPowerMode::TypeC1_5A : mode == 5 ? UsbTypeCPowerMode::TypeC3A
                  : UsbTypeCPowerMode::Unknown;
  out->partner_usb = bit_field(data, 21, 1) != 0;
  out->partner_alt_mode = bit_field(data, 22, 1) != 0;
  out->partner_usb4_gen3 = version >= kVersion2_0 && bit_field(data, 23, 1) != 0;
  out->partner_usb4_gen4 = version >= kVersion2_0 && bit_field(data, 24, 1) != 0;
  const uint32_t type = bit_field(data, 29, 3);
  out->contract.rdo = bit_field(data, 32, 32);
  out->battery_charging = static_cast<uint8_t>(bit_field(data, 64, 2));
  out->provider_limited = static_cast<uint8_t>(bit_field(data, 66, 4));
  out->pd_revision = version >= kVersion1_2 && data.size() >= 11 ? static_cast<uint16_t>(bit_field(data, 70, 16)) : 0;
  out->orientation_known = version >= kVersion2_0 && data.size() >= 12;
  out->flipped = out->orientation_known && bit_field(data, 86, 1) != 0;
  out->sink_path = version >= kVersion2_0 && data.size() >= 12 && bit_field(data, 87, 1) != 0;
  out->power_reading_valid = version >= kVersion2_1 && data.size() >= 19 && bit_field(data, 89, 1) != 0;
  if (out->power_reading_valid) {
    const uint32_t current_scale = bit_field(data, 90, 3), voltage_scale = bit_field(data, 125, 4);   // (5 mA and 5 mV units)
    out->vbus_peak_ma = bit_field(data, 93, 16) * current_scale * 5;
    out->vbus_avg_ma = bit_field(data, 109, 16) * current_scale * 5;
    out->vbus_mv = bit_field(data, 129, 16) * voltage_scale * 5;
  } else {
    out->vbus_peak_ma = out->vbus_avg_ma = out->vbus_mv = 0;
  }
  if (!out->connected) {
    out->partner = UsbTypeCPartner::None;
    out->data_role = UsbTypeCDataRole::None;
    out->power_role = UsbTypeCPowerRole::None;
    out->power_mode = UsbTypeCPowerMode::None;
    out->partner_usb = out->partner_alt_mode = out->partner_usb4_gen3 = out->partner_usb4_gen4 = false;
    out->contract.rdo = 0;
    return true;
  }
  switch (type) {
    case 1: out->partner = UsbTypeCPartner::Host; out->data_role = UsbTypeCDataRole::Device; break;           // a DFP is attached: we are the device
    case 2: out->partner = UsbTypeCPartner::Device; out->data_role = UsbTypeCDataRole::Host; break;
    case 3: out->partner = UsbTypeCPartner::PoweredCable; out->data_role = UsbTypeCDataRole::None; break;
    case 4: out->partner = UsbTypeCPartner::PoweredCableAndDevice; out->data_role = UsbTypeCDataRole::Host; break;
    case 5: out->partner = UsbTypeCPartner::DebugAccessory; out->data_role = UsbTypeCDataRole::None; break;
    case 6: out->partner = UsbTypeCPartner::AudioAccessory; out->data_role = UsbTypeCDataRole::None; break;
    default: out->partner = UsbTypeCPartner::Unknown; out->data_role = UsbTypeCDataRole::None; break;
  }
  out->power_role = provider ? UsbTypeCPowerRole::Source : UsbTypeCPowerRole::Sink;
  return true;
}

UsbPdo decode_pdo(uint32_t raw) {
  UsbPdo p;
  p.raw = raw;
  switch (raw >> 30) {
    case 0:
      p.kind = UsbPdoKind::Fixed;
      p.min_mv = p.max_mv = ((raw >> 10) & 0x3FF) * 50;
      p.max_ma = (raw & 0x3FF) * 10;
      p.max_mw = mul_div(p.max_mv, p.max_ma, 1000);
      p.dual_role_power = raw & (1u << 29);
      p.unconstrained_power = raw & (1u << 27);
      p.usb_communication = raw & (1u << 26);
      p.dual_role_data = raw & (1u << 25);
      break;
    case 1:
      p.kind = UsbPdoKind::Battery;
      p.max_mv = ((raw >> 20) & 0x3FF) * 50;
      p.min_mv = ((raw >> 10) & 0x3FF) * 50;
      p.max_mw = (raw & 0x3FF) * 250;
      break;
    case 2:
      p.kind = UsbPdoKind::Variable;
      p.max_mv = ((raw >> 20) & 0x3FF) * 50;
      p.min_mv = ((raw >> 10) & 0x3FF) * 50;
      p.max_ma = (raw & 0x3FF) * 10;
      p.max_mw = mul_div(p.max_mv, p.max_ma, 1000);
      break;
    default:
      switch ((raw >> 28) & 3) {
        case 0:
          p.kind = UsbPdoKind::Pps;
          p.max_mv = ((raw >> 17) & 0xFF) * 100;
          p.min_mv = ((raw >> 8) & 0xFF) * 100;
          p.max_ma = (raw & 0x7F) * 50;
          p.max_mw = mul_div(p.max_mv, p.max_ma, 1000);
          break;
        case 1:
          p.kind = UsbPdoKind::EprAvs;
          p.max_mv = ((raw >> 17) & 0x1FF) * 100;
          p.min_mv = ((raw >> 8) & 0xFF) * 100;
          p.max_mw = (raw & 0xFF) * 1000;
          break;
        case 2:
          p.kind = UsbPdoKind::SprAvs;      // (PD 3.2: 9 V to 20 V; the current at 15 V and at 20 V)
          p.min_mv = 9000;
          p.max_mv = 20000;
          p.max_ma = ((raw >> 10) & 0x3FF) * 10;
          p.max_mw = mul_div(15000, p.max_ma, 1000);
          break;
        default: p.kind = UsbPdoKind::Unknown; break;
      }
      break;
  }
  return p;
}

uint32_t decode_pdos(ConstByteView data, ArrayView<UsbPdo> out) {
  uint32_t n = 0;
  for (uint64_t at = 0; at + 4 <= data.size() && n < out.size(); at += 4) {
    const uint32_t raw = le32(data, static_cast<uint32_t>(at));
    if (raw == 0) break;                    // (an empty object ends the list)
    out[n++] = decode_pdo(raw);
  }
  return n;
}

UsbPdContract decode_contract(uint32_t rdo, ArrayView<const UsbPdo> source_pdos) {
  UsbPdContract c;
  c.rdo = rdo;
  c.object_position = static_cast<uint8_t>((rdo >> 28) & 0xF);
  if (rdo == 0 || c.object_position == 0) return c;
  c.valid = true;
  const UsbPdo* pdo = c.object_position <= source_pdos.size() ? &source_pdos[c.object_position - 1u] : nullptr;
  c.kind = pdo ? pdo->kind : UsbPdoKind::Fixed;
  switch (c.kind) {
    case UsbPdoKind::Fixed:
    case UsbPdoKind::Variable:
      c.current_ma = ((rdo >> 10) & 0x3FF) * 10;
      c.max_current_ma = (rdo & 0x3FF) * 10;
      c.voltage_mv = pdo ? pdo->max_mv : 0;   // (a variable supply is somewhere in its range: its upper end)
      c.power_mw = mul_div(c.voltage_mv, c.current_ma, 1000);
      break;
    case UsbPdoKind::Battery:
      c.power_mw = ((rdo >> 10) & 0x3FF) * 250;
      c.voltage_mv = pdo->max_mv;
      c.current_ma = c.voltage_mv ? mul_div(c.power_mw, 1000, c.voltage_mv) : 0;
      break;
    case UsbPdoKind::Pps:
      c.voltage_mv = ((rdo >> 9) & 0xFFF) * 20;
      c.current_ma = (rdo & 0x7F) * 50;
      c.power_mw = mul_div(c.voltage_mv, c.current_ma, 1000);
      break;
    case UsbPdoKind::EprAvs:
    case UsbPdoKind::SprAvs:
      c.voltage_mv = ((rdo >> 9) & 0xFFF) * 25;
      c.current_ma = (rdo & 0x7F) * 50;
      c.power_mw = mul_div(c.voltage_mv, c.current_ma, 1000);
      break;
    case UsbPdoKind::Unknown: c.valid = false; break;
  }
  return c;
}

uint32_t decode_alternate_modes(ConstByteView data, ArrayView<UsbAltMode> out) {
  uint32_t n = 0;
  for (uint64_t at = 0; at + 6 <= data.size() && n < out.size(); at += 6) {
    UsbAltMode m;
    m.svid = static_cast<uint16_t>(le16(data, static_cast<uint32_t>(at)));
    if (m.svid == 0) break;
    m.vdo = le32(data, static_cast<uint32_t>(at + 2));
    if (m.svid == kSvidDisplayPort) {
      m.kind = UsbAltModeKind::DisplayPort;
      const bool receptacle = m.vdo & (1u << 6);    // the pin assignments of a plug are given from the other side
      m.dp_pin_assignments = static_cast<uint8_t>((receptacle ? m.vdo >> 16 : m.vdo >> 8) & 0x3F);
    } else if (m.svid == kSvidThunderbolt) {
      m.kind = UsbAltModeKind::Thunderbolt;
    }
    out[n++] = m;
  }
  return n;
}

bool decode_cable_property(ConstByteView data, UsbCable* out) {
  *out = UsbCable();
  if (data.size() < 5) return false;
  const uint32_t speed = le16(data, 0), exponent = speed & 3, mantissa = speed >> 2;
  out->speed_mbps = exponent == 3 ? mantissa * 1000 : exponent == 2 ? mantissa : exponent == 1 ? mantissa / 1000 : mantissa / 1000000;
  out->current_ma = data[2] * 50u;
  const uint32_t flags = data[3];
  out->vbus_in_cable = flags & 0x01;
  out->active = flags & 0x02;
  out->directional = flags & 0x04;
  const uint32_t plug = (flags >> 3) & 3;
  out->plug = plug == 0 ? UsbCablePlug::TypeA : plug == 1 ? UsbCablePlug::TypeB : plug == 2 ? UsbCablePlug::TypeC : UsbCablePlug::Other;
  out->alt_mode_support = flags & 0x20;
  out->latency = data[4] & 0x0F;
  out->valid = true;
  return true;
}

bool decode_error_status(ConstByteView data, uint16_t* error) {
  if (data.size() < 2) return false;
  *error = static_cast<uint16_t>(le16(data, 0));
  return true;
}

}  // namespace hardware::ucsi
