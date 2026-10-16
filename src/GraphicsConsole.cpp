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
  gpu_->draw_char(col_ * gpu_->glyph_width(), row_ * gpu_->glyph_height(), c, foreground_,
                  kBackground);
  ++col_;
}
