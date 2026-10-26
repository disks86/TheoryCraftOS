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
#include "Desktop/ProgramLauncher.hpp"
#include "Program/ProgramHost.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace desktop {

// Everything needed to run a program the shell was told to start.
struct ProgramRun {
  allocators::IAllocator* allocator = nullptr;
  filesystem::IFileSystem* files = nullptr;        // the OS file system, in which the program's world is the folder /Linux or /Windows
  StringView path;                                 // absolute path of the executable in `files`
  StringView working_directory;                    // the shell's current folder
  ArrayView<const StringView> arguments;           // what followed the command
  exe::ExecutableKind kind = exe::ExecutableKind::None;
  void (*output)(void* context, StringView text) = nullptr;     // called on the calling thread with what the program printed, as it comes
  void* context = nullptr;
  // The program's console, if the caller wants to feed its input and send Ctrl+C (push_input(), close_input(), interrupt()) from another thread. Without
  // one the program gets an empty input.
  ProgramHost* host = nullptr;
};

enum class RunResult : uint8_t {
  Exited,         // the program ended itself; *exit_code is its code
  Faulted,        // the processor stopped it (a bad memory access, ...); *message says how, *exit_code is 128 + the vector
  Interrupted,    // Ctrl+C ended it; *exit_code is 130
  CouldNotRun,    // it never started; *message says why
};

// Runs the program in user mode and waits for it, passing its output on while it runs. The calling thread blocks; the program has the "/Linux" or "/Windows"
// folder of the OS file system as its whole file system (the folders are made if missing). Only x86-64 can run programs so far.
RunResult run_program(const ProgramRun& run, uint32_t* exit_code, String* message);

// The launcher the terminals use: runs programs with run_program() on the kernel's user mode, feeding them the terminal's input.
class KernelProgramLauncher final : public IProgramLauncher {
 public:
  explicit KernelProgramLauncher(allocators::IAllocator& allocator) : allocator_(allocator) {}
  void run(const ProgramLaunch& launch, IProgramSink& sink, ProgramOutcome* outcome) override;

 private:
  allocators::IAllocator& allocator_;
};

}  // namespace desktop
