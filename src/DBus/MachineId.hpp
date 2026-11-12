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

// The ids the buses need: the machine's (Peer.GetMachineId; the file /etc/machine-id that libdbus and sd-bus read, made once and kept) and a bus's own
// (GetId and the authentication's OK: new at each start). Both are 32 lowercase hex digits.
#pragma once
#include "Crypto/Random.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ArrayView.hpp"

namespace dbus {

// 16 random bytes as 32 hex digits and a NUL into `out` (33 chars). False if the entropy source failed.
bool random_hex_id(crypto::IEntropySource& entropy, ArrayView<char> out);
// The machine id from filesystem::layout::kLinuxMachineId; when it is missing or not an id, a new one, written there (`files` may be null: then only made).
bool load_machine_id(filesystem::IFileSystem* files, crypto::IEntropySource& entropy, ArrayView<char> out);
bool is_hex_id(StringView text);

}  // namespace dbus
