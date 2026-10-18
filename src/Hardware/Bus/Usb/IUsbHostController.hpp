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
#include "Hardware/Bus/Usb/UsbTypes.hpp"
#include "Utility/ArrayView.hpp"
#include "Hardware/IHardware.hpp"

namespace hardware {

// One USB host controller (UHCI, OHCI, EHCI, xHCI, ...). initialize() brings
// the controller up and enumerates what is plugged into its root ports.
class IUsbHostController : public IHardware {
 public:
  virtual UsbControllerKind kind() const = 0;

  virtual uint32_t port_count() const = 0;  // root ports
  virtual bool port_info(uint32_t port, UsbPortInfo* out) const = 0;

  // Descriptor of the device enumerated on a root port during initialize().
  // False if nothing was enumerated there.
  virtual bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const = 0;
  // The full configuration descriptor (configuration, interfaces, endpoints, class
  // descriptors) of the device on a root port, cached during enumeration.
  virtual bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const = 0;

  // --- Device control (endpoint 0). `port` identifies an enumerated device.
  // Selects a configuration; repeating the call for the same value is a no-op.
  virtual bool set_configuration(uint32_t port, uint8_t configuration) = 0;
  // Control transfer with no data stage.
  virtual bool control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                           uint16_t index) = 0;
  // Control transfer reading `length` bytes (at most 4096) into `buffer`.
  virtual bool control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                          uint16_t index,
                  ByteView buffer) = 0;

  // Control transfer with a data stage from host to device (class requests that set something: a sampling rate, a clock). Default: unsupported.
  virtual bool control_out_data(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) {
    (void)port; (void)request_type; (void)request; (void)value; (void)index; (void)data;
    return false;
  }

  // --- Hot-plug. A controller that notices devices coming and going after start-up says so here (xHCI); the others only know the devices
  // present when initialize() ran.
  virtual bool supports_hotplug() const { return false; }
  // Looks at the root ports and reports at most one change: a device that was unplugged (its slot and endpoints are already released, so
  // everything addressed to that port fails from now on) or a new one, enumerated and ready (descriptors readable). Call it every few
  // hundred milliseconds from one thread; false when nothing changed. May block while a transfer to the port is still running.
  virtual bool poll_hotplug(UsbHotplugEvent* event) { (void)event; return false; }
  // Changes whenever a device on the port is enumerated or released. A driver notes it when it takes the device over and treats the device
  // as gone once the number differs: the port may hold a different device by then.
  virtual uint32_t port_generation(uint32_t port) const { (void)port; return 0; }

  // --- Interrupt IN endpoints (HID reports and the like).
  // Configures the endpoint and starts receiving packets of up to max_packet
  // bytes. `interval` is the descriptor's bInterval.
  virtual bool start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                                  uint8_t interval) = 0;
  // If a packet has arrived, copies it out (at most `capacity` bytes), re-arms
  // the endpoint and returns true. Never blocks.
  virtual bool poll_interrupt_in(uint32_t port, uint8_t endpoint_address, ByteView buffer, uint16_t* length) = 0;

  // A bulk IN endpoint that is received from without waiting (USB MIDI input: the device sends when a key is pressed, which may be never):
  // started like an interrupt IN endpoint, and read with poll_interrupt_in(), which hands out what has arrived and never blocks. The controller
  // keeps one transfer of `max_packet` bytes queued at all times. Controllers that cannot do this refuse (the default); the others are xHCI and EHCI.
  virtual bool start_bulk_in_stream(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) {
    (void)port; (void)endpoint_address; (void)max_packet;
    return false;
  }

  // --- Bulk endpoints (mass storage and the like).
  // Configures a bulk endpoint of an enumerated device (IN when bit 7 of the
  // address is set). Repeating the call for a running endpoint is a no-op.
  virtual bool start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) = 0;
  // Moves `length` bytes (at most 64 KiB, within one 64 KiB-aligned window of
  // memory) and waits for the transfer to end. *transferred, if given, receives
  // the bytes actually moved: a short IN transfer is a success. False on a
  // timeout, a stall or any other error.
  virtual bool bulk_transfer(uint32_t port, uint8_t endpoint_address, ByteView data, uint32_t* transferred) = 0;
  // Brings a stalled bulk endpoint back to life (the controller side of
  // CLEAR_FEATURE(ENDPOINT_HALT), which the caller still sends).
  virtual bool reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) = 0;

  // --- Isochronous endpoints (audio): a steady stream of packets, one per service interval, delivered on time or lost, never retried.
  // Controllers that cannot do them say so here and refuse the calls below (the defaults).
  virtual bool supports_isochronous() const { return false; }
  // Sets up the endpoint of an enumerated device and its queue of packets. `max_packet` is wMaxPacketSize (bits 10:0) and `interval` the descriptor's bInterval.
  // Do this before the device is switched to the alternate setting that uses the endpoint (SET_INTERFACE), and stop it before switching back.
  virtual bool start_iso_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet, uint8_t interval) {
    (void)port; (void)endpoint_address; (void)max_packet; (void)interval;
    return false;
  }
  virtual void stop_iso_endpoint(uint32_t port, uint8_t endpoint_address) { (void)port; (void)endpoint_address; }
  // How long a service interval of the endpoint is (what one packet covers), in microseconds; 0 if the endpoint is not running.
  virtual uint32_t iso_interval_us(uint32_t port, uint8_t endpoint_address) { (void)port; (void)endpoint_address; return 0; }
  // OUT: how many more packets iso_submit() takes right now.
  virtual uint32_t iso_free_packets(uint32_t port, uint8_t endpoint_address) { (void)port; (void)endpoint_address; return 0; }
  // OUT: queues one packet (at most max_packet bytes; copied) for the first service interval after the ones already queued. False if the queue is full.
  virtual bool iso_submit(uint32_t port, uint8_t endpoint_address, ConstByteView packet) { (void)port; (void)endpoint_address; (void)packet; return false; }
  // Packets the controller has finished with since the endpoint started (OUT: sent or missed, IN: received or missed).
  virtual uint64_t iso_completed_packets(uint32_t port, uint8_t endpoint_address) { (void)port; (void)endpoint_address; return 0; }
  // IN: copies out the oldest packet received and not yet taken (an empty one if a service interval passed without data); false if there is none.
  virtual bool iso_receive(uint32_t port, uint8_t endpoint_address, ByteView out, uint16_t* length) {
    (void)port; (void)endpoint_address; (void)out; (void)length;
    return false;
  }

 protected:
  constexpr IUsbHostController() = default;
  ~IUsbHostController() = default;
};

}  // namespace hardware
