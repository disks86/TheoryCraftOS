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

// The bus driver: the methods of org.freedesktop.DBus (and the Introspectable, Peer, Properties and Monitoring interfaces the bus object has), answered
// as dbus-daemon answers them: the same replies, error names and, where clients look at them, error texts.
#include "DBus/BusInternal.hpp"

namespace dbus {

namespace {

constexpr const char* kIntrospectable = "org.freedesktop.DBus.Introspectable";
constexpr const char* kPeer = "org.freedesktop.DBus.Peer";
constexpr const char* kProperties = "org.freedesktop.DBus.Properties";
constexpr const char* kMonitoring = "org.freedesktop.DBus.Monitoring";

constexpr const char* kStandardInterfaces =
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n      <arg direction=\"out\" type=\"s\"/>\n    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"GetMachineId\">\n      <arg direction=\"out\" type=\"s\"/>\n    </method>\n"
    "    <method name=\"Ping\">\n    </method>\n"
    "  </interface>\n";

constexpr const char* kBusInterfaces =
    "  <interface name=\"org.freedesktop.DBus\">\n"
    "    <method name=\"Hello\">\n      <arg direction=\"out\" type=\"s\"/>\n    </method>\n"
    "    <method name=\"RequestName\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"in\" type=\"u\"/>\n      <arg direction=\"out\" type=\"u\"/>\n    </method>\n"
    "    <method name=\"ReleaseName\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"u\"/>\n    </method>\n"
    "    <method name=\"StartServiceByName\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"in\" type=\"u\"/>\n      <arg direction=\"out\" type=\"u\"/>\n    </method>\n"
    "    <method name=\"UpdateActivationEnvironment\">\n      <arg direction=\"in\" type=\"a{ss}\"/>\n    </method>\n"
    "    <method name=\"NameHasOwner\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"b\"/>\n    </method>\n"
    "    <method name=\"ListNames\">\n      <arg direction=\"out\" type=\"as\"/>\n    </method>\n"
    "    <method name=\"ListActivatableNames\">\n      <arg direction=\"out\" type=\"as\"/>\n    </method>\n"
    "    <method name=\"AddMatch\">\n      <arg direction=\"in\" type=\"s\"/>\n    </method>\n"
    "    <method name=\"RemoveMatch\">\n      <arg direction=\"in\" type=\"s\"/>\n    </method>\n"
    "    <method name=\"GetNameOwner\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"s\"/>\n    </method>\n"
    "    <method name=\"ListQueuedOwners\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"as\"/>\n    </method>\n"
    "    <method name=\"GetConnectionUnixUser\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"u\"/>\n    </method>\n"
    "    <method name=\"GetConnectionUnixProcessID\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"u\"/>\n    </method>\n"
    "    <method name=\"GetAdtAuditSessionData\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"ay\"/>\n    </method>\n"
    "    <method name=\"GetConnectionSELinuxSecurityContext\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"ay\"/>\n    </method>\n"
    "    <method name=\"ReloadConfig\">\n    </method>\n"
    "    <method name=\"GetId\">\n      <arg direction=\"out\" type=\"s\"/>\n    </method>\n"
    "    <method name=\"GetConnectionCredentials\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"a{sv}\"/>\n    </method>\n"
    "    <property name=\"Features\" type=\"as\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"const\"/>\n    </property>\n"
    "    <property name=\"Interfaces\" type=\"as\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"const\"/>\n    </property>\n"
    "    <signal name=\"NameOwnerChanged\">\n      <arg type=\"s\"/>\n      <arg type=\"s\"/>\n      <arg type=\"s\"/>\n    </signal>\n"
    "    <signal name=\"NameLost\">\n      <arg type=\"s\"/>\n    </signal>\n"
    "    <signal name=\"NameAcquired\">\n      <arg type=\"s\"/>\n    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"v\"/>\n    </method>\n"
    "    <method name=\"GetAll\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"out\" type=\"a{sv}\"/>\n    </method>\n"
    "    <method name=\"Set\">\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"in\" type=\"s\"/>\n      <arg direction=\"in\" type=\"v\"/>\n    </method>\n"
    "    <signal name=\"PropertiesChanged\">\n      <arg type=\"s\" name=\"interface_name\"/>\n      <arg type=\"a{sv}\" name=\"changed_properties\"/>\n"
    "      <arg type=\"as\" name=\"invalidated_properties\"/>\n    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Monitoring\">\n"
    "    <method name=\"BecomeMonitor\">\n      <arg direction=\"in\" type=\"as\"/>\n      <arg direction=\"in\" type=\"u\"/>\n    </method>\n"
    "  </interface>\n";

// The call's arguments, read in the order of its signature (which the caller has checked).
struct Args {
  explicit Args(const Message& call) : r(call.body.view(), call.big_endian) {}
  StringView s() {
    StringView v;
    r.string(&v);
    return v;
  }
  uint32_t u() {
    uint32_t v = 0;
    r.u32(&v);
    return v;
  }
  Reader r;
};

}  // namespace

struct BusDriverCall {
  Bus& bus;
  Bus::Connection& from;
  const Message& call;
  allocators::IAllocator& a;

