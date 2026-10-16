#pragma once
#include "hardware/IHardware.hpp"
#include "hardware/ImageSignalProcessor/ImageTypes.hpp"

namespace hardware {

// An image signal processor: turns raw Bayer sensor frames into displayable
// RGB images (demosaicing, black level, white balance, gamma) and measures
// frames for the 3A loops. Implementations live in subfolders: a CPU-based
// one that works anywhere, and per-vendor classes for hardware ISPs.
class IImageSignalProcessor : public IHardware {
 public:
  // Largest frame the processor accepts (either dimension).
  virtual uint32_t max_dimension() const = 0;

  virtual bool set_settings(const IspSettings& settings) = 0;
  virtual const IspSettings& settings() const = 0;

  // Converts `in` to RGB in `out`, which must be at least as large as the frame.
  // Returns false for unusable input (null/too small buffers, bad bit depth).
  virtual bool process(const RawFrame& in, ImageBuffer* out) = 0;
  // Measures a frame without producing an image.
  virtual bool analyze(const RawFrame& in, IspStatistics* out) = 0;

 protected:
  constexpr IImageSignalProcessor() = default;
  ~IImageSignalProcessor() = default;
};

// Gray-world auto white balance: gains that make the three channel means equal,
// keeping green at 1.0. Other settings are copied from `base`.
IspSettings auto_white_balance(const IspStatistics& statistics, const IspSettings& base);

}  // namespace hardware
