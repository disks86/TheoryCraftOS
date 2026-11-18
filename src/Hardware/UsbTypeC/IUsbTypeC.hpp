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

// The USB Type-C connectors of the machine as the rest of the system sees them: for each connector whether something is plugged in, what it is
// (a host, a device, a charger, an accessory, a cable), who is the USB host (data role), who gives power (power role, the UCSI "power direction"),
// the Power Delivery contract they agreed on (voltage, current, power), the power the source offers (its PDOs), the alternate modes of the partner
// (DisplayPort, Thunderbolt) and the cable. The facts come from the platform's policy manager through UCSI (UcsiCore); IUsbTypeC hands out copies,
// so a reader on any thread gets a consistent picture without holding anything.
#pragma once
#include <stdint.h>
#include "Hardware/DeviceProperties.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

constexpr uint32_t kUsbTypeCMaxConnectors = 8;
constexpr uint32_t kUsbTypeCMaxPdos = 7;            // a source offers at most 7 objects in Standard Power Range
constexpr uint32_t kUsbTypeCMaxAltModes = 8;

// Who is on the other end (UCSI "connector partner type").
enum class UsbTypeCPartner : uint8_t { None, Host, Device, PoweredCable, PoweredCableAndDevice, DebugAccessory, AudioAccessory, Unknown };
// Which side is the USB host.
enum class UsbTypeCDataRole : uint8_t { None, Host, Device };
// Which side gives power (UCSI's power direction: provider = source, consumer = sink).
enum class UsbTypeCPowerRole : uint8_t { None, Source, Sink };
// How the power was settled (UCSI "power operation mode").
enum class UsbTypeCPowerMode : uint8_t { None, UsbDefault, BatteryCharging, PowerDelivery, TypeC1_5A, TypeC3A, Unknown };

enum class UsbPdoKind : uint8_t { Fixed, Battery, Variable, Pps, EprAvs, SprAvs, Unknown };
// One power data object (USB PD 3.x section 6.4.1), decoded.
struct UsbPdo {
  uint32_t raw = 0;
  UsbPdoKind kind = UsbPdoKind::Unknown;
  uint32_t min_mv = 0, max_mv = 0;    // equal for a fixed supply
  uint32_t max_ma = 0;                // 0 where the object gives power instead
  uint32_t max_mw = 0;                // battery and EPR AVS objects; for the others max_mv * max_ma
  bool dual_role_power = false, usb_communication = false, unconstrained_power = false, dual_role_data = false;   // (the first fixed object)
};

// The contract in force (from the request data object and the object it points at).
struct UsbPdContract {
  bool valid = false;
  uint8_t object_position = 0;        // 1-based, into the source PDOs
  UsbPdoKind kind = UsbPdoKind::Unknown;
  uint32_t voltage_mv = 0;            // 0 when the source's PDOs could not be read
  uint32_t current_ma = 0;            // the operating current
  uint32_t max_current_ma = 0;        // the maximum (fixed / variable) operating current
  uint32_t power_mw = 0;
  uint32_t rdo = 0;
};

enum class UsbAltModeKind : uint8_t { DisplayPort, Thunderbolt, Other };
struct UsbAltMode {
  uint16_t svid = 0;
  uint32_t vdo = 0;                   // the mode's VDO from Discover Modes
  UsbAltModeKind kind = UsbAltModeKind::Other;
  uint8_t dp_pin_assignments = 0;     // DisplayPort: the pin assignments (bit 0 = A ... bit 5 = F) the partner offers
};

enum class UsbCablePlug : uint8_t { TypeA, TypeB, TypeC, Other, Unknown };
struct UsbCable {
  bool valid = false;
  uint32_t speed_mbps = 0;
  uint32_t current_ma = 0;
  bool vbus_in_cable = false, active = false, directional = false, alt_mode_support = false;
  UsbCablePlug plug = UsbCablePlug::Unknown;
  uint8_t latency = 0;                // UCSI encoding: 1 = <10 ns (~1 m) ... 8 = >80 ns, 9 = >1 us optical
};

// What one connector can do (GET_CONNECTOR_CAPABILITY).
struct UsbTypeCConnectorCaps {
  bool dfp = false, ufp = false, drp = false, audio_accessory = false, debug_accessory = false, usb2 = false, usb3 = false, alt_modes = false;
  bool provider = false, consumer = false, swap_to_dfp = false, swap_to_ufp = false, swap_to_source = false, swap_to_sink = false;
};

