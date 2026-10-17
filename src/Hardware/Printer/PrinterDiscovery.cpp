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

#include "Hardware/Printer/PrinterDiscovery.hpp"
#include "Network/Mdns/MdnsMessage.hpp"
#include "Network/Snmp/SnmpMessage.hpp"

namespace hardware {

using namespace network;

namespace {
constexpr uint32_t kMaxServices = 24, kMaxHosts = 24, kMaxHits = 16;
constexpr uint64_t kRoundsMs[] = {0, 1000, 3000, 7000};
constexpr uint32_t kRounds = sizeof kRoundsMs / sizeof kRoundsMs[0];
constexpr uint64_t kFinishedAfterMs = 10000, kAskIntervalMs = 500;
constexpr uint32_t kMaxAsks = 4;
constexpr int32_t kProbeId = 0x5c0de;

enum class Kind : uint8_t { None, Ipp, Ipps, Lpd, Raw };
struct ServiceType {
  const char* name;
  Kind kind;
};
constexpr ServiceType kTypes[] = {{"_ipp._tcp.local", Kind::Ipp}, {"_ipps._tcp.local", Kind::Ipps}, {"_printer._tcp.local", Kind::Lpd}, {"_pdl-datastream._tcp.local", Kind::Raw}};

uint32_t length_of(const char* s) {
  uint32_t n = 0;
  while (s && s[n]) ++n;
  return n;
}
void copy_text(char* dst, uint32_t capacity, const char* src) {
  uint32_t n = 0;
  for (; src && src[n] && n + 1 < capacity; ++n) dst[n] = src[n];
  dst[n] = 0;
}
bool contains_word(const char* haystack, const char* needle) {  // case-insensitive substring
  const uint32_t hn = length_of(haystack), nn = length_of(needle);
  for (uint32_t i = 0; i + nn <= hn; ++i) {
    uint32_t k = 0;
    for (; k < nn; ++k) {
      char a = haystack[i + k], b = needle[k];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
      if (a != b) break;
    }
    if (k == nn) return true;
  }
  return false;
}
// Which service type `name` is the instance of ("X._ipp._tcp.local"); *label_length is the length of the instance label.
Kind instance_kind(const char* name, uint32_t* label_length) {
  const uint32_t n = length_of(name);
  for (const ServiceType& t : kTypes) {
    const uint32_t tn = length_of(t.name);
    if (n > tn + 1 && name[n - tn - 1] == '.' && mdns::names_equal(name + n - tn, t.name)) {
      *label_length = n - tn - 1;
      return t.kind;
    }
  }
  return Kind::None;
}
Kind type_kind(const char* name) {
  for (const ServiceType& t : kTypes)
    if (mdns::names_equal(name, t.name)) return t.kind;
  return Kind::None;
}
}  // namespace

bool DiscoveredPrinter::supports_pcl() const { return contains_word(pdl, "pcl"); }
bool DiscoveredPrinter::supports_postscript() const { return contains_word(pdl, "postscript"); }

struct PrinterDiscovery::Tables {
  struct Service {
    bool used = false;
    Kind kind = Kind::None;
    char instance[mdns::kMaxName] = {};
    char host[mdns::kMaxName] = {};
    uint16_t port = 0;
    bool have_srv = false, have_txt = false;
    char rp[48] = {}, ty[64] = {}, mfg[32] = {}, pdl[96] = {};
    uint8_t srv_asks = 0;
  };
  struct Host {
    bool used = false;
    char name[mdns::kMaxName] = {};
    Ipv4Address address;
    uint8_t asks = 0;
  };
  struct Hit {  // an SNMP answer from a printer
    bool used = false;
    Ipv4Address address;
    char name[64] = {};
    char description[80] = {};
  };
  Service services[kMaxServices];
  Host hosts[kMaxHosts];
  Hit hits[kMaxHits];
  DiscoveredPrinter printers[PrinterDiscovery::kMaxPrinters];
  uint32_t printer_count = 0;
  mdns::Message message;
  uint32_t next_host = 0;

