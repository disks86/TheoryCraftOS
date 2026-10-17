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
#include "Audio/StreamFormat.hpp"
#include "Utility/ArrayView.hpp"

namespace audio {

// Reads the header of a RIFF/WAVE file held in memory. Supports uncompressed PCM, 8 or 16 bits, mono
// or stereo, at any rate StreamFormat allows - the formats the audio streams play. Chunks other than
// "fmt " and "data" are skipped. A data chunk that claims more than the file holds is cut to the file.
class WavFile {
 public:
  // On success sets `format` and `data` (the sample bytes inside `file`, whole frames only).
  static bool parse(ConstByteView file, StreamFormat* format, ConstByteView* data);
};

}  // namespace audio
