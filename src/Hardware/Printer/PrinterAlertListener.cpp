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

#include "Hardware/Printer/PrinterAlertListener.hpp"

namespace hardware {

using namespace network;

namespace {
Oid make(const char* text) {
  Oid o;
  Oid::parse(text, &o);
  return o;
}
}  // namespace

bool PrinterAlertListener::next(PrinterAlertEvent* out) {
  static const Oid prt_alert = make("1.3.6.1.2.1.43.18.2.0.1"), traps = make("1.3.6.1.6.3.1.1.5"), severity = make("1.3.6.1.2.1.43.18.1.1.2"),
                   group = make("1.3.6.1.2.1.43.18.1.1.4"), group_index = make("1.3.6.1.2.1.43.18.1.1.5"), location = make("1.3.6.1.2.1.43.18.1.1.6"),
                   code = make("1.3.6.1.2.1.43.18.1.1.7");
  SnmpNotification n;
  if (!receiver_.next(&n)) return false;
  *out = PrinterAlertEvent();
  out->source = n.source;
  out->uptime = n.uptime;
  out->trap_oid = n.trap_oid;
  out->inform = n.inform;
  if (n.trap_oid == prt_alert) {
    out->kind = PrinterAlertEvent::Kind::Alert;
    if (const SnmpVarBind* b = n.find_prefix(severity)) {
      const int64_t v = b->value.integer;
      out->severity = (v == 3 || v == 4 || v == 5) ? static_cast<AlertSeverity>(v) : AlertSeverity::Other;
    }
    if (const SnmpVarBind* b = n.find_prefix(group)) out->group = static_cast<uint32_t>(b->value.integer);
    if (const SnmpVarBind* b = n.find_prefix(group_index)) out->group_index = static_cast<uint32_t>(b->value.integer);
    if (const SnmpVarBind* b = n.find_prefix(code)) out->code = static_cast<uint32_t>(b->value.integer);
    if (const SnmpVarBind* b = n.find_prefix(location); b && b->value.type == SnmpType::OctetString) {
      uint32_t k = 0;
      for (; k < b->value.length && k + 1 < sizeof out->location; ++k) {
        const char c = static_cast<char>(b->value.bytes[k]);
        out->location[k] = (c >= 0x20 && c < 0x7f) ? c : ' ';
      }
      out->location[k] = 0;
    }
  } else if (n.trap_oid.length == traps.length + 1 && n.trap_oid.starts_with(traps)) {
    switch (n.trap_oid.arc[traps.length]) {
      case 1: out->kind = PrinterAlertEvent::Kind::ColdStart; break;
      case 2: out->kind = PrinterAlertEvent::Kind::WarmStart; break;
      case 3: out->kind = PrinterAlertEvent::Kind::LinkDown; break;
      case 4: out->kind = PrinterAlertEvent::Kind::LinkUp; break;
      case 5: out->kind = PrinterAlertEvent::Kind::AuthenticationFailure; break;
      default: break;
    }
  }
  return true;
}

}  // namespace hardware
