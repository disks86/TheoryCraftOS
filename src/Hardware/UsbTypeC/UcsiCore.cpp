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

#include "Hardware/UsbTypeC/UcsiCore.hpp"

namespace hardware {

using namespace ucsi;

namespace {
constexpr uint32_t kTransportRetries = 3;   // reads of CCI that may fail in a row before the transport counts as gone

uint32_t min32(uint64_t a, uint64_t b) { return static_cast<uint32_t>(a < b ? a : b); }
}  // namespace

const char* UcsiCore::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::Timeout: return "timed out";
    case Result::Busy: return "busy (cancelled)";
    case Result::Error: return "error";
    case Result::NotSupported: return "not supported";
    case Result::Transport: return "transport failed";
    case Result::Malformed: return "malformed answer";
  }
  return "?";
}

void UcsiCore::note(uint32_t cci) {
  const uint32_t connector = cci_connector(cci);
  if (connector == acknowledging_) return;       // (the change being acknowledged shows until the acknowledgement is done)
  if (connector >= 1 && connector <= 32) pending_ |= 1u << (connector - 1);
}

UcsiCore::Result UcsiCore::wait_for(uint32_t bits, uint32_t timeout_us, uint32_t* cci) {
  const uint64_t start = clock_.now_us();
  uint32_t pause = timing_.first_poll_us ? timing_.first_poll_us : 1;
  // (bounded by looks as well as by time: a clock that does not move must not hang the caller)
  const uint64_t max_looks = uint64_t{timeout_us} / pause + 16;
  uint32_t failures = 0;
  for (uint64_t look = 0; look < max_looks; ++look) {
    uint32_t value = 0;
    if (!transport_.read_cci(&value)) {
      if (++failures >= kTransportRetries) return Result::Transport;
    } else {
      failures = 0;
      note(value);
      *cci = value;
      if ((value & bits) && !(value & kCciBusy)) return Result::Ok;
    }
    if (clock_.now_us() - start >= timeout_us) break;
    clock_.sleep_us(pause);
    if (pause < timing_.max_poll_us) pause = pause * 2 > timing_.max_poll_us ? timing_.max_poll_us : pause * 2;
  }
  ++timeouts_;
  return Result::Timeout;
}

UcsiCore::Result UcsiCore::acknowledge(bool command, uint32_t connector) {
  const uint64_t control = control_word(kAckCcCi, (command ? kAckCommandComplete : 0) | (connector ? kAckConnectorChange : 0));
  if (!transport_.write_control(control)) return Result::Transport;
  uint32_t cci = 0;
  return wait_for(kCciAckComplete, timing_.command_timeout_us, &cci);
}

void UcsiCore::cancel() {
  ++cancels_;
  if (!transport_.write_control(control_word(kCancel))) return;
  uint32_t cci = 0;
  if (wait_for(kCciCancelComplete | kCciCommandComplete, timing_.cancel_timeout_us, &cci) == Result::Ok) (void)acknowledge(true, 0);
}

UcsiCore::Result UcsiCore::reset() {
  pending_ = 0;
  if (!transport_.write_control(control_word(kPpmReset))) return Result::Transport;
  const uint64_t start = clock_.now_us();
  const uint32_t pause = timing_.reset_poll_us ? timing_.reset_poll_us : 1;
  const uint64_t max_looks = uint64_t{timing_.reset_timeout_us} / pause + 16;
  uint32_t failures = 0;
  for (uint64_t look = 0; look < max_looks; ++look) {
    uint32_t cci = 0;
    if (!transport_.read_cci(&cci)) {
      if (++failures >= kTransportRetries) return Result::Transport;
    } else {
      failures = 0;
      if (cci & kCciResetComplete) {
        pending_ = 0;
        return Result::Ok;
      }
      if (cci != 0 && !transport_.write_control(control_word(kPpmReset))) return Result::Transport;   // still doing something else: reset again
    }
    if (clock_.now_us() - start >= timing_.reset_timeout_us) break;
    clock_.sleep_us(pause);
  }
  ++timeouts_;
  return Result::Timeout;
}

