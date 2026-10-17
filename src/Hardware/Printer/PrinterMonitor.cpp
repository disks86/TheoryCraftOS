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

#include "Hardware/Printer/PrinterMonitor.hpp"

namespace hardware {

using namespace network;

const char* alert_code_name(uint32_t code) {
  switch (code) {
    case 1: return "other";
    case 3: return "cover open";
    case 4: return "cover closed";
    case 5: return "interlock open";
    case 6: return "interlock closed";
    case 7: return "configuration change";
    case 8: return "jam";
    case 9: return "subunit missing";
    case 10: return "life almost over";
    case 11: return "life over";
    case 12: return "almost empty";
    case 13: return "empty";
    case 14: return "almost full";
    case 15: return "full";
    case 16: return "near limit";
    case 17: return "at limit";
    case 18: return "subunit opened";
    case 19: return "subunit closed";
    case 22: return "off line";
    case 23: return "power saver";
    case 24: return "warming up";
    case 29: return "recoverable failure";
    case 30: return "unrecoverable failure";
    case 34: return "memory exhausted";
    case 35: return "under temperature";
    case 36: return "over temperature";
    case 501: return "door open";
    case 502: return "door closed";
    case 503: return "power up";
    case 504: return "power down";
    case 507: return "ready to print";
    default: return "alert";
  }
}

namespace {

Oid mib(const char* text) {
  Oid o;
  Oid::parse(text, &o);
  return o;
}

const Oid& sys_descr() { static const Oid o = mib("1.3.6.1.2.1.1.1.0"); return o; }
const Oid& hr_printer_status() { static const Oid o = mib("1.3.6.1.2.1.25.3.5.1.1.1"); return o; }
const Oid& hr_error_state() { static const Oid o = mib("1.3.6.1.2.1.25.3.5.1.2.1"); return o; }
const Oid& life_count() { static const Oid o = mib("1.3.6.1.2.1.43.10.2.1.4.1.1"); return o; }
const Oid& serial_number() { static const Oid o = mib("1.3.6.1.2.1.43.5.1.1.17.1"); return o; }
// prtMarkerSuppliesEntry columns: 4 class, 5 type, 6 description, 7 unit, 8 max capacity, 9 level
const Oid& supplies_column(uint32_t c) {
  static Oid cols[10];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 10; ++i) cols[i] = mib("1.3.6.1.2.1.43.11.1.1").child(i);
    ready = true;
  }
  return cols[c];
}
// prtAlertEntry columns: 2 severity, 7 code, 8 description
const Oid& alert_column(uint32_t c) {
  static Oid cols[10];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 10; ++i) cols[i] = mib("1.3.6.1.2.1.43.18.1.1").child(i);
    ready = true;
  }
  return cols[c];
}
const Oid& display_column() { static const Oid o = mib("1.3.6.1.2.1.43.16.5.1.2"); return o; }

// The row part of `oid` after its column.
Oid suffix_of(const Oid& oid, const Oid& column) {
  Oid s;
  for (uint32_t i = column.length; i < oid.length; ++i) s.arc[s.length++] = oid.arc[i];
  return s;
}
Oid with_suffix(const Oid& column, const Oid& suffix) {
  Oid o = column;
  for (uint32_t i = 0; i < suffix.length && o.length < Oid::kMaxArcs; ++i) o.arc[o.length++] = suffix.arc[i];
  return o;
}

void copy_text(char* dst, uint32_t capacity, const SnmpValue& v) {
  uint32_t n = 0;
  if (v.type == SnmpType::OctetString) {
    for (uint32_t i = 0; i < v.length && n + 1 < capacity; ++i) {
      const char c = static_cast<char>(v.bytes[i]);
      dst[n++] = (c >= 0x20 && c < 0x7f) ? c : ' ';
    }
  }
  while (n > 0 && dst[n - 1] == ' ') --n;  // trailing blanks (and padding) are not text
  dst[n] = 0;
}