  void error(StringView name, StringView text) { bus.error_to(from, call, name, text); }

  void error2(StringView name, StringView before, StringView middle, StringView after) {
    String text(a);
    text.append(before);
    text.append(middle);
    text.append(after);
    error(name, text.view());
  }

  // Sends the reply: `body` writes its values (of `signature`).
  template <class Body>
  void reply(StringView signature, Body body) {
    if (!call.expects_reply()) return;
    Message r(a);
    if (!bus.from_bus(&r, kMethodReturn) || !r.destination.assign(from.unique.view()) || !r.signature.assign(signature)) return;
    r.has_reply_serial = true;
    r.reply_serial = call.serial;
    Writer w(r.body);
    body(w);
    if (!w.ok() || !bus.encode_wire(r)) return;
    bus.deliver(from, r, bus.wire_);
    bus.broadcast(r, &from, false);
  }
  void empty_reply() {
    reply("", [](Writer&) {});
  }

  bool signature_is(StringView expected) {
    if (call.signature == expected) return true;
    String text(a);
    text.append("Call to ");
    text.append(call.member.view());
    text.append(" has wrong args (");
    text.append(call.signature.view());
    text.append(", expected ");
    text.append(expected);
    text.append(")");
    error("org.freedesktop.DBus.Error.InvalidArgs", text.view());
    return false;
  }

  // The connection a name names (the bus's own name: none, *is_bus set). Null, with the error sent, when nobody has it.
  Bus::Connection* named(StringView name, const char* what, bool* is_bus) {
    *is_bus = name == kBusName;
    if (*is_bus) return nullptr;
    Bus::Connection* c = valid_bus_name(name) ? bus.find(bus.owner_of(name)) : nullptr;
    if (!c) {
      String text(a);
      text.append("Could not get ");
      text.append(what);
      text.append(" of name '");
      text.append(name);
      text.append("': no such name");
      error("org.freedesktop.DBus.Error.NameHasNoOwner", text.view());
    }
    return c;
  }

  bool name_for_ownership(StringView name, const char* verb) {
    if (!valid_bus_name(name)) {
      error2("org.freedesktop.DBus.Error.InvalidArgs", "Requested bus name \"", name, "\" is not valid");
      return false;
    }
    if (is_unique_name(name)) {
      String text(a);
      text.append("Cannot ");
      text.append(verb);
      text.append(" a service starting with ':' such as \"");
      text.append(name);
      text.append("\"");
      error("org.freedesktop.DBus.Error.InvalidArgs", text.view());
      return false;
    }
    if (name == kBusName) {
      String text(a);
      text.append("Connection \"");
      text.append(from.unique.view());
      text.append("\" is not allowed to own the service \"org.freedesktop.DBus\" because it is reserved for D-Bus' use only");
      error("org.freedesktop.DBus.Error.InvalidArgs", text.view());
      return false;
    }
    return true;
  }