struct UsbTypeCConnector {
  uint8_t number = 0;                 // UCSI's connector number, 1-based
  UsbTypeCConnectorCaps caps;
  bool status_valid = false;          // false: the last GET_CONNECTOR_STATUS failed, the fields below are not current
  bool connected = false;
  UsbTypeCPartner partner = UsbTypeCPartner::None;
  UsbTypeCDataRole data_role = UsbTypeCDataRole::None;
  UsbTypeCPowerRole power_role = UsbTypeCPowerRole::None;
  UsbTypeCPowerMode power_mode = UsbTypeCPowerMode::None;
  bool partner_usb = false, partner_alt_mode = false, partner_usb4_gen3 = false, partner_usb4_gen4 = false;
  uint8_t battery_charging = 0;       // 0 not charging, 1 nominal, 2 slow, 3 trickle
  uint8_t provider_limited = 0;       // the reasons the provider capabilities are limited (bit 0 power budget lowered, bit 1 budget exceeded)
  uint16_t pd_revision = 0;           // UCSI 1.2 and later: the PD version in use (BCD, 0x0300); 0 if not reported
  bool orientation_known = false, flipped = false;   // UCSI 2.0 and later
  bool sink_path = false;             // UCSI 2.0 and later: the sink path is enabled
  bool power_reading_valid = false;   // UCSI 2.1 and later: VBUS as measured
  uint32_t vbus_mv = 0, vbus_avg_ma = 0, vbus_peak_ma = 0;
  UsbPdContract contract;
  // The source's capabilities: the partner's PDOs when this connector is the sink, its own when it is the source.
  UsbPdo source_pdos[kUsbTypeCMaxPdos];
  uint32_t source_pdo_count = 0;
  UsbAltMode alt_modes[kUsbTypeCMaxAltModes];   // the partner's (recipient SOP)
  uint32_t alt_mode_count = 0;
  UsbCable cable;
};

// The whole picture. `generation` counts the changes, so a reader can tell whether it has to look again.
struct UsbTypeCSnapshot {
  bool present = false;               // a policy manager answered
  uint16_t ucsi_version = 0;          // BCD: 0x0120 = 1.2, 0x0200 = 2.0
  uint16_t pd_version = 0, typec_version = 0, bc_version = 0;   // BCD, from GET_CAPABILITY
  uint32_t attributes = 0;            // GET_CAPABILITY bmAttributes
  uint32_t features = 0;              // GET_CAPABILITY bmOptionalFeatures
  uint8_t alt_mode_total = 0;         // bNumAltModes: the alternate modes the platform supports
  uint32_t connector_count = 0;
  UsbTypeCConnector connectors[kUsbTypeCMaxConnectors];
  uint32_t generation = 0;
  uint16_t last_error = 0;            // GET_ERROR_STATUS of the last command that failed (bit 0 unrecognized command, ...)
};

class IUsbTypeC {
 public:
  virtual bool present() const = 0;
  virtual uint32_t connector_count() const = 0;
  // A copy of connector `index` (0-based); false if there is no such connector.
  [[nodiscard]] virtual bool connector(uint32_t index, UsbTypeCConnector* out) const = 0;
  [[nodiscard]] virtual bool snapshot(UsbTypeCSnapshot* out) const = 0;
  virtual uint32_t generation() const = 0;

 protected:
  constexpr IUsbTypeC() = default;
  ~IUsbTypeC() = default;
};

const char* usb_type_c_partner_name(UsbTypeCPartner partner);
const char* usb_type_c_data_role_name(UsbTypeCDataRole role);
const char* usb_type_c_power_role_name(UsbTypeCPowerRole role);
const char* usb_type_c_power_mode_name(UsbTypeCPowerMode mode);
const char* usb_pdo_kind_name(UsbPdoKind kind);
const char* usb_alt_mode_name(const UsbAltMode& mode);
// A one-line summary for the hardware list ("device, sink, PD 20.0 V 3.25 A, DisplayPort"), zero-terminated and cut to fit `out`.
void usb_type_c_summary(const UsbTypeCConnector& connector, ArrayView<char> out);
// The connector's facts for a Properties window.
void usb_type_c_describe(const UsbTypeCConnector& connector, IPropertySink& sink);

}  // namespace hardware