UcsiCore::Result UcsiCore::execute(uint64_t control, ByteView data, uint32_t* length, uint32_t ack_connector) {
  struct Acknowledging {           // (for the whole command: its CCI shows the connector until the acknowledgement)
    uint32_t& slot;
    uint32_t saved;
    Acknowledging(uint32_t& s, uint32_t connector) : slot(s), saved(s) { if (connector) slot = connector; }
    ~Acknowledging() { slot = saved; }
  } acknowledging(acknowledging_, ack_connector);
  *length = 0;
  ++commands_;
  if (!transport_.write_control(control)) return Result::Transport;
  uint32_t cci = 0;
  const Result waited = wait_for(kCciCommandComplete | kCciError | kCciNotSupported, timing_.command_timeout_us, &cci);
  if (waited == Result::Timeout) {
    if (cci & kCciBusy) {          // the PPM took the command and never finished it
      cancel();
      return Result::Busy;
    }
    return waited;
  }
  if (waited != Result::Ok) return waited;
  if (cci & kCciNotSupported) {
    const Result r = acknowledge(true, ack_connector);
    return r == Result::Ok ? Result::NotSupported : r;
  }
  if (cci & kCciError) {
    const Result r = acknowledge(true, ack_connector);
    if (r != Result::Ok) return r;
    if (!in_error_status_ && control_command(control) != kGetErrorStatus) {
      in_error_status_ = true;
      uint8_t status[16] = {};
      uint32_t got = 0;
      uint16_t error = 0;
      if (execute(control_word(kGetErrorStatus), ByteView(status), &got) == Result::Ok &&
          decode_error_status(ConstByteView(status, min32(got, sizeof status)), &error))
        last_error_ = error;
      in_error_status_ = false;
    }
    return Result::Error;
  }
  const uint32_t reported = cci_length(cci);
  const uint32_t capacity = transport_.message_capacity();
  if (reported > capacity || capacity > kMessageBytesV2) {     // more than MESSAGE IN holds: the CCI is not to be trusted
    const Result r = acknowledge(true, ack_connector);
    return r == Result::Ok ? Result::Malformed : r;
  }
  uint8_t message[kMessageBytesV2] = {};
  if (reported != 0 && !transport_.read_message_in(ByteView(message, reported))) return Result::Transport;
  const uint32_t copied = min32(reported, data.size());
  for (uint32_t i = 0; i < copied; ++i) data[i] = message[i];
  const Result r = acknowledge(true, ack_connector);
  if (r != Result::Ok) return r;
  *length = reported;
  return Result::Ok;
}

UcsiCore::Result UcsiCore::initialize(UsbTypeCSnapshot* out) {
  const uint32_t generation = out->generation;
  *out = UsbTypeCSnapshot();
  out->generation = generation;
  if (!transport_.read_version(&version_)) return Result::Transport;
  out->ucsi_version = version_;
  Result r = reset();
  if (r != Result::Ok) return r;
  uint8_t data[kMessageBytesV2] = {};
  uint32_t length = 0;
  r = execute(control_word(kSetNotificationEnable, kNotifyCommandComplete | kNotifyError), ByteView(data), &length);
  if (fatal(r)) return r;
  r = execute(control_word(kGetCapability), ByteView(data), &length);
  if (r != Result::Ok) return r;
  Capability cap;
  if (!decode_capability(ConstByteView(data, min32(length, sizeof data)), version_, &cap)) return Result::Malformed;
  features_ = cap.features;
  out->attributes = cap.attributes;
  out->features = cap.features;
  out->alt_mode_total = cap.alt_modes;
  out->bc_version = cap.bc_version;
  out->pd_version = cap.pd_version;
  out->typec_version = cap.typec_version;
  out->connector_count = cap.connectors < kUsbTypeCMaxConnectors ? cap.connectors : kUsbTypeCMaxConnectors;
  for (uint32_t i = 0; i < out->connector_count; ++i) {
    UsbTypeCConnector& c = out->connectors[i];
    c.number = static_cast<uint8_t>(i + 1);
    r = execute(control_word(kGetConnectorCapability, connector_field(c.number)), ByteView(data), &length);
    if (fatal(r)) return r;
    if (r == Result::Ok && !decode_connector_capability(ConstByteView(data, min32(length, sizeof data)), &c.caps)) c.caps = UsbTypeCConnectorCaps();
    bool changed = false;
    r = read_status(out, i, 0, &changed);
    if (fatal(r)) return r;
  }
  r = execute(control_word(kSetNotificationEnable, kNotifyAll), ByteView(data), &length);
  if (fatal(r)) return r;
  out->present = true;
  out->last_error = last_error_;
  return Result::Ok;
}