SupplyKind kind_of(int64_t type) {
  switch (type) {
    case 3: case 21: return SupplyKind::Toner;
    case 4: return SupplyKind::WasteToner;
    case 5: case 6: case 7: return SupplyKind::Ink;
    case 9: return SupplyKind::Drum;
    case 10: return SupplyKind::Developer;
    case 11: case 17: case 22: return SupplyKind::Oil;
    case 15: case 19: return SupplyKind::Fuser;
    case 18: return SupplyKind::Cleaner;
    case 20: return SupplyKind::Transfer;
    default: return SupplyKind::Other;
  }
}

}  // namespace

bool PrinterMonitor::start() {
  if (busy()) return false;
  report_ = PrinterReport();
  client_.reset();
  issue(Step::Basics);
  return true;
}

void PrinterMonitor::issue(Step step) {
  step_ = step;
  Oid q[6];
  switch (step) {
    case Step::Basics:
      q[0] = sys_descr();
      q[1] = hr_printer_status();
      q[2] = hr_error_state();
      q[3] = life_count();
      q[4] = serial_number();
      client_.get(q, 5);
      break;
    case Step::SupplyWalk:
      client_.get_next(&cursor_, 1);
      break;
    case Step::SupplyDetail:
      q[0] = with_suffix(supplies_column(9), suffix_);
      q[1] = with_suffix(supplies_column(8), suffix_);
      q[2] = with_suffix(supplies_column(5), suffix_);
      q[3] = with_suffix(supplies_column(4), suffix_);
      client_.get(q, 4);
      break;
    case Step::AlertWalk:
      client_.get_next(&cursor_, 1);
      break;
    case Step::AlertDetail:
      q[0] = with_suffix(alert_column(7), suffix_);
      q[1] = with_suffix(alert_column(8), suffix_);
      client_.get(q, 2);
      break;
    case Step::Display:
      q[0] = display_column();
      client_.get_next(q, 1);
      break;
    default:
      break;
  }
}

void PrinterMonitor::poll() {
  if (!busy()) return;
  client_.poll();
  if (client_.state() == SnmpState::Pending) return;
  handle();
}

void PrinterMonitor::finish_supply() {
  SupplyInfo& s = pending_supply_;
  s.kind = kind_of(type_);
  s.consumed = klass_ != 4;
  if (level_ < 0) s.level_percent = static_cast<int32_t>(level_);
  else if (capacity_ > 0) s.level_percent = static_cast<int32_t>(level_ >= capacity_ ? 100 : level_ * 100 / capacity_);
  else s.level_percent = static_cast<int32_t>(level_ > 100 ? 100 : level_);  // capacity unknown: the printer reports percent
  if (report_.supply_count < PrinterReport::kMaxSupplies) report_.supplies[report_.supply_count++] = s;
}

