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
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Printer/IPrinter.hpp"
#include "Hardware/Printer/Usb/Dot4.hpp"
#include "Hardware/Printer/Usb/Ieee1284.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The printer's status byte (USB Printer Class GET_PORT_STATUS; bits as on a parallel port).
struct UsbPrinterStatus {
  bool paper_empty = false;
  bool selected = true;   // on line
  bool no_error = true;
  bool ok() const { return no_error && selected && !paper_empty; }
};

// Where a printer interface and its bulk endpoints are.
struct UsbPrinterEndpoints {
  uint8_t configuration = 1;
  uint8_t interface_number = 0;
  uint8_t alternate = 0;
  uint8_t protocol = 2;  // 1 unidirectional, 2 bidirectional, 3 IEEE 1284.4 (Dot4) multiplexed
  uint8_t out_endpoint = 0, in_endpoint = 0;  // in_endpoint is 0 for a unidirectional interface
  uint16_t out_max_packet = 0, in_max_packet = 0;
};

// A printer on USB (device class "printer", USB Printer Class 1.1): jobs go out on a bulk endpoint, the printer's IEEE 1284 device
// ID and port status come from class requests, and a bidirectional interface lets the printer answer (PJL status replies). It runs
// on any host controller that offers bulk endpoints. poll() sends at most 16 KiB of the job at the front of the queue per call, so
// a long job never holds the caller for long; a transfer that fails clears the endpoint's halt and is tried again, and a job that
// keeps failing is dropped after a printer soft reset.
class UsbPrinter final : public IPrinter {
 public:
  static constexpr uint32_t kQueueDepth = 8;
  static constexpr uint32_t kChunk = 16384;

  UsbPrinter(IUsbHostController& host, uint32_t port, const UsbPrinterEndpoints& endpoints, allocators::IAllocator& allocator)
      : host_(host), port_(port), ep_(endpoints), allocator_(allocator) {}
  ~UsbPrinter();

  // Adds a printer for every printer-class interface on every enumerated USB device (several alternate settings of an interface count
  // once, the bidirectional one preferred). Printers that do not start are left out.
  static void discover(IUsbBus& usb, allocators::IAllocator& allocator, List<UniquePointer<IPrinter>>& out);

  const char* name() const override { return name_; }
  bool initialize() override;
  void shutdown() override;
  const PrinterInfo& info() const override { return info_; }
  PrinterState state() const override { return count_ > 0 ? PrinterState::Busy : last_failed_ ? PrinterState::Error : PrinterState::Ready; }
  bool submit(ConstByteView job, const char* job_name) override;
  void poll() override;
  uint32_t pending_jobs() const override { return count_; }
  void cancel_all() override;
  PrinterStatistics statistics() const override { return stats_; }
  // A USB printer's port status gives only the error bits: paper out (no paper), off line (the select line), and the error line
  // (reported as "service requested"). Completes at once. Page counts and supplies need SNMP, which USB does not carry.
  bool request_report() override;
  const PrinterReport* report() const override { return have_report_ ? &report_ : nullptr; }

  const PrinterIdentity& identity() const { return identity_; }
  bool bidirectional() const { return ep_.in_endpoint != 0; }
  // True if jobs travel through an IEEE 1284.4 channel (protocol 3 interface).
  bool multiplexed() const { return dot4_ != nullptr; }
  const Dot4Session* dot4() const { return dot4_.get(); }
  // Asks the printer for its port status (paper out, off line, error).
  bool read_status(UsbPrinterStatus* status);
  // Reads whatever the printer has sent back (a bidirectional interface only) into `out`; waits for the transfer to end, so only
  // call it when the printer is expected to answer (after a PJL INFO request). *length receives the byte count.
  bool read_back(ByteView out, uint32_t* length);
  // Resets the printer's USB port (it drops buffered data).
  bool soft_reset();

 private:
  struct Job {
    explicit Job(allocators::IAllocator& a) : data(a) {}
    ByteBuffer data;
    uint64_t sent = 0;
    uint32_t failures = 0;
  };
  bool read_identity();
  void recover();
  void fail_front();

  IUsbHostController& host_;
  uint32_t port_;
  UsbPrinterEndpoints ep_;
  allocators::IAllocator& allocator_;
  ByteView wire_;  // transfer buffers (out, then in): one 64 KiB-aligned block
  UniquePointer<Dot4Session> dot4_;
  PrinterIdentity identity_;
  PrinterInfo info_;
  char name_[100] = "USB printer";
  UniquePointer<Job> queue_[kQueueDepth];
  uint32_t head_ = 0, count_ = 0;
  bool last_failed_ = false, started_ = false;
  PrinterStatistics stats_;
  PrinterReport report_;
  bool have_report_ = false;
};

}  // namespace hardware
