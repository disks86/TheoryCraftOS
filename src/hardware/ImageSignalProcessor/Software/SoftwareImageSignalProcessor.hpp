#pragma once
#include "hardware/ImageSignalProcessor/IImageSignalProcessor.hpp"

namespace hardware {

// A CPU implementation of the ISP pipeline, usable on any machine: bilinear
// demosaicing, black-level subtraction, per-channel white-balance gain, then a
// gamma curve through a lookup table. Hardware ISP classes use it until they
// have native drivers.
class SoftwareImageSignalProcessor : public IImageSignalProcessor {
 public:
  static constexpr uint32_t kGammaEntries = 1024;

  SoftwareImageSignalProcessor();

  const char* name() const override { return "software ISP"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  uint32_t max_dimension() const override { return 16384; }

  bool set_settings(const IspSettings& settings) override;
  const IspSettings& settings() const override { return settings_; }

  bool process(const RawFrame& in, ImageBuffer* out) override;
  bool analyze(const RawFrame& in, IspStatistics* out) override;

 private:
  IspSettings settings_;
  uint8_t gamma_[kGammaEntries];
};

}  // namespace hardware
