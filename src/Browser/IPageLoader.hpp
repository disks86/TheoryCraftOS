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

// How the browser gets a web page: an IPageLoader fetches the address it is given and says what came back. The browser does not know how (the
// system's loader speaks HTTP over the network stacks; a test hands out canned pages). fetch() waits for the answer, so it is called from the
// handler thread and not the input thread.
#pragma once
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace browser {

struct PageResult {
  explicit PageResult(allocators::IAllocator& allocator) : final_address(allocator), content_type(allocator), error(allocator), body(allocator) {}

  uint32_t status = 0;        // the HTTP status (200, 404, ...)
  String final_address;       // where the page really came from, after redirects
  String content_type;        // as the server said it ("text/html; charset=utf-8"), may be empty
  String error;               // when fetch() returns false: what went wrong
  ByteBuffer body;
};

class IPageLoader {
 public:
  // Fetches `address` (an http:// or https:// address). True if an answer arrived - whatever its status; false (with `error` set) if not.
  virtual bool fetch(StringView address, PageResult* result) = 0;

 protected:
  IPageLoader() = default;
  ~IPageLoader() = default;
};

}  // namespace browser
