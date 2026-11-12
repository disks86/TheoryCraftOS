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

#include "DBus/ProgramActivator.hpp"
#include "DBus/Bus.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Kernel/Thread.hpp"
#include "Program/ProgramRunner.hpp"
#include "console.hpp"

namespace dbus {

// One program: what it was started with, and (under `lock`) how it ended. The thread keeps it alive through `self` until it is done.
struct ProgramActivator::Run {
  explicit Run(allocators::IAllocator& a) : allocator(a), path(a), arguments(a), starter_address(a), why(a) {}
  allocators::IAllocator& allocator;
  filesystem::IFileSystem* files = nullptr;
  uint64_t handle = 0;
  String path;                   // in the OS's file system
  List<String> arguments;        // after argv[0]
  exe::ExecutableKind kind = exe::ExecutableKind::None;
  bool system_bus = false;
  String starter_address;        // DBUS_STARTER_ADDRESS=...
  kernel::Mutex lock;
  State state = State::Running;
  uint32_t exit_code = 0;
  String why;
  SharedPointer<Run> self;       // (the thread's use)
};

namespace {

void program_output(void*, StringView text) {
  char line[256];
  text.first(text.size() < 255 ? text.size() : 255).copy_to(ArrayView<char>(line, sizeof line));
  console::puts(line);
}

void run_main(void* argument) {
  ProgramActivator::Run& r = *static_cast<ProgramActivator::Run*>(argument);
  StringView arguments[kMaxExecWords];
  uint64_t n = 0;
  for (const String& a : r.arguments.view()) arguments[n++] = a.view();
  desktop::ProgramRun run;
  run.allocator = &r.allocator;
  run.files = r.files;
  run.path = r.path.view();
  run.working_directory = filesystem::layout::kLinux;
  run.arguments = ArrayView<const StringView>(arguments, n);
  run.kind = r.kind;
  run.output = &program_output;
  // the bus that started it, as dbus-daemon tells its services (libdbus' DBUS_BUS_STARTER, GDBus' G_BUS_TYPE_STARTER)
  const StringView starter[] = {r.starter_address.view(), r.system_bus ? StringView("DBUS_STARTER_BUS_TYPE=system") : StringView("DBUS_STARTER_BUS_TYPE=session")};
  run.extra_environment = ArrayView<const StringView>(starter, 2);
  uint32_t code = 0;
  String message(r.allocator);
  const desktop::RunResult result = desktop::run_program(run, &code, &message);
  SharedPointer<ProgramActivator::Run> keep;
  {
    kernel::ScopedMutex hold(r.lock);
    r.exit_code = code;
    r.state = result == desktop::RunResult::CouldNotRun ? IServiceLauncher::State::CouldNotRun : IServiceLauncher::State::Exited;
    r.why.assign(message.view());
    keep = util::move(r.self);
  }
}      // (`keep` lets go of the run last)

}  // namespace

ProgramActivator::ProgramActivator(filesystem::IFileSystem& files, allocators::IAllocator& allocator) : files_(files), allocator_(allocator), runs_(allocator) {}

uint64_t ProgramActivator::start(BusKind bus, StringView service_name, ArrayView<const StringView> argv, String* why) {
  (void)service_name;
  if (argv.empty() || !argv[0].starts_with('/')) {
    why->assign("the program is not an absolute path");
    return 0;
  }
  SharedPointer<Run> r = SharedPointer<Run>::make(allocator_, allocator_);
  if (!r || !r->path.append(filesystem::layout::kLinux) || !r->path.append(argv[0])) return 0;
  for (uint64_t i = 1; i < argv.size(); ++i) {
    String a(allocator_);
    if (!a.assign(argv[i]) || !r->arguments.push_back(util::move(a))) return 0;
  }
  r->kind = exe::identify_executable_file(files_, r->path.view());
  if (r->kind == exe::ExecutableKind::None) {
    why->assign("no such program: ");
    why->append(argv[0]);
    return 0;
  }
  r->files = &files_;
  r->system_bus = bus == BusKind::System;
  if (!r->starter_address.append("DBUS_STARTER_ADDRESS=unix:path=")) return 0;
  if (r->system_bus) {
    if (!r->starter_address.append(filesystem::layout::kLinuxSystemBusSocket)) return 0;
  } else if (!r->starter_address.append(filesystem::layout::kLinuxRuntimeFolder) || !r->starter_address.push_back('/') ||
             !r->starter_address.append(filesystem::layout::kSessionBusSocket)) {
    return 0;
  }
  {
    kernel::ScopedMutex hold(lock_);
    r->handle = next_handle_++;
    if (!runs_.push_back(r)) return 0;
  }
  r->self = r;
  if (!kernel::spawn(&run_main, r.get(), "dbus-service")) {
    r->self = SharedPointer<Run>();
    forget(r->handle);
    why->assign("no thread for it");
    return 0;
  }
  return r->handle;
}

SharedPointer<ProgramActivator::Run> ProgramActivator::find(uint64_t handle) {
  kernel::ScopedMutex hold(lock_);
  for (const SharedPointer<Run>& r : runs_.view())
    if (r->handle == handle) return r;
  return SharedPointer<Run>();
}

IServiceLauncher::State ProgramActivator::poll(uint64_t handle, uint32_t* exit_code, String* why) {
  SharedPointer<Run> r = find(handle);
  if (!r) return State::CouldNotRun;
  kernel::ScopedMutex hold(r->lock);
  *exit_code = r->exit_code;
  why->assign(r->why.view());
  return r->state;
}

void ProgramActivator::forget(uint64_t handle) {
  kernel::ScopedMutex hold(lock_);
  for (uint64_t i = 0; i < runs_.count(); ++i)
    if (runs_[i]->handle == handle) {
      runs_.remove_at(i);
      return;
    }
}

}  // namespace dbus
