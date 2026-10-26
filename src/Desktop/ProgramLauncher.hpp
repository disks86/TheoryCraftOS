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
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/StringView.hpp"

namespace desktop {

// How the terminal runs the programs the shell finds, without knowing how programs run (that is the kernel's business, in Program/). The terminal gives the
// launcher a sink for the program's output; while the program runs, the launcher gives the terminal a control to feed the program's input and to stop it.

// Handed to the terminal while a program runs. Any thread may call these.
class IProgramControl {
 public:
  virtual void send_input(StringView text) = 0;      // a line of input (with its "\n")
  virtual void end_input() = 0;                      // Ctrl+D: the program reads end of file
  virtual void interrupt() = 0;                      // Ctrl+C: the program is stopped

 protected:
  IProgramControl() = default;
  ~IProgramControl() = default;
};

// Implemented by the terminal. All calls come from the thread that called run().
class IProgramSink {
 public:
  virtual void program_output(StringView text) = 0;
  // The program is about to start / has ended; `control` is valid in between.
  virtual void program_started(IProgramControl* control) = 0;
  virtual void program_ended() = 0;

 protected:
  IProgramSink() = default;
  ~IProgramSink() = default;
};

struct ProgramLaunch {
  filesystem::IFileSystem* files = nullptr;      // the OS file system the shell works on
  StringView path;                               // absolute path of the executable in `files`
  StringView working_directory;                  // the shell's current folder
  ArrayView<const StringView> arguments;         // what followed the command
  exe::ExecutableKind kind = exe::ExecutableKind::None;
};

struct ProgramOutcome {
  enum class Kind : uint8_t { Exited, Faulted, Interrupted, CouldNotRun };
  Kind kind = Kind::CouldNotRun;
  uint32_t exit_code = 0;
  FixedString<256> message;      // why it could not run, how it was stopped, or what it lacked; empty if nothing to say
};

class IProgramLauncher {
 public:
  // Runs the program to its end on the calling thread, blocking. The sink receives the output as it comes.
  virtual void run(const ProgramLaunch& launch, IProgramSink& sink, ProgramOutcome* outcome) = 0;

 protected:
  IProgramLauncher() = default;
  ~IProgramLauncher() = default;
};

}  // namespace desktop
