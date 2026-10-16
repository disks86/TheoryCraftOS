#include "GraphicsConsole.hpp"

void GraphicsConsole::attach(hardware::IGraphicsProcessingUnit* gpu) {
  gpu_ = gpu;
  col_ = row_ = 0;
  cols_ = rows_ = 0;
  if (!gpu_) return;
  cols_ = gpu_->width() / gpu_->glyph_width();
  rows_ = gpu_->height() / gpu_->glyph_height();
  if (!cols_ || !rows_) {
    gpu_ = nullptr;
    return;
  }
  gpu_->clear(kBackground);
}

void GraphicsConsole::new_line() {
  col_ = 0;
  if (++row_ < rows_) return;
  row_ = rows_ - 1;  // scroll up by one text row
  const uint32_t gh = gpu_->glyph_height();
  gpu_->copy_rect(0, gh, 0, 0, gpu_->width(), (rows_ - 1) * gh);
  gpu_->fill_rect(0, (rows_ - 1) * gh, gpu_->width(), gh, kBackground);
}

void GraphicsConsole::put(char c) {
  if (!gpu_) return;
  if (c == '\r') return;
  if (c == '\n') {
    new_line();
    return;
  }
  if (col_ == cols_) new_line();
  gpu_->draw_char(col_ * gpu_->glyph_width(), row_ * gpu_->glyph_height(), c, kForeground,
                  kBackground);
  ++col_;
}
