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

#include "Desktop/AppMan/KernelSetupHost.hpp"
#include "Kernel/Thread.hpp"
#include "Process/WindowsRegistry.hpp"
#include "Program/ProgramHost.hpp"
#include "Program/ProgramRunner.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

// Watches the cancel flag while the program runs, and stops the program when it is set.
struct Watch {
  ProgramHost* host;
  const bool* cancel;
  bool done = false;
  static void entry(void* argument) {
    Watch& w = *static_cast<Watch*>(argument);
    while (!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE)) {
      if (__atomic_load_n(w.cancel, __ATOMIC_RELAXED)) {
        w.host->interrupt();
        return;
      }
      kernel::sleep_ms(200);
    }
  }
};

}  // namespace

bool KernelSetupHost::run(StringView program, ArrayView<const StringView> arguments, const bool* cancel, uint32_t* exit_code, String* message) {
  *exit_code = 0;
  ProgramHost host(allocator_);
  host.close_input();      // (a setup program that reads its console reads the end of it)
  ProgramRun run;
  run.allocator = &allocator_;
  run.files = &files_;
  run.path = program;
  const uint64_t slash = program.rfind('/');
  run.working_directory = slash == StringView::npos ? StringView("/Windows") : program.first(slash);
  run.arguments = arguments;
  run.kind = exe::ExecutableKind::Pe;
  run.host = &host;
  run.wait_for_descendants = true;      // (an uninstaller hands its work to a copy of itself and ends: the work is done when the copy is)
  Watch watch{&host, cancel};
  kernel::Thread* watcher = cancel ? kernel::spawn(&Watch::entry, &watch, "setup-cancel") : nullptr;
  const RunResult result = run_program(run, exit_code, message);
  __atomic_store_n(&watch.done, true, __ATOMIC_RELEASE);
  if (watcher) kernel::join(watcher);
  if (result == RunResult::Exited) return true;
  if (result == RunResult::Interrupted) message->assign("it was stopped");
  else if (message->empty()) message->assign(result == RunResult::Faulted ? "it stopped with a fault" : "it could not be started");
  return false;
}

bool KernelSetupHost::uninstall_entries(List<UniquePointer<UninstallEntry>>* entries) {
  entries->clear();
  List<UniquePointer<process::windowsabi::RegistryUninstallEntry>> found(allocator_);
  if (!process::windowsabi::list_uninstall_entries(allocator_, &found)) return false;
  for (const UniquePointer<process::windowsabi::RegistryUninstallEntry>& from : found.view()) {
    UniquePointer<UninstallEntry> entry = UniquePointer<UninstallEntry>::make(allocator_, allocator_);
    if (!entry || !entry->key.assign(from->key.view()) || !entry->name.assign(from->display_name.view()) || !entry->version.assign(from->display_version.view()) ||
        !entry->command.assign(from->uninstall.view()) || !entry->quiet_command.assign(from->quiet_uninstall.view()) || !entries->push_back(util::move(entry)))
      return false;
  }
  return true;
}

}  // namespace desktop::appman
