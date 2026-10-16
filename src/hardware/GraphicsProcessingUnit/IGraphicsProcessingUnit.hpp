#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// A display-capable graphics device. Colors are 0x00RRGGBB.
// Implementations live beside this file.
class IGraphicsProcessingUnit : public IHardware {
 public:
  virtual uint32_t width() const = 0;   // pixels
  virtual uint32_t height() const = 0;  // pixels
  virtual void put_pixel(uint32_t x, uint32_t y, uint32_t color) = 0;
  virtual void clear(uint32_t color) = 0;

 protected:
  constexpr IGraphicsProcessingUnit() = default;
  ~IGraphicsProcessingUnit() = default;
};

}  // namespace hardware
