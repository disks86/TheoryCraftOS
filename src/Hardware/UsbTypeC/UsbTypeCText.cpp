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

// The words for a USB-C connector's state, for the hardware list and the Properties window.
#include "Hardware/UsbTypeC/IUsbTypeC.hpp"

namespace hardware {

const char* usb_type_c_partner_name(UsbTypeCPartner partner) {
  switch (partner) {
    case UsbTypeCPartner::None: return "nothing";
    case UsbTypeCPartner::Host: return "a host";
    case UsbTypeCPartner::Device: return "a device";
    case UsbTypeCPartner::PoweredCable: return "a powered cable";
    case UsbTypeCPartner::PoweredCableAndDevice: return "a device through a powered cable";
    case UsbTypeCPartner::DebugAccessory: return "a debug accessory";
    case UsbTypeCPartner::AudioAccessory: return "an audio adapter";
    case UsbTypeCPartner::Unknown: break;
  }
  return "something unknown";
}

const char* usb_type_c_data_role_name(UsbTypeCDataRole role) {
  switch (role) {
    case UsbTypeCDataRole::Host: return "Host";
    case UsbTypeCDataRole::Device: return "Device";
    case UsbTypeCDataRole::None: break;
  }
  return "None";
}

const char* usb_type_c_power_role_name(UsbTypeCPowerRole role) {
  switch (role) {
    case UsbTypeCPowerRole::Source: return "Source (gives power)";
    case UsbTypeCPowerRole::Sink: return "Sink (takes power)";
    case UsbTypeCPowerRole::None: break;
  }
  return "None";
}

const char* usb_type_c_power_mode_name(UsbTypeCPowerMode mode) {
  switch (mode) {
    case UsbTypeCPowerMode::None: return "None";
    case UsbTypeCPowerMode::UsbDefault: return "USB default";
    case UsbTypeCPowerMode::BatteryCharging: return "USB Battery Charging";
    case UsbTypeCPowerMode::PowerDelivery: return "USB Power Delivery";
    case UsbTypeCPowerMode::TypeC1_5A: return "Type-C 1.5 A";
    case UsbTypeCPowerMode::TypeC3A: return "Type-C 3 A";
    case UsbTypeCPowerMode::Unknown: break;
  }
  return "Unknown";
}

const char* usb_pdo_kind_name(UsbPdoKind kind) {
  switch (kind) {
    case UsbPdoKind::Fixed: return "Fixed";
    case UsbPdoKind::Battery: return "Battery";
    case UsbPdoKind::Variable: return "Variable";
    case UsbPdoKind::Pps: return "PPS";
    case UsbPdoKind::EprAvs: return "EPR AVS";
    case UsbPdoKind::SprAvs: return "SPR AVS";
    case UsbPdoKind::Unknown: break;
  }
  return "Unknown";
}

const char* usb_alt_mode_name(const UsbAltMode& mode) {
  switch (mode.kind) {
    case UsbAltModeKind::DisplayPort: return "DisplayPort";
    case UsbAltModeKind::Thunderbolt: return "Thunderbolt";
    case UsbAltModeKind::Other: break;
  }
  return "Vendor mode";
}

namespace {

// A small text builder over a fixed buffer: everything is cut to fit and the text stays zero-terminated.
class Text {
 public:
  explicit Text(ArrayView<char> out) : out_(out) {
    if (!out_.empty()) out_[0] = 0;
  }
  void add(const char* s) {
    for (uint32_t i = 0; s[i]; ++i) put(s[i]);
  }
  void number(uint64_t v) {
    char digits[20];
    uint32_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v && n < sizeof digits);
    while (n) put(digits[--n]);
  }
  // 20000 -> "20.0", 3250 -> "3.25": thousandths with one or two decimals
  void milli(uint32_t v) {
    number(v / 1000);
    put('.');
    const uint32_t frac = v % 1000;
    put(static_cast<char>('0' + frac / 100));
    if (frac % 100) put(static_cast<char>('0' + frac / 10 % 10));
  }
  void hex(uint32_t v, uint32_t digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    add("0x");
    for (uint32_t i = digits; i > 0; --i) put(kDigits[(v >> ((i - 1) * 4)) & 15]);
  }
  StringView view() const { return StringView(out_.data(), length_); }
  void clear() {
    length_ = 0;
    if (!out_.empty()) out_[0] = 0;
  }

 private:
  void put(char c) {
    if (out_.empty() || length_ + 1 >= out_.size()) return;
    out_[length_++] = c;
    out_[length_] = 0;
  }
  ArrayView<char> out_;
  uint32_t length_ = 0;
};

void add_pdo(Text& t, const UsbPdo& p) {
  t.add(usb_pdo_kind_name(p.kind));
  t.add(" ");
  if (p.min_mv != p.max_mv) {
    t.milli(p.min_mv);
    t.add("-");
  }
  t.milli(p.max_mv);
  t.add(" V");
  if (p.max_ma) {
    t.add(" ");
    t.milli(p.max_ma);
    t.add(" A");
  } else if (p.max_mw) {
    t.add(" ");
    t.number(p.max_mw / 1000);
    t.add(" W");
  }
}

void add_contract(Text& t, const UsbPdContract& c) {
  if (c.voltage_mv) {
    t.milli(c.voltage_mv);
    t.add(" V ");
  }
  t.milli(c.current_ma);
  t.add(" A");
  if (c.power_mw) {
    t.add(" (");
    t.milli(c.power_mw);
    t.add(" W)");
  }
}

}  // namespace

