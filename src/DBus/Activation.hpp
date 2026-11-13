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
//   User=root                      (system bus: handed to the launcher; the kernel has one user, so every service runs as it)
//   SystemdService=example.service (the systemd unit that runs the service: see below)
// Exec is split into words as a shell would, with '...' and "..." quoting and \ escapes, and nothing else (no variables, no globbing). The file name
// should be the name plus ".service"; a file whose Name is not a valid well-known name, or that has no Exec, is skipped.
//
// There is no systemd, so a service with SystemdService= is started from the unit file itself, as systemd would start it: the unit is looked for in the
// bus's unit folders (the first that has it), by its file name or, failing that, by an Alias= of a unit in them (systemctl enable makes those symbolic
// links; a file system that follows links finds them by name). Its [Service] ExecStart= is the command and its Environment= assignments are added to the
// environment. A unit that cannot be found, or whose command uses what is not supported here (variables, specifiers other than %%), is passed over and
// Exec= is run, as dbus-daemon does when it runs without systemd (distributions' system services often say Exec=/bin/false: they fail as they would there).
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

// What a systemd unit says about running its service (ExecStart's words, Environment's assignments).
struct UnitService {
  explicit UnitService(allocators::IAllocator& allocator) : argv(allocator), environment(allocator) {}
  List<String> argv;
  List<String> environment;      // "NAME=value"
};
static constexpr uint32_t kMaxUnitEnvironment = 64;
// Reads a unit file's [Service] group. False when it has no usable ExecStart= (none, empty, or with variables or specifiers this does not expand).
// ExecStart's prefixes are taken off ('-', ':', '+', '!', '!!'; '@' drops the argv[0] word that follows the path); lines may go on after a trailing '\'.
bool parse_unit_service(allocators::IAllocator& allocator, StringView text, UnitService* out);
// True when the unit's [Install] group lists `alias` in an Alias= line.
bool unit_has_alias(StringView text, StringView alias);

// What a service is started with.
struct ServiceStart {
  BusKind bus;
  StringView name;                              // the bus name it is to take
  ArrayView<const StringView> argv;             // argv[0]: the program's path as Linux programs see it
  ArrayView<const StringView> environment;      // "NAME=value", added to the standard environment (UpdateActivationEnvironment's, the unit's)
  StringView user;                              // the service file's User= (empty: none)
};

// What runs an activated service's program. Called on the bus's thread; must not block.
class IServiceLauncher {
 public:
  // Starts the program. Returns a handle for poll(), 0 if it could not even be started (*why says). The launcher adds DBUS_STARTER_ADDRESS and
  // DBUS_STARTER_BUS_TYPE after `environment` (they win over it).
  virtual uint64_t start(const ServiceStart& service, String* why) = 0;
  enum class State : uint8_t { Running, Exited, CouldNotRun };
  // How the program is doing; *exit_code once it has exited, *why when it could not run.
  virtual State poll(uint64_t handle, uint32_t* exit_code, String* why) = 0;
  // Ends the program (its activation timed out, as dbus-daemon kills the child then). poll() says Exited once it has ended. Does not wait.
  virtual void stop(uint64_t handle) = 0;
  // The bus no longer asks about it (it took its name, or the activation failed). A program that runs goes on.
  virtual void forget(uint64_t handle) = 0;

 protected:
  IServiceLauncher() = default;
  ~IServiceLauncher() = default;
};

}  // namespace dbus
