// A host controller with four root ports into which fake USB audio devices (FakeAudioHost) can be plugged and from which they can be pulled, for
// the audio hot-plug tests. Every call is serialized, so threads may share it.
#pragma once
#include <mutex>
#include "FakeUsbAudio.hpp"

// A host controller with four root ports; a port holds a FakeAudioHost (one audio device) or nothing.
struct FakeAudioBay final : IUsbHostController {
  static constexpr uint32_t kPorts = 4;
  FakeAudioHost* dev[kPorts] = {};
  uint32_t generation[kPorts] = {};
  std::recursive_mutex m;

  const char* name() const override { return "fake bay"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return kPorts; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override {
    std::lock_guard<std::recursive_mutex> g(const_cast<std::recursive_mutex&>(m));
    return port < kPorts && dev[port] && dev[port]->port_info(0, out);
  }
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override {
    std::lock_guard<std::recursive_mutex> g(const_cast<std::recursive_mutex&>(m));
    return port < kPorts && dev[port] && dev[port]->device_descriptor(0, out);
  }
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override {
    std::lock_guard<std::recursive_mutex> g(const_cast<std::recursive_mutex&>(m));
    return port < kPorts && dev[port] && dev[port]->configuration_descriptor(0, buffer, length);
  }
#define FORWARD(port, call) std::lock_guard<std::recursive_mutex> g(m); return port < kPorts && dev[port] ? dev[port]->call : false
  bool set_configuration(uint32_t port, uint8_t c) override { FORWARD(port, set_configuration(0, c)); }
  bool control_out(uint32_t port, uint8_t t, uint8_t r, uint16_t v, uint16_t i) override { FORWARD(port, control_out(0, t, r, v, i)); }
  bool control_out_data(uint32_t port, uint8_t t, uint8_t r, uint16_t v, uint16_t i, ConstByteView d) override { FORWARD(port, control_out_data(0, t, r, v, i, d)); }
  bool control_in(uint32_t port, uint8_t t, uint8_t r, uint16_t v, uint16_t i, ByteView b) override { FORWARD(port, control_in(0, t, r, v, i, b)); }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
  bool supports_isochronous() const override { return true; }
  bool start_iso_endpoint(uint32_t port, uint8_t e, uint16_t mp, uint8_t i) override { FORWARD(port, start_iso_endpoint(0, e, mp, i)); }
  void stop_iso_endpoint(uint32_t port, uint8_t e) override {
    std::lock_guard<std::recursive_mutex> g(m);
    if (port < kPorts && dev[port]) dev[port]->stop_iso_endpoint(0, e);
  }
  uint32_t iso_interval_us(uint32_t port, uint8_t e) override {
    std::lock_guard<std::recursive_mutex> g(m);
    return port < kPorts && dev[port] ? dev[port]->iso_interval_us(0, e) : 0;
  }
  uint32_t iso_free_packets(uint32_t port, uint8_t e) override {
    std::lock_guard<std::recursive_mutex> g(m);
    return port < kPorts && dev[port] ? dev[port]->iso_free_packets(0, e) : 0;
  }
  bool iso_submit(uint32_t port, uint8_t e, ConstByteView p) override { FORWARD(port, iso_submit(0, e, p)); }
  uint64_t iso_completed_packets(uint32_t, uint8_t) override { return 0; }
  bool iso_receive(uint32_t port, uint8_t e, ByteView out, uint16_t* length) override { FORWARD(port, iso_receive(0, e, out, length)); }
  uint32_t port_generation(uint32_t port) const override { return port < kPorts ? __atomic_load_n(&generation[port], __ATOMIC_ACQUIRE) : 0; }
#undef FORWARD

  void plug(uint32_t port, FakeAudioHost* device) {
    std::lock_guard<std::recursive_mutex> g(m);
    dev[port] = device;
    __atomic_add_fetch(&generation[port], 1, __ATOMIC_ACQ_REL);
  }
  void unplug(uint32_t port) {
    std::lock_guard<std::recursive_mutex> g(m);
    dev[port] = nullptr;
    __atomic_add_fetch(&generation[port], 1, __ATOMIC_ACQ_REL);
  }
  // The device on the port has recorded a packet (microphone endpoint 0x81).
  void feed_capture(uint32_t port, const Bytes& packet) {
    std::lock_guard<std::recursive_mutex> g(m);
    if (port < kPorts && dev[port]) dev[port]->endpoints[0x81].incoming.push_back(packet);
  }
  // Time passing on the port's playback endpoint 1: everything queued has been sent.
  void play_out(uint32_t port) {
    std::lock_guard<std::recursive_mutex> g(m);
    if (port < kPorts && dev[port]) dev[port]->play_out(1);
  }
};