void usb_type_c_summary(const UsbTypeCConnector& c, ArrayView<char> out) {
  Text t(out);
  if (!c.status_valid) {
    t.add("status unknown");
    return;
  }
  if (!c.connected) {
    t.add("nothing connected");
    return;
  }
  t.add(usb_type_c_partner_name(c.partner));
  if (c.power_role == UsbTypeCPowerRole::Sink) t.add(", charging from it");
  else if (c.power_role == UsbTypeCPowerRole::Source) t.add(", powered from here");
  if (c.contract.valid) {
    t.add(", PD ");
    add_contract(t, c.contract);
  } else if (c.power_mode != UsbTypeCPowerMode::None) {
    t.add(", ");
    t.add(usb_type_c_power_mode_name(c.power_mode));
  }
  for (uint32_t i = 0; i < c.alt_mode_count && i < kUsbTypeCMaxAltModes; ++i)
    if (c.alt_modes[i].kind != UsbAltModeKind::Other) {
      t.add(", ");
      t.add(usb_alt_mode_name(c.alt_modes[i]));
    }
}

void usb_type_c_describe(const UsbTypeCConnector& c, IPropertySink& sink) {
  char buffer[120];
  Text t{ArrayView<char>(buffer)};
  sink.add("Type", "USB Type-C connector (UCSI)");
  sink.add_number("Connector", c.number);
  t.clear();
  if (c.caps.drp) t.add("dual role");
  else if (c.caps.dfp) t.add("host only");
  else if (c.caps.ufp) t.add("device only");
  if (c.caps.usb3) t.add(c.caps.drp || c.caps.dfp || c.caps.ufp ? ", USB 3" : "USB 3");
  else if (c.caps.usb2) t.add(c.caps.drp || c.caps.dfp || c.caps.ufp ? ", USB 2" : "USB 2");
  if (c.caps.alt_modes) t.add(", alternate modes");
  if (c.caps.provider) t.add(", can give power");
  if (c.caps.consumer) t.add(", can take power");
  if (!t.view().empty()) sink.add("Capabilities", t.view());
  if (!c.status_valid) {
    sink.add("Status", "Unknown (the policy manager did not answer)");
    return;
  }
  sink.add_flag("Connected", c.connected);
  if (!c.connected) return;
  t.clear();
  t.add(usb_type_c_partner_name(c.partner));
  sink.add("Partner", t.view());
  sink.add("Data role", usb_type_c_data_role_name(c.data_role));
  sink.add("Power role", usb_type_c_power_role_name(c.power_role));
  sink.add("Power", usb_type_c_power_mode_name(c.power_mode));
  if (c.partner_usb4_gen4) sink.add("Partner speed", "USB4 Gen 4");
  else if (c.partner_usb4_gen3) sink.add("Partner speed", "USB4 Gen 3");
  if (c.pd_revision) {
    t.clear();
    t.number(c.pd_revision >> 8);
    t.add(".");
    t.number((c.pd_revision >> 4) & 15);
    sink.add("PD revision", t.view());
  }
  if (c.orientation_known) sink.add("Orientation", c.flipped ? "Flipped" : "Normal");
  if (c.contract.valid) {
    t.clear();
    add_contract(t, c.contract);
    t.add(", object ");
    t.number(c.contract.object_position);
    t.add(" (");
    t.add(usb_pdo_kind_name(c.contract.kind));
    t.add(")");
    sink.add("Contract", t.view());
  }
  if (c.power_reading_valid) {
    t.clear();
    t.milli(c.vbus_mv);
    t.add(" V, ");
    t.milli(c.vbus_avg_ma);
    t.add(" A average");
    sink.add("VBUS", t.view());
  }
  static const char* const kSlots[kUsbTypeCMaxPdos] = {"Source PDO 1", "Source PDO 2", "Source PDO 3", "Source PDO 4", "Source PDO 5", "Source PDO 6", "Source PDO 7"};
  for (uint32_t i = 0; i < c.source_pdo_count && i < kUsbTypeCMaxPdos; ++i) {
    t.clear();
    add_pdo(t, c.source_pdos[i]);
    sink.add(kSlots[i], t.view());
  }
  for (uint32_t i = 0; i < c.alt_mode_count && i < kUsbTypeCMaxAltModes; ++i) {
    const UsbAltMode& m = c.alt_modes[i];
    t.clear();
    t.add(usb_alt_mode_name(m));
    t.add(" (SVID ");
    t.hex(m.svid, 4);
    t.add(")");
    if (m.kind == UsbAltModeKind::DisplayPort && m.dp_pin_assignments) {
      t.add(", pins ");
      for (uint32_t pin = 0; pin < 6; ++pin)
        if (m.dp_pin_assignments & (1u << pin)) {
          const char letter[2] = {static_cast<char>('A' + pin), 0};
          t.add(letter);
        }
    }
    sink.add("Alternate mode", t.view());
  }
  if (c.cable.valid) {
    t.clear();
    t.add(c.cable.active ? "active" : "passive");
    if (c.cable.speed_mbps) {
      t.add(", ");
      t.number(c.cable.speed_mbps);
      t.add(" Mb/s");
    }
    if (c.cable.current_ma) {
      t.add(", ");
      t.milli(c.cable.current_ma);
      t.add(" A");
    }
    if (c.cable.plug == UsbCablePlug::TypeA) t.add(", Type-A plug");
    else if (c.cable.plug == UsbCablePlug::TypeB) t.add(", Type-B plug");
    sink.add("Cable", t.view());
  }
}

}  // namespace hardware