  void list_names(bool activatable) {
    reply("as", [&](Writer& w) {
      const Writer::ArrayMark m = w.begin_array(4);
      w.string(kBusName);
      if (!activatable) {
        for (const Bus::Connection& c : bus.connections_)
          if (c.hello && !c.monitor && !c.closing) w.string(c.unique.view());
        for (const Bus::NameEntry& e : bus.names_)
          if (e.queue.count()) w.string(e.name.view());
      }
      w.end_array(m);
    });
  }

  void add_match(StringView text, bool remove) {
    UniquePointer<MatchRule> rule = UniquePointer<MatchRule>::make(a, a);
    if (!rule) return error("org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
    if (!rule->parse(text)) return error("org.freedesktop.DBus.Error.MatchRuleInvalid", "Could not parse match rule");
    if (remove) {
      for (uint64_t i = 0; i < from.matches.count(); ++i)
        if (from.matches[i]->same_as(*rule)) {
          if (from.matches[i]->eavesdrop && bus.eavesdrop_rules_) --bus.eavesdrop_rules_;
          from.matches.remove_at(i);
          return empty_reply();
        }
      return error("org.freedesktop.DBus.Error.MatchRuleNotFound", "The given match rule wasn't found and can't be removed");
    }
    if (from.matches.count() >= Bus::kMaxMatchesPerConnection)
      return error("org.freedesktop.DBus.Error.LimitsExceeded", "Connection has too many match rules");
    const bool eavesdrop = rule->eavesdrop;
    if (!from.matches.push_back(util::move(rule))) return error("org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
    if (eavesdrop) ++bus.eavesdrop_rules_;
    empty_reply();
  }

  void write_property(Writer& w, StringView property) {
    const Writer::ArrayMark m = w.begin_array(4);
    w.string(property == "Features" ? "HeaderFiltering" : kMonitoring);
    w.end_array(m);
  }

  void properties(StringView member) {
    if (member == "Get") {
      if (!signature_is("ss")) return;
      Args args(call);
      const StringView iface = args.s(), property = args.s();
      if (iface != kBusName) return error2("org.freedesktop.DBus.Error.UnknownInterface", "Interface \"", iface, "\" does not exist");
      if (property != "Features" && property != "Interfaces")
        return error2("org.freedesktop.DBus.Error.UnknownProperty", "Property \"", property, "\" does not exist");
      reply("v", [&](Writer& w) {
        w.variant("as");
        write_property(w, property);
      });
      return;
    }
    if (member == "GetAll") {
      if (!signature_is("s")) return;
      Args args(call);
      const StringView iface = args.s();
      const bool ours = iface == kBusName;
      if (!ours && !iface.empty() && iface != kIntrospectable && iface != kPeer && iface != kProperties && iface != kMonitoring)
        return error2("org.freedesktop.DBus.Error.UnknownInterface", "Interface \"", iface, "\" does not exist");
      reply("a{sv}", [&](Writer& w) {
        const Writer::ArrayMark m = w.begin_array(8);
        static const char* const kNames[] = {"Features", "Interfaces"};
        if (ours || iface.empty())
          for (const char* p : kNames) {
            w.begin_struct();
            w.string(p);
            w.variant("as");
            write_property(w, p);
          }
        w.end_array(m);
      });
      return;
    }
    if (member == "Set") {
      if (!signature_is("ssv")) return;
      Args args(call);
      const StringView iface = args.s(), property = args.s();
      if (iface != kBusName) return error2("org.freedesktop.DBus.Error.UnknownInterface", "Interface \"", iface, "\" does not exist");
      if (property == "Features" || property == "Interfaces")
        return error2("org.freedesktop.DBus.Error.PropertyReadOnly", "Property \"", property, "\" cannot be set");
      return error2("org.freedesktop.DBus.Error.UnknownProperty", "Property \"", property, "\" does not exist");
    }
    unknown();
  }

  void introspect() {
    if (!signature_is("")) return;
    const StringView path = call.path.view();
    String xml(a);
    xml.append("<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
               "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n");
    if (path == kBusPath) xml.append(kBusInterfaces);
    xml.append(kStandardInterfaces);
    const char* child = path == "/" ? "org" : path == "/org" ? "freedesktop" : path == "/org/freedesktop" ? "DBus" : nullptr;
    if (child) {
      xml.append("  <node name=\"");
      xml.append(child);
      xml.append("\"/>\n");
    }
    xml.append("</node>\n");
    reply("s", [&](Writer& w) { w.string(xml.view()); });
  }

  void become_monitor() {
    if (!signature_is("asu")) return;
    Reader r(call.body.view(), call.big_endian);
    uint64_t end = 0;
    List<UniquePointer<MatchRule>> rules(a);
    if (!r.begin_array(4, &end)) return;
    while (r.position() < end) {
      StringView text;
      if (!r.string(&text)) return;
      UniquePointer<MatchRule> rule = UniquePointer<MatchRule>::make(a, a);
      if (!rule) return error("org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
      if (!rule->parse(text)) return error("org.freedesktop.DBus.Error.MatchRuleInvalid", "Could not parse match rule");
      if (rules.count() >= Bus::kMaxMatchesPerConnection) return error("org.freedesktop.DBus.Error.LimitsExceeded", "Too many match rules");
      if (!rules.push_back(util::move(rule))) return error("org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
    }
    uint32_t flags = 0;
    r.u32(&flags);
    if (flags != 0) return error("org.freedesktop.DBus.Error.InvalidArgs", "BecomeMonitor does not support any flags yet");
    if (bus.monitors_ >= Bus::kMaxMonitors) return error("org.freedesktop.DBus.Error.LimitsExceeded", "Too many monitors");
    empty_reply();
    // it stops being an ordinary connection: its names go, then its unique name (it may not send any more)
    bus.release_all_names(from, true);
    bus.drop_pending(from);      // (a monitor answers nothing and is answered nothing)
    for (const UniquePointer<MatchRule>& rule : from.matches.view())
      if (rule->eavesdrop && bus.eavesdrop_rules_) --bus.eavesdrop_rules_;
    from.matches.clear();
    bus.signal_to(from, "NameLost", from.unique.view());
    from.monitor = true;
    ++bus.monitors_;
    bus.signal_owner_changed(from.unique.view(), from.unique.view(), StringView());
    for (UniquePointer<MatchRule>& rule : rules.view())
      if (!from.matches.push_back(util::move(rule))) {      // (fewer rules than asked would watch more: none means everything)
        bus.close(from);
        return;
      }
  }

  void unknown() {
    String text(a);
    text.append("org.freedesktop.DBus does not understand message ");
    text.append(call.member.view());
    error("org.freedesktop.DBus.Error.UnknownMethod", text.view());
  }

  void credentials(StringView member) {
    if (!signature_is("s")) return;
    Args args(call);
    const StringView name = args.s();
    bool is_bus = false;
    const bool wants_creds = member == "GetConnectionCredentials";
    Bus::Connection* c = named(name, wants_creds ? "credentials" : member == "GetConnectionUnixUser" ? "UID" : member == "GetConnectionUnixProcessID" ? "PID" : "audit session data", &is_bus);
    if (!c && !is_bus) return;
    const bool kernel = is_bus || (c && c->local);
    const uint32_t uid = kernel ? 0 : c->uid;
    const uint64_t pid = kernel ? 0 : c->pid;
    if (wants_creds) {
      reply("a{sv}", [&](Writer& w) {
        const Writer::ArrayMark m = w.begin_array(8);
        if (uid != ~0u) {
          w.begin_struct();
          w.string("UnixUserID");
          w.variant("u");
          w.u32(uid);
        }
        if (pid != 0 && pid <= 0xFFFFFFFFu) {
          w.begin_struct();
          w.string("ProcessID");
          w.variant("u");
          w.u32(uint32_t(pid));
        }
        w.end_array(m);
      });
    } else if (member == "GetConnectionUnixUser") {
      if (uid == ~0u) return error2("org.freedesktop.DBus.Error.Failed", "Could not determine UID for '", name, "'");
      reply("u", [&](Writer& w) { w.u32(uid); });
    } else if (member == "GetConnectionUnixProcessID") {
      if (pid == 0 || pid > 0xFFFFFFFFu) return error2("org.freedesktop.DBus.Error.UnixProcessIdUnknown", "Could not determine PID for '", name, "'");
      reply("u", [&](Writer& w) { w.u32(uint32_t(pid)); });
    } else if (member == "GetAdtAuditSessionData") {
      error2("org.freedesktop.DBus.Error.AdtAuditDataUnknown", "Could not determine audit session data for '", name, "'");
    } else {
      error2("org.freedesktop.DBus.Error.SELinuxSecurityContextUnknown", "Could not determine security context for '", name, "'");
    }
  }

  void bus_method(StringView member) {
    if (member == "Hello") return error("org.freedesktop.DBus.Error.Failed", "Already handled an Hello message");
    if (member == "RequestName") {
      if (!signature_is("su")) return;
      Args args(call);
      const StringView name = args.s();
      const uint32_t flags = args.u();
      if (!name_for_ownership(name, "acquire")) return;
      const uint32_t result = bus.request_name(from.id, name, flags & 7);
      if (result == 0) return error("org.freedesktop.DBus.Error.LimitsExceeded", "Connection owns too many names");
      return reply("u", [&](Writer& w) { w.u32(result); });
    }
    if (member == "ReleaseName") {
      if (!signature_is("s")) return;
      Args args(call);
      const StringView name = args.s();
      if (!name_for_ownership(name, "release")) return;
      const uint32_t result = bus.release_name(from.id, name);
      return reply("u", [&](Writer& w) { w.u32(result); });
    }
    if (member == "StartServiceByName") {
      if (!signature_is("su")) return;
      Args args(call);
      const StringView name = args.s();
      if (name == kBusName || (valid_bus_name(name) && bus.owner_of(name))) return reply("u", [](Writer& w) { w.u32(kStartAlreadyRunning); });
      return error2("org.freedesktop.DBus.Error.ServiceUnknown", "The name ", name, " was not provided by any .service files");
    }
    if (member == "UpdateActivationEnvironment") {
      if (!signature_is("a{ss}")) return;
      if (bus.kind() == BusKind::System) return error("org.freedesktop.DBus.Error.AccessDenied", "Cannot change activation environment on a system bus.");
      return empty_reply();      // (nothing is activated: there is no environment to change)
    }
    if (member == "NameHasOwner") {
      if (!signature_is("s")) return;
      Args args(call);
      const StringView name = args.s();
      const bool owned = name == kBusName || (valid_bus_name(name) && bus.owner_of(name) != 0);
      return reply("b", [&](Writer& w) { w.boolean(owned); });
    }
    if (member == "ListNames" || member == "ListActivatableNames") {
      if (!signature_is("")) return;
      return list_names(member == "ListActivatableNames");
    }
    if (member == "AddMatch" || member == "RemoveMatch") {
      if (!signature_is("s")) return;
      Args args(call);
      return add_match(args.s(), member == "RemoveMatch");
    }
    if (member == "GetNameOwner") {
      if (!signature_is("s")) return;
      Args args(call);
      const StringView name = args.s();
      bool is_bus = false;
      Bus::Connection* c = named(name, "owner", &is_bus);
      if (is_bus) return reply("s", [](Writer& w) { w.string(kBusName); });
      if (c) reply("s", [&](Writer& w) { w.string(c->unique.view()); });
      return;
    }
    if (member == "ListQueuedOwners") {
      if (!signature_is("s")) return;
      Args args(call);
      const StringView name = args.s();
      bool is_bus = false;
      Bus::Connection* c = named(name, "owners", &is_bus);
      if (is_bus) return reply("as", [](Writer& w) { const Writer::ArrayMark m = w.begin_array(4); w.string(kBusName); w.end_array(m); });
      if (!c) return;
      const Bus::NameEntry* e = is_unique_name(name) ? nullptr : bus.name_entry(name);
      return reply("as", [&](Writer& w) {
        const Writer::ArrayMark m = w.begin_array(4);
        if (!e) {
          w.string(c->unique.view());
        } else {
          for (const Bus::NameEntry::Owner& o : e->queue.view())
            if (const Bus::Connection* q = bus.find(o.client)) w.string(q->unique.view());
        }
        w.end_array(m);
      });
    }
    if (member == "GetConnectionUnixUser" || member == "GetConnectionUnixProcessID" || member == "GetConnectionCredentials" ||
        member == "GetAdtAuditSessionData" || member == "GetConnectionSELinuxSecurityContext")
      return credentials(member);
    if (member == "ReloadConfig") {
      if (!signature_is("")) return;
      return empty_reply();
    }
    if (member == "GetId") {
      if (!signature_is("")) return;
      return reply("s", [&](Writer& w) { w.string(bus.guid()); });
    }
    unknown();
  }
};

void BusDriver::hello(Bus& bus, Bus::Connection& from, const Message& call) {
  bus.make_unique_name(from);
  if (from.closing) return;
  from.hello = true;
  bus.signal_owner_changed(from.unique.view(), StringView(), from.unique.view());
  BusDriverCall d{bus, from, call, bus.allocator_};
  d.reply("s", [&](Writer& w) { w.string(from.unique.view()); });
  bus.signal_to(from, "NameAcquired", from.unique.view());
}

void BusDriver::handle(Bus& bus, Bus::Connection& from, const Message& call) {
  BusDriverCall d{bus, from, call, bus.allocator_};
  const StringView iface = call.interface.view(), member = call.member.view();
  const bool any = iface.empty();
  if (any || iface == kBusName) {
    static const char* const kMethods[] = {"Hello", "RequestName", "ReleaseName", "StartServiceByName", "UpdateActivationEnvironment", "NameHasOwner",
                                           "ListNames", "ListActivatableNames", "AddMatch", "RemoveMatch", "GetNameOwner", "ListQueuedOwners",
                                           "GetConnectionUnixUser", "GetConnectionUnixProcessID", "GetAdtAuditSessionData",
                                           "GetConnectionSELinuxSecurityContext", "ReloadConfig", "GetId", "GetConnectionCredentials"};
    bool known = false;
    for (const char* m : kMethods) known |= member == m;
    if (known || !any) return d.bus_method(member);
  }
  if ((any || iface == kIntrospectable) && member == "Introspect") return d.introspect();
  if (any || iface == kPeer) {
    if (member == "Ping") return d.signature_is("") ? d.empty_reply() : void();
    if (member == "GetMachineId") {
      if (d.signature_is("")) d.reply("s", [&](Writer& w) { w.string(StringView(bus.machine_id_)); });
      return;
    }
  }
  if ((any || iface == kProperties) && (member == "Get" || member == "GetAll" || member == "Set")) return d.properties(member);
  if ((any || iface == kMonitoring) && member == "BecomeMonitor") return d.become_monitor();
  if (!any && iface != kIntrospectable && iface != kPeer && iface != kProperties && iface != kMonitoring)
    return d.error2("org.freedesktop.DBus.Error.UnknownInterface", "org.freedesktop.DBus does not understand interface ", iface, "");
  d.unknown();
}

}  // namespace dbus