  Service* find_service(const char* instance) {
    for (Service& s : services)
      if (s.used && mdns::names_equal(s.instance, instance)) return &s;
    return nullptr;
  }
  Service* service_for(const char* instance, Kind kind) {
    if (Service* s = find_service(instance)) return s;
    for (Service& s : services)
      if (!s.used) {
        s = Service();
        s.used = true;
        s.kind = kind;
        copy_text(s.instance, sizeof s.instance, instance);
        return &s;
      }
    return nullptr;
  }
  Host* find_host(const char* name) {
    for (Host& h : hosts)
      if (h.used && mdns::names_equal(h.name, name)) return &h;
    return nullptr;
  }
  Host* host_for(const char* name) {
    if (Host* h = find_host(name)) return h;
    for (Host& h : hosts)
      if (!h.used) {
        h = Host();
        h.used = true;
        copy_text(h.name, sizeof h.name, name);
        return &h;
      }
    Host& h = hosts[next_host++ % kMaxHosts];  // full: the oldest slot goes
    h = Host();
    h.used = true;
    copy_text(h.name, sizeof h.name, name);
    return &h;
  }
  DiscoveredPrinter* printer_for(Ipv4Address address) {
    for (uint32_t i = 0; i < printer_count; ++i)
      if (printers[i].address == address) return &printers[i];
    if (printer_count >= PrinterDiscovery::kMaxPrinters) return nullptr;
    DiscoveredPrinter& p = printers[printer_count++];
    p = DiscoveredPrinter();
    p.address = address;
    return &p;
  }
};

bool PrinterDiscovery::start(uint8_t methods) {
  stop();
  tables_ = UniquePointer<Tables>::make(allocator_);
  if (!tables_) return false;
  methods_ = methods;
  if (methods & kMdns) {
    mdns_ = stack_.udp_open(mdns::kPort);
    if (!mdns_) mdns_ = stack_.udp_open(0);   // 5353 is taken: answers then come straight to us (legacy unicast)
    joined_ = stack_.join_multicast(mdns::kGroup);
    if (!mdns_) {
      stop();
      return false;
    }
  }
  if (methods & kSnmp) {
    snmp_ = stack_.udp_open(0);
    if (!snmp_) {
      stop();
      return false;
    }
  }
  running_ = true;
  finished_ = false;
  round_ = 0;
  started_ms_ = stack_.now_ms();
  last_ask_ms_ = started_ms_;
  return true;
}

void PrinterDiscovery::stop() {
  if (joined_) stack_.leave_multicast(mdns::kGroup);
  joined_ = false;
  if (mdns_) mdns_->close();
  if (snmp_) snmp_->close();
  mdns_ = SharedPointer<IUdpSocket>();
  snmp_ = SharedPointer<IUdpSocket>();
  running_ = false;
}

void PrinterDiscovery::set_snmp_community(const char* community) { copy_text(community_, sizeof community_, community); }

uint32_t PrinterDiscovery::count() const { return tables_ ? tables_->printer_count : 0; }
const DiscoveredPrinter& PrinterDiscovery::at(uint32_t index) const { return tables_->printers[index]; }
const DiscoveredPrinter* PrinterDiscovery::find(Ipv4Address address) const {
  if (tables_)
    for (uint32_t i = 0; i < tables_->printer_count; ++i)
      if (tables_->printers[i].address == address) return &tables_->printers[i];
  return nullptr;
}

void PrinterDiscovery::send_mdns_queries(bool browse) {
  if (!mdns_) return;
  mdns::Question q[mdns::Message::kMaxQuestions];
  uint32_t n = 0;
  if (browse) {
    for (const ServiceType& t : kTypes) {
      copy_text(q[n].name, sizeof q[n].name, t.name);
      q[n++].type = mdns::kTypePtr;
    }
  } else {
    // What is missing: the SRV and TXT of instances that answered but did not say more, the address of hosts we have names for.
    for (Tables::Service& s : tables_->services) {
      if (!s.used || s.have_srv || s.srv_asks >= kMaxAsks || n + 2 > mdns::Message::kMaxQuestions) continue;
      ++s.srv_asks;
      copy_text(q[n].name, sizeof q[n].name, s.instance);
      q[n++].type = mdns::kTypeSrv;
      copy_text(q[n].name, sizeof q[n].name, s.instance);
      q[n++].type = mdns::kTypeTxt;
    }
    for (Tables::Service& s : tables_->services) {
      if (!s.used || !s.have_srv || n >= mdns::Message::kMaxQuestions) continue;
      Tables::Host* h = tables_->host_for(s.host);
      if (!h || h->address.value() != 0 || h->asks >= kMaxAsks) continue;
      ++h->asks;
      copy_text(q[n].name, sizeof q[n].name, s.host);
      q[n++].type = mdns::kTypeA;
    }
  }
  if (n == 0) return;
  ByteBuffer out(allocator_);
  if (mdns::build_query(q, n, out) && out.size() <= 1400) mdns_->send_to(mdns::kGroup, mdns::kPort, out.view());
}

bool PrinterDiscovery::probe_host(Ipv4Address address) {
  if (!snmp_) return false;
  SnmpMessage m;
  m.version = 1;
  copy_text(m.community, sizeof m.community, community_);
  m.pdu = SnmpPdu::Get;
  m.request_id = kProbeId;
  static const char* const kOids[] = {"1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.25.3.2.1.2.1", "1.3.6.1.2.1.43.5.1.1.16.1"};
  for (const char* text : kOids) {
    if (!Oid::parse(text, &m.binds[m.count].oid)) return false;
    ++m.count;
  }
  ByteBuffer out(allocator_);
  return encode_snmp(m, allocator_, &out) && snmp_->send_to(address, 161, out.view());
}

void PrinterDiscovery::send_snmp_probe() {
  const IpConfiguration config = stack_.configuration();
  if (config.configured()) probe_host(Ipv4Address::subnet_broadcast(config.address, config.netmask));
  probe_host(kBroadcastAddress);
}

void PrinterDiscovery::poll() {
  if (!running_ || !tables_) return;
  const uint64_t now = stack_.now_ms();
  const uint64_t elapsed = now - started_ms_;
  while (round_ < kRounds && elapsed >= kRoundsMs[round_]) {
    if (methods_ & kMdns) send_mdns_queries(true);
    if ((methods_ & kSnmp) && (round_ == 0 || round_ == 2)) send_snmp_probe();
    ++round_;
  }
  if (elapsed >= kFinishedAfterMs) finished_ = true;
  if ((methods_ & kMdns) && now - last_ask_ms_ >= kAskIntervalMs && !finished_) {
    last_ask_ms_ = now;
    send_mdns_queries(false);
  }
  uint8_t buffer[1500];
  Ipv4Address from;
  uint16_t port;
  uint32_t length;
  for (int i = 0; mdns_ && i < 16 && mdns_->receive_from(ByteView(buffer, sizeof buffer), &from, &port, &length); ++i)
    if (length <= sizeof buffer) handle_mdns(ConstByteView(buffer, length));
  for (int i = 0; snmp_ && i < 16 && snmp_->receive_from(ByteView(buffer, sizeof buffer), &from, &port, &length); ++i)
    if (length <= sizeof buffer) handle_snmp(from, ConstByteView(buffer, length));
}

void PrinterDiscovery::handle_mdns(ConstByteView datagram) {
  mdns::Message& m = tables_->message;
  if (!mdns::decode_message(datagram, &m) || !m.response) return;
  bool changed = false;
  for (uint32_t i = 0; i < m.record_count; ++i) {
    const mdns::Record& r = m.records[i];
    uint32_t label = 0;
    switch (r.type) {
      case mdns::kTypePtr: {
        const Kind kind = type_kind(r.name);
        if (kind == Kind::None || !r.target[0]) break;
        if (r.ttl == 0) {   // goodbye
          if (Tables::Service* s = tables_->find_service(r.target)) *s = Tables::Service();
        } else {
          tables_->service_for(r.target, kind);
        }
        changed = true;
        break;
      }
      case mdns::kTypeSrv: {
        const Kind kind = instance_kind(r.name, &label);
        if (kind == Kind::None) break;
        if (Tables::Service* s = tables_->service_for(r.name, kind)) {
          s->have_srv = true;
          s->port = r.port;
          copy_text(s->host, sizeof s->host, r.target);
          changed = true;
        }
        break;
      }
      case mdns::kTypeTxt: {
        const Kind kind = instance_kind(r.name, &label);
        if (kind == Kind::None) break;
        if (Tables::Service* s = tables_->service_for(r.name, kind)) {
          s->have_txt = true;
          mdns::txt_value(r.txt, r.txt_length, "rp", s->rp, sizeof s->rp);
          if (!mdns::txt_value(r.txt, r.txt_length, "ty", s->ty, sizeof s->ty) && !mdns::txt_value(r.txt, r.txt_length, "product", s->ty, sizeof s->ty))
            mdns::txt_value(r.txt, r.txt_length, "usb_MDL", s->ty, sizeof s->ty);
          if (!mdns::txt_value(r.txt, r.txt_length, "usb_MFG", s->mfg, sizeof s->mfg)) mdns::txt_value(r.txt, r.txt_length, "mfg", s->mfg, sizeof s->mfg);
          mdns::txt_value(r.txt, r.txt_length, "pdl", s->pdl, sizeof s->pdl);
          changed = true;
        }
        break;
      }
      case mdns::kTypeA:
        if (r.ttl != 0)
          if (Tables::Host* h = tables_->host_for(r.name)) {
            h->address = r.address;
            changed = true;
          }
        break;
    }
  }
  if (changed) rebuild();
}

void PrinterDiscovery::handle_snmp(Ipv4Address from, ConstByteView datagram) {
  SnmpMessage m;
  if (!decode_snmp(datagram, &m) || m.pdu != SnmpPdu::Response || m.request_id != kProbeId || m.error_status != 0 || m.count < 4) return;
  // binds: sysDescr, sysName, hrDeviceType.1, prtGeneralPrinterName.1 - answered in order
  Oid printer_type;
  Oid::parse("1.3.6.1.2.1.25.3.1.5", &printer_type);
  const SnmpValue& type = m.binds[2].value;
  const SnmpValue& printer_name = m.binds[3].value;
  const bool is_printer = (type.type == SnmpType::ObjectId && type.oid == printer_type) || (printer_name.type == SnmpType::OctetString);
  if (!is_printer) return;
  Tables::Hit* hit = nullptr;
  for (Tables::Hit& h : tables_->hits)
    if (h.used && h.address == from) hit = &h;
  for (Tables::Hit& h : tables_->hits)
    if (!hit && !h.used) hit = &h;
  if (!hit) return;
  hit->used = true;
  hit->address = from;
  auto text = [](const SnmpValue& v, char* dst, uint32_t capacity) {
    uint32_t n = 0;
    if (v.type == SnmpType::OctetString)
      for (; n < v.length && n + 1 < capacity; ++n) dst[n] = static_cast<char>(v.bytes[n] >= 32 && v.bytes[n] < 127 ? v.bytes[n] : '?');
    dst[n] = 0;
  };
  text(m.binds[0].value, hit->description, sizeof hit->description);
  text(printer_name, hit->name, sizeof hit->name);
  if (!hit->name[0]) text(m.binds[1].value, hit->name, sizeof hit->name);
  rebuild();
}

// Recomputes the printer list from the services, hosts and SNMP answers, keeping the entries' order.
void PrinterDiscovery::rebuild() {
  Tables& t = *tables_;
  for (uint32_t i = 0; i < t.printer_count; ++i) {
    DiscoveredPrinter& p = t.printers[i];
    const Ipv4Address a = p.address;
    p = DiscoveredPrinter();
    p.address = a;
  }
  for (Tables::Service& s : t.services) {
    if (!s.used || !s.have_srv) continue;
    Tables::Host* h = t.find_host(s.host);
    if (!h || h->address.value() == 0) continue;
    DiscoveredPrinter* p = t.printer_for(h->address);
    if (!p) continue;
    p->by_mdns = true;
    uint32_t label = 0;
    instance_kind(s.instance, &label);
    if (!p->name[0]) {
      const uint32_t n = label < sizeof p->name - 1 ? label : sizeof p->name - 1;
      for (uint32_t i = 0; i < n; ++i) p->name[i] = s.instance[i];
      p->name[n] = 0;
    }
    if (!p->make_model[0]) copy_text(p->make_model, sizeof p->make_model, s.ty);
    if (!p->manufacturer[0]) copy_text(p->manufacturer, sizeof p->manufacturer, s.mfg);
    if (!p->pdl[0]) copy_text(p->pdl, sizeof p->pdl, s.pdl);
    switch (s.kind) {
      case Kind::Ipp:
        p->has_ipp = true;
        p->ipp_port = s.port;
        if (s.rp[0] == '/') copy_text(p->ipp_path, sizeof p->ipp_path, s.rp);
        else {
          p->ipp_path[0] = '/';
          copy_text(p->ipp_path + 1, sizeof p->ipp_path - 1, s.rp[0] ? s.rp : "ipp/print");
        }
        break;
      case Kind::Ipps:
        p->has_ipps = true;
        p->ipps_port = s.port;
        if (s.rp[0] == '/') copy_text(p->ipps_path, sizeof p->ipps_path, s.rp);
        else {
          p->ipps_path[0] = '/';
          copy_text(p->ipps_path + 1, sizeof p->ipps_path - 1, s.rp[0] ? s.rp : "ipp/print");
        }
        break;
      case Kind::Lpd:
        p->has_lpd = true;
        p->lpd_port = s.port;
        copy_text(p->lpd_queue, sizeof p->lpd_queue, s.rp);
        break;
      case Kind::Raw:
        p->has_raw = true;
        p->raw_port = s.port;
        break;
      default:
        break;
    }
  }
  for (Tables::Hit& hit : t.hits) {
    if (!hit.used) continue;
    DiscoveredPrinter* p = t.printer_for(hit.address);
    if (!p) continue;
    p->has_snmp = true;
    if (!p->name[0]) copy_text(p->name, sizeof p->name, hit.name);
    copy_text(p->description, sizeof p->description, hit.description);
  }
  // Entries with nothing left (a service said goodbye) leave the list.
  uint32_t kept = 0;
  for (uint32_t i = 0; i < t.printer_count; ++i) {
    const DiscoveredPrinter& p = t.printers[i];
    if (!(p.has_ipp || p.has_ipps || p.has_lpd || p.has_raw || p.has_snmp)) continue;
    if (kept != i) t.printers[kept] = p;
    ++kept;
  }
  t.printer_count = kept;
}

}  // namespace hardware
