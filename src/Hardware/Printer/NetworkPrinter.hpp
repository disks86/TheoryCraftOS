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
#include "Hardware/Printer/IPrinter.hpp"
#include "Hardware/Printer/PrintTransport.hpp"
#include "Hardware/Printer/PrinterMonitor.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

enum class PrintProtocol : uint8_t { Raw9100, Lpd };

// A printer on the network with its own print server: jobs are queued here and delivered one at a time over raw TCP (port 9100)
// or LPD (port 515). The network stack must be polled for jobs to move (INetworkStack::poll(), or a NetworkStackSet) -
// poll() here only drives the delivery. A failed delivery is retried twice before the job is dropped and state() says Error.
class NetworkPrinter : public IPrinter {
 public:
  static constexpr uint32_t kQueueDepth = 8;
  NetworkPrinter(network::INetworkStack& stack, network::Ipv4Address address, const PrinterInfo& info, allocators::IAllocator& allocator,
                 PrintProtocol protocol = PrintProtocol::Raw9100)
      : stack_(stack), address_(address), info_(info), allocator_(allocator), protocol_(protocol) {}
  ~NetworkPrinter() { cancel_all(); }

  const char* name() const override { return info_.model; }
  bool initialize() override { return true; }
  void shutdown() override { cancel_all(); }
  const PrinterInfo& info() const override { return info_; }
  PrinterState state() const override { return count_ > 0 ? PrinterState::Busy : last_failed_ ? PrinterState::Error : PrinterState::Ready; }
  bool submit(ConstByteView job, const char* job_name) override;
  void poll() override;
  uint32_t pending_jobs() const override { return count_; }
  void cancel_all() override;
  PrinterStatistics statistics() const override { return stats_; }
  // Status over SNMP (UDP port 161): see PrinterMonitor. A printer that does not answer leaves report() null and
  // report_failed() true.
  bool request_report() override;
  bool report_pending() const override { return monitor_ && monitor_->busy(); }
  const PrinterReport* report() const override { return have_report_ ? &report_ : nullptr; }
  bool report_failed() const { return monitor_ && monitor_->failed(); }
  void set_snmp_community(const char* community) { community_ = community; }
  // Asks with SNMPv3 (user-based security) instead of a community: the user, and the algorithms and passwords it was set up with on
  // the printer (passwords of at least 8 characters). Takes effect for the next report. False if the arguments cannot be used.
  bool set_snmp_v3_user(const char* user, network::UsmAuth auth, const char* auth_password, network::UsmPriv priv, const char* priv_password);

  network::Ipv4Address address() const { return address_; }
  uint32_t ipv4_address() const override { return address_.value(); }
  PrintProtocol protocol() const { return protocol_; }
  void set_protocol(PrintProtocol protocol) { protocol_ = protocol; }
  void set_lpd_queue(const char* queue) { lpd_queue_ = queue; }

 private:
  struct Job {
    explicit Job(allocators::IAllocator& a) : data(a) {}
    ByteBuffer data;
    char name[48] = {};
    uint32_t attempts = 0;
  };
  network::INetworkStack& stack_;
  network::Ipv4Address address_;
  PrinterInfo info_;
  allocators::IAllocator& allocator_;
  PrintProtocol protocol_;
  const char* lpd_queue_ = "BINARY_P1";
  UniquePointer<Job> queue_[kQueueDepth];
  uint32_t head_ = 0, count_ = 0;
  UniquePointer<IPrintTransport> transport_;
  bool last_failed_ = false;
  PrinterStatistics stats_;
  const char* community_ = "public";
  bool v3_ = false;
  char v3_user_[32] = {}, v3_auth_password_[64] = {}, v3_priv_password_[64] = {};
  network::UsmAuth v3_auth_ = network::UsmAuth::None;
  network::UsmPriv v3_priv_ = network::UsmPriv::None;
  UniquePointer<PrinterMonitor> monitor_;
  PrinterReport report_;
  bool have_report_ = false;
};

}  // namespace hardware
