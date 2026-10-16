#pragma once
#include "hardware/IHardware.hpp"
#include "uefi.hpp"

namespace hardware {

// Firmware text output (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL on ConOut).
class TextConsole final : public IHardware {
 public:
  constexpr TextConsole() = default;

  // Must be called before initialize().
  void bind(efi::SystemTable* st) { st_ = st; }

  const char* name() const override { return "UEFI text console"; }
  bool initialize() override { return st_ && st_->con_out; }
  void shutdown() override { st_ = nullptr; }

  void put(char c) const;

 private:
  efi::SystemTable* st_ = nullptr;
};

}  // namespace hardware
