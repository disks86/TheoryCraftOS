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

// USB serial ports of the Communications Device Class, Abstract Control Model (CDC-ACM, USB CDC 1.2 and PSTN 1.2): Arduinos and other
// microcontroller boards, modems, GPS receivers, the Framework Laptop 16's LED matrix module, debug consoles. Two interfaces: the
// communication interface (class 2, subclass 2) with its notification endpoint (interrupt IN: the modem lines), and the data interface
// (class 0x0A) with a bulk IN and a bulk OUT endpoint that carry the bytes.
//
// The framing goes out as SET_LINE_CODING and the RTS/DTR lines as SET_CONTROL_LINE_STATE, both on endpoint 0. Received bytes come from a
// bulk IN endpoint the host controller keeps armed (start_bulk_in_stream: xHCI and EHCI), read without waiting; sending is a bulk transfer
// of at most one staging buffer per poll, done outside the lock. A device whose controller cannot keep a bulk endpoint armed is not taken.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/SerialPort/ISerialPort.hpp"
#include "Hardware/SerialPort/SerialRing.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct UsbAcmFunction {
  uint8_t configuration = 0;
  uint8_t control_interface = 0, data_interface = 0;
  uint8_t notify_endpoint = 0;                       // 0: none (the lines are then not reported)
  uint16_t notify_max_packet = 0;
  uint8_t notify_interval = 0;
  uint8_t in_endpoint = 0, out_endpoint = 0;
  uint16_t in_max_packet = 0, out_max_packet = 0;
  uint8_t capabilities = 0;                          // bmCapabilities of the ACM functional descriptor (bit 1: line coding and control lines)
};

// Finds the first ACM function of a configuration: a communication interface of subclass 2 and the data interface its union descriptor (or,
// lacking one, the next interface) names, with both bulk endpoints. False if there is none. The configuration is untrusted.
[[nodiscard]] bool find_usb_acm_function(ConstByteView configuration, UsbAcmFunction* out);

// SET_LINE_CODING's 7 bytes for a configuration; false if the framing has no ACM form.
[[nodiscard]] bool encode_line_coding(const SerialConfig& config, ByteView out);
// The lines a SERIAL_STATE notification reports (SerialLines bits; *breaks and errors counted into `statistics`); false if it is not one.
[[nodiscard]] bool decode_serial_state(ConstByteView notification, uint8_t* lines, SerialStatistics* statistics);

class UsbCdcAcm final : public ISerialPort {
 public:
  static constexpr uint32_t kQueue = 16384, kStaging = 4096;

  // The ACM device on a root port, set up (rings, endpoints) but closed; null if it is none or cannot be served.
  static UniquePointer<UsbCdcAcm> open_device(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator);

  UsbCdcAcm(IUsbHostController& host, uint32_t port, const UsbAcmFunction& function, allocators::IAllocator& allocator, uint16_t vendor, uint16_t product);
  ~UsbCdcAcm();
  UsbCdcAcm(const UsbCdcAcm&) = delete;
  UsbCdcAcm& operator=(const UsbCdcAcm&) = delete;

  const char* name() const override { return name_; }
  bool initialize() override;
  bool present() const override { return host_.port_generation(port_) == generation_; }
  void shutdown() override;
  bool open(const SerialConfig& config) override;
  void close() override;
  SerialState state() const override;
  const SerialConfig& config() const override { return config_; }
  uint32_t write(ConstByteView data) override;
  uint32_t read(ByteView out) override;
  uint32_t bytes_readable() const override;
  uint32_t bytes_writable() const override;
  bool transmit_pending() const override;
  void discard_input() override;
  void poll() override;
  uint8_t lines() const override;
  void set_lines(uint8_t lines) override;
  void set_break(bool on) override;
  SerialStatistics statistics() const override;

  uint16_t vendor_id() const { return vendor_; }
  uint16_t product_id() const { return product_; }
  // Sets the name shown for the port (a known product's: the LED matrix module); cut to fit.
  void set_name(const char* name);

 private:
  bool control(uint8_t request, uint16_t value);
  bool send_line_coding(const SerialConfig& config);

  IUsbHostController& host_;
  uint32_t port_;
  UsbAcmFunction function_;
  allocators::IAllocator& allocator_;
  uint16_t vendor_, product_;
  uint32_t generation_;
  char name_[64] = {};
  mutable kernel::Mutex mutex_;          // the queues, the state, the lines, the statistics
  SerialRing rx_, tx_;
  ByteView staging_;                     // what one bulk OUT transfer sends (DMA memory)
  SerialConfig config_;
  SerialState state_ = SerialState::Closed;
  uint8_t lines_ = 0;                    // inputs from SERIAL_STATE, outputs as last set
  SerialStatistics statistics_;
  uint32_t in_flight_ = 0;               // bytes being sent (taken from tx_, not yet confirmed)
  bool ready_ = false;
};

}  // namespace hardware
