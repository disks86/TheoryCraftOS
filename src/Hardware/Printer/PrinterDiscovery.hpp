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

#pragma once
#include "Network/INetworkStack.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A printer found on the network, with what is known about how to reach it. The same printer found through several service
// advertisements (IPP, LPD, raw 9100) or through SNMP as well is one entry, matched by IP address.
struct DiscoveredPrinter {
  network::Ipv4Address address;
  char name[64] = {};          // the service instance name ("Brother HL-2070N series"), else the SNMP name
  char make_model[64] = {};    // the TXT "ty" (or "product", "usb_MDL")
  char manufacturer[32] = {};  // "usb_MFG" / "mfg"
  char pdl[96] = {};           // the page description languages announced ("application/postscript,application/vnd.hp-PCL")
  char description[80] = {};   // sysDescr from SNMP
  bool has_ipp = false, has_lpd = false, has_raw = false, has_snmp = false;
  bool by_mdns = false;
  uint16_t ipp_port = 631, lpd_port = 515, raw_port = 9100;
  char ipp_path[48] = {};      // the resource path from TXT "rp", with a leading slash ("/ipp/print")
  char lpd_queue[32] = {};     // the queue from the _printer service's TXT "rp"
  // A printer found only by SNMP is assumed to take raw jobs on port 9100 (HP JetDirect, Brother and most print servers do); this was not checked.
  bool raw_assumed() const { return !has_raw && has_snmp && !has_ipp && !has_lpd; }
  bool supports_pcl() const;
  bool supports_postscript() const;
};

// Looks for printers on the local network, two ways at once: multicast DNS service discovery (queries for _ipp._tcp, _printer._tcp and
// _pdl-datastream._tcp, then SRV, TXT and A for what answers) and an SNMP probe (a v2c GET for the printer MIB objects, sent to the broadcast
// addresses; printers answer directly). Polled: call poll() while the stack is polled; results accumulate in at(), and finished() turns true
// once the query schedule has run out (the listening goes on until stop()). Not tried on a real network.
class PrinterDiscovery {
 public:
  static constexpr uint32_t kMaxPrinters = 16;
  PrinterDiscovery(network::INetworkStack& stack, allocators::IAllocator& allocator) : stack_(stack), allocator_(allocator) {}
  ~PrinterDiscovery() { stop(); }
  PrinterDiscovery(const PrinterDiscovery&) = delete;
  PrinterDiscovery& operator=(const PrinterDiscovery&) = delete;

  static constexpr uint8_t kMdns = 1, kSnmp = 2;
  // Starts the search; forgets earlier results. False if the sockets or memory were not available.
  bool start(uint8_t methods = kMdns | kSnmp);
  void stop();
  void poll();
  bool running() const { return running_; }
  bool finished() const { return running_ && finished_; }
  void set_snmp_community(const char* community);
  // Sends the SNMP probe to one address (a printer known to be there but silent to broadcasts).
  bool probe_host(network::Ipv4Address address);

  uint32_t count() const;
  const DiscoveredPrinter& at(uint32_t index) const;
  // The entry for an address, or nullptr.
  const DiscoveredPrinter* find(network::Ipv4Address address) const;

 private:
  struct Tables;
  void send_mdns_queries(bool browse);
  void send_snmp_probe();
  void handle_mdns(ConstByteView datagram);
  void handle_snmp(network::Ipv4Address from, ConstByteView datagram);
  void rebuild();
  network::INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  UniquePointer<Tables> tables_;
  SharedPointer<network::IUdpSocket> mdns_, snmp_;
  uint8_t methods_ = 0;
  bool running_ = false, finished_ = false, joined_ = false;
  uint64_t started_ms_ = 0, last_ask_ms_ = 0;
  uint32_t round_ = 0;
  char community_[32] = "public";
};

}  // namespace hardware