void PrinterMonitor::handle() {
  const SnmpState state = client_.state();
  const SnmpMessage& r = client_.response();
  const bool agent_error = state == SnmpState::Failed && client_.error() == SnmpError::Agent;
  if (state == SnmpState::Failed && !agent_error) {  // a timeout: the printer is not there
    step_ = Step::Failed;
    return;
  }
  switch (step_) {
    case Step::Basics: {
      if (!agent_error) {
        for (uint32_t i = 0; i < r.count; ++i) {
          const SnmpVarBind& b = r.binds[i];
          if (b.value.is_exception()) continue;
          if (b.oid == sys_descr()) copy_text(report_.description, sizeof report_.description, b.value);
          else if (b.oid == hr_printer_status() && b.value.is_number())
            report_.engine = b.value.integer == 3 ? PrinterEngineState::Idle : b.value.integer == 4 ? PrinterEngineState::Printing
                             : b.value.integer == 5 ? PrinterEngineState::Warmup : b.value.integer == 1 ? PrinterEngineState::Other : PrinterEngineState::Unknown;
          else if (b.oid == hr_error_state() && b.value.type == SnmpType::OctetString) {
            for (uint32_t bit = 0; bit < 15 && bit / 8 < b.value.length; ++bit)
              if (b.value.bytes[bit / 8] & (0x80 >> (bit % 8))) report_.errors = static_cast<uint16_t>(report_.errors | (1u << bit));
          } else if (b.oid == life_count() && b.value.is_number()) {
            report_.has_page_count = true;
            report_.page_count = static_cast<uint32_t>(b.value.integer);
          } else if (b.oid == serial_number()) copy_text(report_.serial, sizeof report_.serial, b.value);
        }
      }
      cursor_ = supplies_column(6);
      issue(Step::SupplyWalk);
      return;
    }
    case Step::SupplyWalk: {
      if (agent_error || r.count == 0 || r.binds[0].value.is_exception() || !r.binds[0].oid.starts_with(supplies_column(6)) ||
          report_.supply_count >= PrinterReport::kMaxSupplies) {
        cursor_ = alert_column(2);
        issue(Step::AlertWalk);
        return;
      }
      cursor_ = r.binds[0].oid;
      suffix_ = suffix_of(cursor_, supplies_column(6));
      pending_supply_ = SupplyInfo();
      copy_text(pending_supply_.description, sizeof pending_supply_.description, r.binds[0].value);
      level_ = capacity_ = -2;
      type_ = 2;
      klass_ = 3;
      issue(Step::SupplyDetail);
      return;
    }
    case Step::SupplyDetail: {
      if (!agent_error) {
        for (uint32_t i = 0; i < r.count; ++i) {
          const SnmpVarBind& b = r.binds[i];
          if (!b.value.is_number()) continue;
          if (b.oid == with_suffix(supplies_column(9), suffix_)) level_ = b.value.integer;
          else if (b.oid == with_suffix(supplies_column(8), suffix_)) capacity_ = b.value.integer;
          else if (b.oid == with_suffix(supplies_column(5), suffix_)) type_ = b.value.integer;
          else if (b.oid == with_suffix(supplies_column(4), suffix_)) klass_ = b.value.integer;
        }
      }
      finish_supply();
      issue(Step::SupplyWalk);   // (cursor_ is the description just read)
      return;
    }
    case Step::AlertWalk: {
      if (agent_error || r.count == 0 || r.binds[0].value.is_exception() || !r.binds[0].oid.starts_with(alert_column(2)) ||
          report_.alert_count >= PrinterReport::kMaxAlerts) {
        issue(Step::Display);
        return;
      }
      cursor_ = r.binds[0].oid;
      suffix_ = suffix_of(cursor_, alert_column(2));
      pending_alert_ = AlertInfo();
      pending_alert_.severity = r.binds[0].value.is_number() && (r.binds[0].value.integer == 3 || r.binds[0].value.integer == 4 || r.binds[0].value.integer == 5)
                                    ? static_cast<AlertSeverity>(r.binds[0].value.integer) : AlertSeverity::Other;
      issue(Step::AlertDetail);
      return;
    }
    case Step::AlertDetail: {
      if (!agent_error) {
        for (uint32_t i = 0; i < r.count; ++i) {
          const SnmpVarBind& b = r.binds[i];
          if (b.oid == with_suffix(alert_column(7), suffix_) && b.value.is_number()) pending_alert_.code = static_cast<uint32_t>(b.value.integer);
          else if (b.oid == with_suffix(alert_column(8), suffix_)) copy_text(pending_alert_.description, sizeof pending_alert_.description, b.value);
        }
      }
      if (report_.alert_count < PrinterReport::kMaxAlerts) report_.alerts[report_.alert_count++] = pending_alert_;
      issue(Step::AlertWalk);
      return;
    }
    case Step::Display: {
      if (!agent_error && r.count > 0 && !r.binds[0].value.is_exception() && r.binds[0].oid.starts_with(display_column()))
        copy_text(report_.display, sizeof report_.display, r.binds[0].value);
      report_.valid = true;
      step_ = Step::Done;
      return;
    }
    default:
      return;
  }
}

}  // namespace hardware