UcsiCore::Result UcsiCore::read_pdos(UsbTypeCConnector* c) {
  // the partner's source capabilities when this side is the sink, this side's when it is the source
  const bool partner = c->power_role == UsbTypeCPowerRole::Sink;
  UsbPdo pdos[kUsbTypeCMaxPdos];
  uint32_t count = 0;
  for (uint32_t offset = 0; offset < kUsbTypeCMaxPdos;) {
    const uint32_t want = kUsbTypeCMaxPdos - offset < 4 ? kUsbTypeCMaxPdos - offset : 4;
    uint8_t data[16] = {};
    uint32_t length = 0;
    const Result r = execute(control_get_pdos(c->number, partner, offset, want, true), ByteView(data), &length);
    if (r != Result::Ok) {
      if (fatal(r)) return r;
      break;
    }
    const uint32_t got = decode_pdos(ConstByteView(data, min32(length, sizeof data)), ArrayView<UsbPdo>(pdos + count, kUsbTypeCMaxPdos - count));
    count += got;
    if (got < want) break;
    offset += got;
  }
  for (uint32_t i = 0; i < count; ++i) c->source_pdos[i] = pdos[i];
  c->source_pdo_count = count;
  return Result::Ok;
}

UcsiCore::Result UcsiCore::read_alt_modes(UsbTypeCConnector* c) {
  uint32_t count = 0;
  for (uint32_t offset = 0; offset < kUsbTypeCMaxAltModes; ++offset) {
    uint8_t data[16] = {};
    uint32_t length = 0;
    // (one at a time, as Linux does: some PPMs answer a request for two wrongly)
    const Result r = execute(control_get_alternate_modes(AltModeRecipient::Sop, c->number, offset, 1), ByteView(data), &length);
    if (r != Result::Ok) {
      if (fatal(r)) return r;
      break;
    }
    UsbAltMode mode[2];
    if (decode_alternate_modes(ConstByteView(data, min32(length, sizeof data)), ArrayView<UsbAltMode>(mode)) == 0) break;
    c->alt_modes[count++] = mode[0];
  }
  c->alt_mode_count = count;
  return Result::Ok;
}

UcsiCore::Result UcsiCore::read_details(UsbTypeCConnector* c) {
  c->source_pdo_count = 0;
  c->alt_mode_count = 0;
  c->cable = UsbCable();
  c->contract = decode_contract(c->contract.rdo, ArrayView<const UsbPdo>());
  if (!c->connected) return Result::Ok;
  if (c->power_mode == UsbTypeCPowerMode::PowerDelivery && (features_ & kFeaturePdoDetails)) {
    const Result r = read_pdos(c);
    if (r != Result::Ok) return r;
  }
  if (c->power_mode == UsbTypeCPowerMode::PowerDelivery) c->contract = decode_contract(c->contract.rdo, ArrayView<const UsbPdo>(c->source_pdos, c->source_pdo_count));
  else c->contract = UsbPdContract();
  if (c->partner_alt_mode && (features_ & kFeatureAltModeDetails)) {
    const Result r = read_alt_modes(c);
    if (r != Result::Ok) return r;
  }
  if (features_ & kFeatureCableDetails) {
    uint8_t data[16] = {};
    uint32_t length = 0;
    const Result r = execute(control_word(kGetCableProperty, connector_field(c->number)), ByteView(data), &length);
    if (fatal(r)) return r;
    if (r == Result::Ok && !decode_cable_property(ConstByteView(data, min32(length, sizeof data)), &c->cable)) c->cable = UsbCable();
  }
  return Result::Ok;
}

UcsiCore::Result UcsiCore::read_status(UsbTypeCSnapshot* snapshot, uint32_t index, uint32_t ack_connector, bool* changed) {
  UsbTypeCConnector& c = snapshot->connectors[index];
  const UsbTypeCConnector before = c;
  uint8_t data[kMessageBytesV2] = {};
  uint32_t length = 0;
  Result r = execute(control_word(kGetConnectorStatus, connector_field(c.number)), ByteView(data), &length, ack_connector);
  uint16_t change = 0;
  UsbTypeCConnector next = c;
  if (r == Result::Ok && !decode_connector_status(ConstByteView(data, min32(length, sizeof data)), version_, &next, &change)) r = Result::Malformed;
  if (r != Result::Ok) {
    c.status_valid = false;
    *changed = !usb_type_c_connector_equal(before, c);
    return r;
  }
  next.status_valid = true;
  const bool partner_moved = !before.status_valid || before.connected != next.connected || before.partner != next.partner ||
                             before.power_role != next.power_role || before.power_mode != next.power_mode || before.contract.rdo != next.contract.rdo ||
                             before.partner_alt_mode != next.partner_alt_mode ||
                             (change & (kChangeConnect | kChangePartner | kChangeCapability | kChangePowerLevel | kChangePowerDirection | kChangeCam));
  c = next;
  if (partner_moved) r = read_details(&c);
  else c.contract = before.contract;
  *changed = !usb_type_c_connector_equal(before, c);
  snapshot->last_error = last_error_;
  return r;
}

