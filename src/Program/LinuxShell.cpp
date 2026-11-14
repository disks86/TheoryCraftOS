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

#include "Process/LinuxExec.hpp"
#include "Program/AppRunScript.hpp"
#include "Utility/Move.hpp"

// The Linux tree's /bin/sh is this OS's AppRun shell (kept apart from AppRunScript.cpp so that what uses the shell alone needs nothing of the Linux layer).
namespace desktop {

namespace {

int run_builtin_shell(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView world_root, StringView script,
                      ArrayView<const StringView> arguments, ArrayView<const StringView> environment, String* program, List<String>* program_arguments,
                      List<String>* program_environment) {
  AppRunLaunch launch(allocator);
  const AppRunResult result = evaluate_apprun(allocator, files, world_root, script, arguments, environment, &launch);
  if (result == AppRunResult::OutOfMemory) return -1;
  if (result != AppRunResult::Launch) return 0;
  if (!program->assign(launch.program.view())) return -1;
  for (String& argument : launch.arguments.view())
    if (!program_arguments->push_back(util::move(argument))) return -1;
  for (String& variable : launch.environment.view())
    if (!program_environment->push_back(util::move(variable))) return -1;
  return 1;
}

}  // namespace

void install_linux_builtin_shell() { process::linuxabi::install_builtin_shell(run_builtin_shell); }

}  // namespace desktop
