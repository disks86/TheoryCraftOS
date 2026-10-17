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
#include "Network/Ipp/IppExchange.hpp"
#include "Network/Ipp/IppMessage.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// One job as the printer's queue lists it (Get-Jobs).
struct IppJobInfo {
  uint32_t id = 0;
  uint32_t state = 0;       // job-state: 3 pending, 4 held, 5 processing, 6 stopped, 7 canceled, 8 aborted, 9 completed
  uint32_t size_kb = 0;     // job-k-octets
  char name[48] = {};
  bool active() const { return state >= 3 && state <= 6; }
};

// A printer reached over IPP/1.1 (RFC 8011): jobs go out as Print-Job requests over HTTP on TCP port 631, and the report comes from
// Get-Printer-Attributes (state, state reasons, marker levels). Like NetworkPrinter it is polled: the network stack must be polled
// too. Status, queue listing and cancelling run one at a time between jobs.
//
// The job bytes are what PrintJob builds (PCL or PostScript); `document_format` is the MIME type announced with them
// ("application/octet-stream" lets the printer sniff the language).
class IppPrinter : public IPrinter {
 public:
  static constexpr uint32_t kQueueDepth = 8, kMaxJobs = 8;
  IppPrinter(network::INetworkStack& stack, network::Ipv4Address address, const PrinterInfo& info, allocators::IAllocator& allocator,
             const char* path = "/ipp/print", uint16_t port = 631)
      : stack_(stack), address_(address), info_(info), allocator_(allocator), path_(path), port_(port), exchange_(stack, allocator), head_(allocator) {}
  ~IppPrinter() { cancel_all(); }

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
  uint32_t ipv4_address() const override { return address_.value(); }
  bool request_report() override;
  bool report_pending() const override { return report_requested_ || op_ == Op::Report; }
  const PrinterReport* report() const override { return have_report_ ? &report_ : nullptr; }

  // Asks for the queue (Get-Jobs, jobs not yet completed); jobs() is current when jobs_pending() turns false.
  bool request_jobs();
  bool jobs_pending() const { return jobs_requested_ || op_ == Op::Jobs; }
  uint32_t job_count() const { return job_count_; }
  const IppJobInfo& job(uint32_t index) const { return jobs_[index]; }
  // Cancels a job on the printer (Cancel-Job). cancel_result() is the IPP status once cancel_pending() is false.
  bool cancel_job(uint32_t job_id);
  bool cancel_pending() const { return cancel_requested_ || op_ == Op::Cancel; }
  uint16_t cancel_result() const { return cancel_result_; }

  void set_document_format(const char* mime) { format_ = mime; }
  void set_user_name(const char* user) { user_ = user; }
  // The job-id the printer gave the last job it accepted (0 if none yet).
  uint32_t last_job_id() const { return last_job_id_; }
  // The IPP status of the last answer (0x0000 successful-ok, 0x0400 client-error-bad-request, ...), or 0xffff if the last exchange failed below IPP.
  uint16_t last_status() const { return last_status_; }
  uint32_t http_status() const { return exchange_.http_status(); }

 private:
  enum class Op : uint8_t { None, Print, Report, Jobs, Cancel };
  struct Job {
    explicit Job(allocators::IAllocator& a) : data(a) {}
    ByteBuffer data;
    char name[48] = {};
    uint32_t attempts = 0;
  };
  void begin(Op op);
  void finish(Op op);
  bool build_header(uint16_t operation);
  bool build_printer_uri(char* out, uint32_t capacity) const;
  void fill_report(const network::ipp::Response& r);
  void fill_jobs(const network::ipp::Response& r);

  network::INetworkStack& stack_;
  network::Ipv4Address address_;
  PrinterInfo info_;
  allocators::IAllocator& allocator_;
  const char* path_;
  uint16_t port_;
  const char* format_ = "application/octet-stream";
  const char* user_ = "theorycraftos";
  network::IppExchange exchange_;
  ByteBuffer head_;
  Op op_ = Op::None;
  uint32_t request_id_ = 0;
  uint64_t retry_at_ms_ = 0;

  UniquePointer<Job> queue_[kQueueDepth];
  uint32_t head_index_ = 0, count_ = 0;
  PrinterStatistics stats_;
  bool last_failed_ = false;
  uint32_t last_job_id_ = 0;
  uint16_t last_status_ = 0;

  bool report_requested_ = false, jobs_requested_ = false, cancel_requested_ = false;
  uint32_t cancel_id_ = 0;
  uint16_t cancel_result_ = 0;
  PrinterReport report_;
  bool have_report_ = false;
  IppJobInfo jobs_[kMaxJobs];
  uint32_t job_count_ = 0;
};

}  // namespace hardware
