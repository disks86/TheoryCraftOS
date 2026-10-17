// A host controller with four root ports into which fake USB cameras (FakeCameraHost) can be plugged and from which they can be pulled, for the
// camera hot-plug tests. Every call is serialized, so threads may share it.
#pragma once
#include <mutex>
#include "FakeUsbCamera.hpp"

struct FakeCameraBay final : IUsbHostController {
  static constexpr uint32_t kPorts = 4;
  FakeCameraHost* dev[kPorts] = {};
  uint32_t generation[kPorts] = {};
  mutable std::recursive_mutex m;

  const char* name() const override { return "fake camera bay"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return kPorts; }
#define LOCKED std::lock_guard<std::recursive_mutex> g(m)
  bool port_info(uint32_t port, UsbPortInfo* out) const override { LOCKED; return port < kPorts && dev[port] && dev[port]->port_info(0, out); }
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override { LOCKED; return port < kPorts && dev[port] && dev[port]->device_descriptor(0, out); }
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override {
    LOCKED;
    return port < kPorts && dev[port] && dev[port]->configuration_descriptor(0, buffer, length);
  }
  bool set_configuration(uint32_t port, uint8_t c) override { LOCKED; return port < kPorts && dev[port] && dev[port]->set_configuration(0, c); }
  bool control_out(uint32_t port, uint8_t t, uint8_t r, uint16_t v, uint16_t i) override { LOCKED; return port < kPorts && dev[port] && dev[port]->control_out(0, t, r, v, i); }
  bool control_out_data(uint32_t port, uint8_t t, uint8_t r, uint16_t v, uint16_t i, ConstByteView d) override {
    LOCKED;
    return port < kPorts && dev[port] && dev[port]->control_out_data(0, t, r, v, i, d);
  }
  bool control_in(uint32_t port, uint8_t t, uint8_t r, uint16_t v, uint16_t i, ByteView b) override {
    LOCKED;
    return port < kPorts && dev[port] && dev[port]->control_in(0, t, r, v, i, b);
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
  bool supports_isochronous() const override { return true; }
  bool start_iso_endpoint(uint32_t port, uint8_t e, uint16_t mp, uint8_t i) override { LOCKED; return port < kPorts && dev[port] && dev[port]->start_iso_endpoint(0, e, mp, i); }
  void stop_iso_endpoint(uint32_t port, uint8_t e) override { LOCKED; if (port < kPorts && dev[port]) dev[port]->stop_iso_endpoint(0, e); }
  uint32_t iso_interval_us(uint32_t port, uint8_t e) override { LOCKED; return port < kPorts && dev[port] ? dev[port]->iso_interval_us(0, e) : 0; }
  bool iso_receive(uint32_t port, uint8_t e, ByteView out, uint16_t* length) override {
    LOCKED;
    return port < kPorts && dev[port] && dev[port]->iso_receive(0, e, out, length);
  }
  uint32_t port_generation(uint32_t port) const override { return port < kPorts ? __atomic_load_n(&generation[port], __ATOMIC_ACQUIRE) : 0; }

  void plug(uint32_t port, FakeCameraHost* device) {
    LOCKED;
    dev[port] = device;
    __atomic_add_fetch(&generation[port], 1, __ATOMIC_ACQ_REL);
  }
  void unplug(uint32_t port) {
    LOCKED;
    dev[port] = nullptr;
    __atomic_add_fetch(&generation[port], 1, __ATOMIC_ACQ_REL);
  }
  // The camera on the port sends these packets on its streaming endpoint (0x81).
  void send(uint32_t port, const std::vector<Bytes>& packets) {
    LOCKED;
    if (port < kPorts && dev[port]) feed(*dev[port], 0x81, packets);
  }
#undef LOCKED
};