UcsiCore::Result UcsiCore::refresh_connector(UsbTypeCSnapshot* snapshot, uint32_t index, bool* changed) {
  *changed = false;
  if (index >= snapshot->connector_count) return Result::Malformed;
  return read_status(snapshot, index, 0, changed);
}

UcsiCore::Result UcsiCore::service(UsbTypeCSnapshot* out, bool* changed) {
  *changed = false;
  uint32_t cci = 0;
  if (!transport_.read_cci(&cci)) return Result::Transport;
  note(cci);
  // Each connector that reported a change, once per call: the PPM shows one connector at a time in CCI and the next one after the acknowledgement,
  // so the loop goes on while new ones come up; one that keeps reporting itself waits for the next call (it cannot hold the caller here).
  uint32_t handled = 0;
  for (;;) {
    const uint32_t work = pending_ & ~handled;
    if (work == 0) break;
    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(work));
    handled |= 1u << bit;
    pending_ &= ~(1u << bit);
    Result r;
    bool one = false;
    if (bit < out->connector_count) {
      r = read_status(out, bit, bit + 1, &one);
    } else {
      acknowledging_ = bit + 1;
      r = acknowledge(false, bit + 1);   // a connector beyond the ones known: only acknowledged
      acknowledging_ = 0;
    }
    *changed = *changed || one;
    if (fatal(r)) {
      pending_ |= 1u << bit;             // (looked at again next time)
      return r;
    }
  }
  return Result::Ok;
}

bool usb_type_c_connector_equal(const UsbTypeCConnector& a, const UsbTypeCConnector& b) {
  if (a.number != b.number || a.status_valid != b.status_valid || a.connected != b.connected || a.partner != b.partner || a.data_role != b.data_role ||
      a.power_role != b.power_role || a.power_mode != b.power_mode || a.partner_usb != b.partner_usb || a.partner_alt_mode != b.partner_alt_mode ||
      a.partner_usb4_gen3 != b.partner_usb4_gen3 || a.partner_usb4_gen4 != b.partner_usb4_gen4 || a.battery_charging != b.battery_charging ||
      a.provider_limited != b.provider_limited || a.pd_revision != b.pd_revision || a.orientation_known != b.orientation_known ||
      a.flipped != b.flipped || a.sink_path != b.sink_path || a.power_reading_valid != b.power_reading_valid || a.vbus_mv != b.vbus_mv ||
      a.vbus_avg_ma != b.vbus_avg_ma || a.vbus_peak_ma != b.vbus_peak_ma)
    return false;
  if (a.contract.valid != b.contract.valid || a.contract.rdo != b.contract.rdo || a.contract.voltage_mv != b.contract.voltage_mv ||
      a.contract.current_ma != b.contract.current_ma)
    return false;
  if (a.source_pdo_count != b.source_pdo_count || a.alt_mode_count != b.alt_mode_count) return false;
  for (uint32_t i = 0; i < a.source_pdo_count && i < kUsbTypeCMaxPdos; ++i)
    if (a.source_pdos[i].raw != b.source_pdos[i].raw) return false;
  for (uint32_t i = 0; i < a.alt_mode_count && i < kUsbTypeCMaxAltModes; ++i)
    if (a.alt_modes[i].svid != b.alt_modes[i].svid || a.alt_modes[i].vdo != b.alt_modes[i].vdo) return false;
  const UsbTypeCConnectorCaps &x = a.caps, &y = b.caps;
  if (x.dfp != y.dfp || x.ufp != y.ufp || x.drp != y.drp || x.provider != y.provider || x.consumer != y.consumer || x.alt_modes != y.alt_modes) return false;
  return a.cable.valid == b.cable.valid && a.cable.speed_mbps == b.cable.speed_mbps && a.cable.current_ma == b.cable.current_ma &&
         a.cable.active == b.cable.active && a.cable.plug == b.cable.plug && a.cable.vbus_in_cable == b.cable.vbus_in_cable;
}

}  // namespace hardware
