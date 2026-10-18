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

// "name" or "name:port" as network filesystems name a server in their paths.
#pragma once
#include "Utility/String.hpp"

namespace network {

// Splits `host` into the name and the port (`default_port` when there is none, or it is not a number from 1 to 65535).
inline void split_host_port(StringView host, uint16_t default_port, StringView* name, uint16_t* port) {
  *name = host;
  *port = default_port;
  const uint64_t colon = host.rfind(':');
  if (colon == StringView::npos || colon == 0 || colon + 1 >= host.size() || host.size() - colon > 6) return;
  uint32_t value = 0;
  for (char c : host.substr(colon + 1)) {
    if (c < '0' || c > '9') return;
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  if (value == 0 || value > 65535) return;
  *name = host.first(colon);
  *port = static_cast<uint16_t>(value);
}

}  // namespace network
