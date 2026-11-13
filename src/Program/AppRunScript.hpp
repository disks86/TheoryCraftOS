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
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace desktop {

// The program an AppImage's AppRun ends up starting, with its arguments and environment.
struct AppRunLaunch {
  explicit AppRunLaunch(allocators::IAllocator& allocator) : program(allocator), arguments(allocator), environment(allocator) {}
  String program;                // its path in the Linux world, symbolic links resolved ("/tmp/.mount_App/usr/bin/app")
  List<String> arguments;        // after argv[0]
  List<String> environment;      // "NAME=value": every exported variable when it was started
};

enum class AppRunResult : uint8_t {
  Launch,         // *launch says what to run
  NoProgram,      // the script ended (or failed) without starting a program it could find
  OutOfMemory,
};

// Works out what the AppRun at `entry` (a path in the Linux world, whose files are `files` below `world_root`, "/Linux") would start.
//
// An ELF AppRun is the program itself. A shell script AppRun (what electron-builder, linuxdeploy and most hand-made AppImages ship) cannot run here
// as it is: the Linux layer has no fork and exec, so a real shell could neither run the commands the script substitutes nor hand over to the
// program. Instead the script is interpreted by a small shell of this OS's own, enough of POSIX sh and bash for these launchers:
//   - variables, export/readonly/local/unset, positional parameters, shift, arrays (`args=("$@")`, "${args[@]}", NAME=(...)),
//     parameter expansion ($X ${X} ${X:-w} ${X:+w} ${X:=w} ${X-w} ${X+w} ${#X} ${X%p} ${X%%p} ${X#p} ${X##p}), quoting, field splitting,
//     globbing in the last component of a path ("$dir"/apprun-hooks/*.sh);
//   - if/elif/else, while/until, for, case is skipped, && || ! and pipelines (by exit status), functions, return, break, continue,
//     `trap f EXIT` (run when the script ends), source and `.`, eval;
//   - test, [ ] and [[ ]] with the usual string, number and file tests;
//   - $( ) and backticks over the commands such scripts substitute: dirname, basename, readlink -f, realpath, pwd, echo, printf, uname, id, whoami;
//   - `exec prog args`, or running a program that lies inside the AppImage, is where the script hands over: that program is the result. A program that
//     is itself a script is interpreted in turn.
// Any other command fails as a missing one would (exit status 127) and the script carries on, as a shell would. `set -e` is not honoured.
[[nodiscard]] AppRunResult evaluate_apprun(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView world_root, StringView entry,
                                           ArrayView<const StringView> arguments, ArrayView<const StringView> environment, AppRunLaunch* launch);

}  // namespace desktop
