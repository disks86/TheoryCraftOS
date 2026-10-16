#pragma once
#include <stdint.h>
#include "hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

// A scrolling text console drawn through an IGraphicsProcessingUnit. It only
// uses the GPU interface (glyphs, fill and copy), so it works with every GPU
// class, including the UEFI framebuffer fallback.
class GraphicsConsole {
 public:
  // Starts (or restarts after a mode change) on `gpu` with a cleared screen.
  void attach(hardware::IGraphicsProcessingUnit* gpu);
  bool attached() const { return gpu_ != nullptr; }

  void put(char c);

 private:
  void new_line();

  static constexpr uint32_t kForeground = 0xE0E0E0;
  static constexpr uint32_t kBackground = 0x101018;

  hardware::IGraphicsProcessingUnit* gpu_ = nullptr;
  uint32_t cols_ = 0, rows_ = 0, col_ = 0, row_ = 0;
};
