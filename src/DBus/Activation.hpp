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

// Starting services on demand ("activation"): the .service files that say which program owns a name, and what starts that program.
//
// A service file is a desktop-entry style file, as dbus-daemon reads them from /usr/share/dbus-1/services (session) and system-services (system):
//   [D-BUS Service]
//   Name=org.example.Service
//   Exec=/usr/bin/example-service --flag "an argument"
//   User=root                      (system bus; read and ignored: there is one user)
//   SystemdService=example.service (read and ignored: there is no systemd)
// Exec is split into words as a shell would, with '...' and "..." quoting and \ escapes, and nothing else (no variables, no globbing). The file name
// should be the name plus ".service"; a file whose Name is not a valid well-known name, or that has no Exec, is skipped.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Allocators/IAllocator.hpp"

namespace dbus {

enum class BusKind : uint8_t;

struct ServiceFile {
  explicit ServiceFile(allocators::IAllocator& allocator) : name(allocator), exec(allocator), user(allocator), systemd_service(allocator) {}
  String name, exec, user, systemd_service;
};

static constexpr uint64_t kMaxServiceFileBytes = 64 * 1024;

// Parses one service file. False when it is not one (no [D-BUS Service] group, no valid Name, no Exec).
bool parse_service_file(StringView text, ServiceFile* out);
// Splits an Exec line into words. False on an unterminated quote, an empty line or more than kMaxExecWords words.
static constexpr uint32_t kMaxExecWords = 64;
bool split_exec(allocators::IAllocator& allocator, StringView line, List<String>* words);

// What runs an activated service's program. Called on the bus's thread; must not block.
class IServiceLauncher {
 public:
  // Starts the program (`argv[0]` is its path as Linux programs see it). Returns a handle for poll(), 0 if it could not even be started (*why says).
  virtual uint64_t start(BusKind bus, StringView service_name, ArrayView<const StringView> argv, String* why) = 0;
  enum class State : uint8_t { Running, Exited, CouldNotRun };
  // How the program is doing; *exit_code once it has exited, *why when it could not run.
  virtual State poll(uint64_t handle, uint32_t* exit_code, String* why) = 0;
  // The bus no longer asks about it (it took its name, or the activation failed). The program itself goes on if it runs.
  virtual void forget(uint64_t handle) = 0;

 protected:
  IServiceLauncher() = default;
  ~IServiceLauncher() = default;
};

}  // namespace dbus
