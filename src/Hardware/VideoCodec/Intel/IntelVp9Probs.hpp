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

#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_vp9 {

// The probability tables the Intel video engine reads and writes for VP9, in the layout of the reference driver's FRAME_CONTEXT (vp9_probs.h): 2048 bytes in which the
// engine finds the probabilities a frame starts from and leaves the ones it adapted, with gaps ("dummy") where the engine keeps its own data. The tables are those of
// libvpx (BSD licensed, see IntelVp9Probs.cpp), rearranged for the engine.
constexpr uint32_t kProbabilityBufferBytes = 2048;
constexpr uint32_t kFirstPartBytes = 2010;      // the probabilities proper (behind them: segmentation probabilities the engine takes from the frame header)
constexpr uint32_t kKeyInterBytes = 343;        // the part of the buffer that holds the inter frame probabilities (the end of the first part)
constexpr uint32_t kContexts = 4;

// The state the driver keeps between frames: the four saved frame contexts and what the last frame was, which says whether and how the engine's output becomes a saved
// context. All of it is host memory; the engine only sees the buffer built for each frame.
class ProbabilityContexts {
 public:
  ProbabilityContexts();
  // A frame's parameters, as far as the contexts care.
  struct Frame {
    bool key = false, intra_only = false, error_resilient = false, refresh_frame_context = false;
    uint32_t reset_frame_context = 0, frame_context_idx = 0;
    bool segmentation_enabled = false, segmentation_update_map = false;
    uint8_t segment_tree_probs[7] = {}, segment_pred_probs[3] = {};
  };
  // Builds the buffer the engine starts the frame from. `engine_output` is the buffer as the previous frame left it (empty before the first frame).
  void prepare(const Frame& frame, ConstByteView engine_output, ByteView buffer);
  // Forgets the previous frame (the next one is a key frame).
  void reset();

 private:
  struct Last {
    bool valid = false, refresh_frame_context = false, key = false, intra_only = false;
    uint32_t frame_context_idx = 0;
  };
  uint8_t context_[kContexts][kProbabilityBufferBytes];
  Last last_;
};

}  // namespace intel_vp9
}  // namespace hardware
