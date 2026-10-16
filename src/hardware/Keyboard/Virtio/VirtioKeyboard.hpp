#pragma once
#include "hardware/Bus/Virtio/VirtioInputDevice.hpp"
#include "hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// QEMU's virtio-keyboard-pci (and any virtio-input device that reports keys).
class VirtioKeyboard final : public KeyboardBase {
 public:
  VirtioKeyboard(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : device_(pci, device, allocator, st) {}

  const char* name() const override { return "virtio keyboard"; }
  bool initialize() override { return device_.initialize() && device_.kind() == VirtioInputKind::Keyboard; }
  void shutdown() override { device_.shutdown(); }

 private:
  void fetch() override;

  VirtioInputDevice device_;
};

}  // namespace hardware
