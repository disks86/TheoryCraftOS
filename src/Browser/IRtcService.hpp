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

// What a page needs of WebRTC: peer connections it can make, drive by name and let go of (the engine is in src/Rtc/; the page and its script natives only know this).
#pragma once
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"

namespace browser {

class IRtcService {
 public:
  // A new peer connection; `ice_servers` are the STUN/TURN addresses, one on each line. 0 (and *error) when it cannot be made.
  virtual uint32_t create(StringView ice_servers, String* error) = 0;
  // One operation on a connection (see Rtc/RtcService.cpp for the list). False: *out is the error, "Name: message".
  virtual bool op(uint32_t id, StringView name, StringView a, StringView b, ConstByteView bytes, int64_t n1, int64_t n2, String* out) = 0;
  virtual void release(uint32_t id) = 0;

 protected:
  IRtcService() = default;
  ~IRtcService() = default;
};

}  // namespace browser
