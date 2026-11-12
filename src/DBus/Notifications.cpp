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

#include "DBus/Notifications.hpp"
#include "DBus/Wire.hpp"

namespace dbus {

namespace {

constexpr const char* kName = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";

constexpr const char* kIntrospection =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n"
    "  <interface name=\"org.freedesktop.Notifications\">\n"
    "    <method name=\"GetCapabilities\">\n      <arg direction=\"out\" type=\"as\" name=\"capabilities\"/>\n    </method>\n"
    "    <method name=\"Notify\">\n      <arg direction=\"in\" type=\"s\" name=\"app_name\"/>\n      <arg direction=\"in\" type=\"u\" name=\"replaces_id\"/>\n"
    "      <arg direction=\"in\" type=\"s\" name=\"app_icon\"/>\n      <arg direction=\"in\" type=\"s\" name=\"summary\"/>\n"
    "      <arg direction=\"in\" type=\"s\" name=\"body\"/>\n      <arg direction=\"in\" type=\"as\" name=\"actions\"/>\n"
    "      <arg direction=\"in\" type=\"a{sv}\" name=\"hints\"/>\n      <arg direction=\"in\" type=\"i\" name=\"expire_timeout\"/>\n"
    "      <arg direction=\"out\" type=\"u\" name=\"id\"/>\n    </method>\n"
    "    <method name=\"CloseNotification\">\n      <arg direction=\"in\" type=\"u\" name=\"id\"/>\n    </method>\n"
    "    <method name=\"GetServerInformation\">\n      <arg direction=\"out\" type=\"s\" name=\"name\"/>\n      <arg direction=\"out\" type=\"s\" name=\"vendor\"/>\n"
    "      <arg direction=\"out\" type=\"s\" name=\"version\"/>\n      <arg direction=\"out\" type=\"s\" name=\"spec_version\"/>\n    </method>\n"
    "    <signal name=\"NotificationClosed\">\n      <arg type=\"u\" name=\"id\"/>\n      <arg type=\"u\" name=\"reason\"/>\n    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n      <arg direction=\"out\" type=\"s\"/>\n    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n    <method name=\"Ping\">\n    </method>\n  </interface>\n"
    "</node>\n";

// At most `limit` bytes of the text, cut before a character that would not fit whole.
StringView cut(StringView text, uint64_t limit) {
  if (text.size() <= limit) return text;
  uint64_t n = limit;
  while (n > 0 && (uint8_t(text[n]) & 0xC0) == 0x80) --n;
  return text.first(n);
}

}  // namespace

NotificationService::NotificationService(Bus& bus, INotificationDisplay& display, allocators::IAllocator& allocator)
    : bus_(bus), display_(display), allocator_(allocator), showing_(allocator) {}

NotificationService::~NotificationService() {
  if (client_) bus_.disconnect_local(client_);
}

bool NotificationService::start() {
  if (client_) return true;
  client_ = bus_.connect_local(*this);
  if (!client_) return false;
  if (bus_.request_name(client_, kName, kNameDoNotQueue) != kRequestPrimaryOwner) {
    bus_.disconnect_local(client_);
    client_ = 0;
    return false;
  }
  return true;
}

void NotificationService::reply(Message& m) { bus_.send(client_, m); }

void NotificationService::closed(uint32_t id, uint32_t reason) {
  Message s(allocator_);
  if (!make_signal(kPath, kName, "NotificationClosed", &s) || !s.signature.assign("uu")) return;
  Writer w(s.body);
  w.u32(id);
  w.u32(reason);
  if (w.ok()) bus_.send(client_, s);
}

void NotificationService::notify(const Message& call) {
  Reader r(call.body.view(), call.big_endian);
  StringView app, icon, summary, body;
  uint32_t replaces = 0, timeout_bits = 0;
  uint64_t end = 0;
  r.string(&app);
  r.u32(&replaces);
  r.string(&icon);
  r.string(&summary);
  r.string(&body);
  if (r.begin_array(4, &end)) r.skip(end - r.position());      // actions (not offered)
  if (r.begin_array(8, &end)) r.skip(end - r.position());      // hints
  r.u32(&timeout_bits);
  if (r.failed()) return;
  const int32_t timeout = int32_t(timeout_bits);
  const uint32_t timeout_ms = timeout < 0 ? kDefaultTimeoutMs : uint32_t(timeout);
  uint32_t id = 0;
  for (uint64_t i = 0; replaces && i < showing_.count(); ++i)
    if (showing_[i].id == replaces) {
      display_.close(showing_[i].toast);
      id = replaces;
      showing_.remove_at(i);
      break;
    }
  Message m(allocator_);
  if (showing_.count() >= kMaxShowing) {
    if (call.expects_reply() && make_error(call, "org.freedesktop.DBus.Error.LimitsExceeded", "Too many notifications are showing", &m)) reply(m);
    return;
  }
  if (id == 0) {
    if (next_id_ == 0) next_id_ = 1;
    id = next_id_++;
  }
  Shown shown;
  shown.id = id;
  shown.toast = display_.show(cut(app, 256), cut(summary, kMaxTextBytes), cut(body, kMaxTextBytes), timeout_ms);
  shown.timeout_ms = timeout_ms;
  shown.expires_us = timeout_ms && now_us_ ? now_us_ + uint64_t(timeout_ms) * 1000 : 0;
  if (!showing_.push_back(shown)) {
    if (shown.toast) display_.close(shown.toast);
    if (call.expects_reply() && make_error(call, "org.freedesktop.DBus.Error.NoMemory", "Not enough memory", &m)) reply(m);
    return;
  }
  if (!call.expects_reply() || !make_reply(call, &m) || !m.signature.assign("u")) return;
  Writer w(m.body);
  w.u32(id);
  if (w.ok()) reply(m);
}

void NotificationService::close_notification(const Message& call) {
  Reader r(call.body.view(), call.big_endian);
  uint32_t id = 0;
  if (!r.u32(&id)) return;
  for (uint64_t i = 0; i < showing_.count(); ++i)
    if (showing_[i].id == id) {
      display_.close(showing_[i].toast);
      showing_.remove_at(i);
      closed(id, 3);
      break;
    }
  Message m(allocator_);
  if (call.expects_reply() && make_reply(call, &m)) reply(m);
}

void NotificationService::deliver(const Message& call) {
  if (call.type != kMethodCall) return;      // (NameAcquired and the like)
  const StringView iface = call.interface.view(), member = call.member.view();
  const bool any = iface.empty();
  const bool ours = call.path == kPath;
  Message m(allocator_);
  auto simple = [&](StringView signature, auto body) {
    if (!call.expects_reply() || !make_reply(call, &m) || !m.signature.assign(signature)) return;
    Writer w(m.body);
    body(w);
    if (w.ok()) reply(m);
  };
  if (ours && (any || iface == kName)) {
    if (member == "Notify" && call.signature == "susssasa{sv}i") return notify(call);
    if (member == "CloseNotification" && call.signature == "u") return close_notification(call);
    if (member == "GetCapabilities" && call.signature.empty())
      return simple("as", [](Writer& w) {
        const Writer::ArrayMark mark = w.begin_array(4);
        w.string("body");
        w.end_array(mark);
      });
    if (member == "GetServerInformation" && call.signature.empty())
      return simple("ssss", [](Writer& w) {
        w.string("TheoryCraftOS");
        w.string("TheoryCraftOS");
        w.string("1.0");
        w.string("1.2");
      });
  }
  if ((any || iface == "org.freedesktop.DBus.Introspectable") && member == "Introspect" && call.signature.empty())
    return simple("s", [&](Writer& w) {
      if (ours) {
        w.string(kIntrospection);
      } else {
        // the folders above the object
        const StringView p = call.path.view(), target = kPath;
        const char* child = p == "/" ? "org" : p == "/org" ? "freedesktop" : p == "/org/freedesktop" ? "Notifications" : nullptr;
        String xml(allocator_);
        xml.append("<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
                   "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n");
        if (child && target.starts_with(p)) {
          xml.append("  <node name=\"");
          xml.append(child);
          xml.append("\"/>\n");
        }
        xml.append("</node>\n");
        w.string(xml.view());
      }
    });
  if ((any || iface == "org.freedesktop.DBus.Peer") && member == "Ping" && call.signature.empty()) return simple("", [](Writer&) {});
  if (!call.expects_reply()) return;
  String text(allocator_);
  text.append("No such method '");
  text.append(member);
  text.append("' with signature '");
  text.append(call.signature.view());
  text.append("' on ");
  text.append(call.path.view());
  if (make_error(call, ours ? "org.freedesktop.DBus.Error.UnknownMethod" : "org.freedesktop.DBus.Error.UnknownObject", text.view(), &m)) reply(m);
}

void NotificationService::tick(uint64_t now_us) {
  now_us_ = now_us;
  for (uint64_t i = 0; i < showing_.count();) {
    if (showing_[i].timeout_ms && showing_[i].expires_us == 0) showing_[i].expires_us = now_us + uint64_t(showing_[i].timeout_ms) * 1000;
    if (showing_[i].expires_us && now_us >= showing_[i].expires_us) {
      const uint32_t id = showing_[i].id;
      showing_.remove_at(i);
      closed(id, 1);      // (the toast has gone by itself)
    } else {
      ++i;
    }
  }
}

}  // namespace dbus
