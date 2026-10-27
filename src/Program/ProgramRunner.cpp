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
#include "Program/ProgramRunner.hpp"
#include "Program/ProgramHost.hpp"
#include "Emulation/GuestCpu.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Process/HostArch.hpp"
#include "Process/ProgramEnvironment.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/UserProgram.hpp"
#include "Program/EmulatedTask.hpp"
#include "Utility/ByteBuffer.hpp"

#if defined(__x86_64__)
#include "Kernel/UserMode.hpp"
#endif

namespace desktop {

namespace {

constexpr uint64_t kMaxProgramBytes = 128ull << 20;
constexpr uint32_t kMaxArguments = 16;

bool fail(String* message, StringView text) {
  message->assign(text);
  return false;
}

// The program file, whole.
bool read_program(filesystem::IFileSystem& files, StringView path, ByteBuffer* out, String* message) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return fail(message, "cannot open the program");
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxProgramBytes) return fail(message, "the program file is empty or too big");
  if (!out->put_zeros(size)) return fail(message, "not enough memory to load the program");
  ByteView bytes = out->writable_view();
  for (uint64_t done = 0; done < size;) {
    uint64_t got = 0;
    if (file->read(done, bytes.subview(done, size - done < (1u << 20) ? size - done : (1u << 20)), &got) != filesystem::Status::Ok || got == 0) return fail(message, "cannot read the program");
    done += got;
  }
  return true;
}

StringView load_failure(process::LoadStatus status) {
  switch (status) {
    case process::LoadStatus::NotExecutable: return "this is not a program";
    case process::LoadStatus::Malformed: return "the program file is damaged, or its layout is not one this OS can load";
    case process::LoadStatus::Unsupported: return "this kind of program cannot run here (only 64-bit programs for Linux, and x86-64 programs for Windows)";
    case process::LoadStatus::NeedsLibraries: return "the program needs shared libraries or DLLs, which this OS does not have yet (it must be statically linked, or use the NT interface directly)";
    case process::LoadStatus::OutOfMemory: return "not enough memory to start the program";
    default: return "the program could not be loaded";
  }
}

StringView fault_name(uint64_t vector) {
  switch (vector) {
    case 0: return "divide error";
    case 6: return "invalid instruction";
    case 13: return "protection fault";
    case 14: return "invalid memory access";
    case 16: return "floating-point error";
    default: return "processor exception";
  }
}

// Passes on what the program printed so far (carriage returns dropped: the terminal starts a line at "\n").
void pass_on_output(ProgramHost& host, const ProgramRun& run, allocators::IAllocator& allocator) {
  String text(allocator);
  if (!host.take_output(&text) || !run.output) return;
  String clean(allocator);
  for (char c : text.view()) {
    if (c != '\r') clean.push_back(c);
  }
  run.output(run.context, clean.view());
}

}  // namespace

namespace {

// What the CPU of the program means for running it: directly in user mode (only a program for this machine's own CPU), or on the in-kernel emulator.
enum class Plan : uint8_t { Native, Emulated, Impossible };

Plan plan_for(const ProgramRun& run, ConstByteView image, String* message) {
  const exe::Architecture arch = exe::identify_architecture(image.at_most(exe::kIdentifyBytes));
  const process::UserArch guest = process::user_arch_for(arch);
  if (guest == process::UserArch::None) {
    if (arch == exe::Architecture::Unknown) return Plan::Native;      // (the loader names whatever is wrong with the file)
    message->assign("this program is for ");
    message->append(exe::architecture_name(arch));
    message->append(", which this OS cannot run");
    return Plan::Impossible;
  }
  if (run.kind == exe::ExecutableKind::Pe && guest != process::UserArch::X86_64) {
    message->assign("this Windows program is for ");
    message->append(exe::architecture_name(arch));
    message->append("; only x86-64 Windows programs are supported");
    return Plan::Impossible;
  }
  const bool native_possible =
#if defined(__x86_64__)
      guest == process::native_arch();
#else
      false;      // (the other machines have no ring-3 entry yet: every program is emulated)
#endif
  if (native_possible && !run.force_emulation) return Plan::Native;
  if (!emulation::can_emulate(guest)) {
    message->assign("this program is for ");
    message->append(exe::architecture_name(arch));
    message->append(", which this OS can neither run directly nor emulate on this machine");
    return Plan::Impossible;
  }
  return Plan::Emulated;
}

}  // namespace

RunResult run_program(const ProgramRun& run, uint32_t* exit_code, String* message) {
  *exit_code = 0;
  allocators::IAllocator& allocator = *run.allocator;
  static kernel::Mutex init_mutex;
  static uint64_t next_id = 100;
  const process::SyscallAbi abi = process::abi_for_executable(run.kind);
  if (abi == process::SyscallAbi::None) {
    fail(message, "this is not a program");
    return RunResult::CouldNotRun;
  }
  process::prepare_program_folders(*run.files);      // (best effort: without a writable root volume the program just has no files)

  ByteBuffer image(allocator);
  if (!read_program(*run.files, run.path, &image, message)) return RunResult::CouldNotRun;
  const Plan plan = plan_for(run, image.view(), message);
  if (plan == Plan::Impossible) return RunResult::CouldNotRun;
#if defined(__x86_64__)
  if (plan == Plan::Native) {
    kernel::ScopedMutex lock(init_mutex);
    if (!kernel::usermode::ready() && !kernel::usermode::init(allocator)) {
      fail(message, "user mode is not available on this machine");
      return RunResult::CouldNotRun;
    }
  }
#endif

  String name(allocator);
  StringView argv[kMaxArguments + 1];
  uint32_t argc = 0;
  if (!process::program_name(abi, run.path, &name)) {
    fail(message, "not enough memory");
    return RunResult::CouldNotRun;
  }
  argv[argc++] = name.view();
  for (const StringView& argument : run.arguments) {
    if (argc > kMaxArguments) {
      fail(message, "too many arguments");
      return RunResult::CouldNotRun;
    }
    argv[argc++] = argument;
  }

  ProgramHost own_host(allocator);
  ProgramHost& host = run.host ? *run.host : own_host;
  if (!run.host) host.close_input();
  uint8_t random[16];
  if (!host.random_bytes(ByteView(random, sizeof(random)))) {      // (no hardware generator: the stack protector's canary is only as good as this)
    uint64_t t = host.monotonic_ns() ^ reinterpret_cast<uintptr_t>(&random);
    for (uint8_t& b : random) {
      t = t * 6364136223846793005ull + 1442695040888963407ull;
      b = static_cast<uint8_t>(t >> 56);
    }
  }
  uint64_t id;
  {
    kernel::ScopedMutex lock(init_mutex);
    id = next_id++;
  }
  process::UserProgram program(allocator, id, host);
#if defined(__x86_64__)
  if (plan == Plan::Native) program.address_space().set_reserved(kernel::usermode::reserved_ranges());      // (pages the CPU's own page tables need; an emulated program has none)
#endif
  program.set_file_system(run.files);
  process::FileLibraryProvider libraries(allocator, run.files);      // every DLL, ntdll.dll and kernel32.dll included, comes from C:\Windows\System32
  if (abi == process::SyscallAbi::Windows) program.set_library_provider(&libraries);
  String directory(allocator);
  if (process::world_directory(abi, run.working_directory, &directory)) program.set_working_directory(directory.view());
  uint32_t processors = 0;
  for (uint64_t mask = kernel::online_core_mask(); mask; mask &= mask - 1) ++processors;
  program.set_processor_count(processors);
  // The environment the shell gives a program, with the real number of processors.
  ArrayView<const StringView> standard = process::default_environment(abi);
  StringView environment[32];
  uint32_t environment_count = 0;
  String processors_entry(allocator);
  for (const StringView& entry : standard) {
    if (environment_count >= 31) break;
    if (abi == process::SyscallAbi::Windows && entry.size() > 21 && StringView(entry.data(), 21).equals("NUMBER_OF_PROCESSORS=")) {
      processors_entry.assign("NUMBER_OF_PROCESSORS=");
      char digits[12];
      uint32_t n = 0, value = processors ? processors : 1;
      do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
      while (n) processors_entry.push_back(digits[--n]);
      environment[environment_count++] = processors_entry.view();
    } else {
      environment[environment_count++] = entry;
    }
  }
  const process::LoadStatus status = program.load(image.view(), ArrayView<const StringView>(argv, argc), ArrayView<const StringView>(environment, environment_count), ConstByteView(random, sizeof(random)));
  if (status != process::LoadStatus::Ok) {
    fail(message, load_failure(status));
    if (!program.load_detail().empty()) {
      message->append(" (");
      message->append(program.load_detail());
      message->append(")");
    }
    return RunResult::CouldNotRun;
  }

  // How it ended, whichever way it ran.
  uint32_t code = 0;
  bool killed = false, faulted = false;
  uint64_t fault_vector = 0, fault_address = 0, fault_rip = 0;
  if (plan == Plan::Emulated) {
    if (!ensure_system_objects(allocator)) {
      fail(message, "not enough memory to start the program");
      return RunResult::CouldNotRun;
    }
    UniquePointer<emulation::IGuestCpu> cpu = emulation::create_guest_cpu(program.start().arch, allocator);
    if (!cpu) {
      fail(message, "no emulator for this program's CPU");
      return RunResult::CouldNotRun;
    }
    EmulatedTask task(program, static_cast<UniquePointer<emulation::IGuestCpu>&&>(cpu), allocator);
    if (!task.start("emulated")) {
      fail(message, "could not start the program (out of memory)");
      return RunResult::CouldNotRun;
    }
    host.set_interrupt_callback([](void* t) { static_cast<EmulatedTask*>(t)->request_stop(); }, &task);
    while (!task.wait(20000)) {
      pass_on_output(host, run, allocator);
      if (host.interrupted()) task.request_stop();
    }
    host.set_interrupt_callback(nullptr, nullptr);
    const emulation::EmulatedOutcome& outcome = task.outcome();
    code = outcome.exit_code;
    killed = outcome.killed;
    faulted = outcome.faulted;
    fault_vector = outcome.fault.vector;
    fault_address = outcome.fault.address;
    fault_rip = outcome.fault.pc;
  } else {
#if defined(__x86_64__)
    UniquePointer<kernel::usermode::Task> task = kernel::usermode::spawn(program, allocator, "program");
    if (!task) {
      fail(message, "could not start the program (out of memory)");
      return RunResult::CouldNotRun;
    }
    task->set_name(name.view());      // (what the computer manager lists)
    bool kill_sent = false;
    host.set_interrupt_callback([](void* t) { kernel::usermode::request_kill(*static_cast<kernel::usermode::Task*>(t)); }, task.get());
    while (!kernel::usermode::wait(*task, &code, 20000)) {
      pass_on_output(host, run, allocator);
      if (host.interrupted() && !kill_sent) {
        kernel::usermode::request_kill(*task);
        kill_sent = true;
      }
    }
    host.set_interrupt_callback(nullptr, nullptr);
    killed = task->killed;
    faulted = task->faulted;
    fault_vector = task->fault_vector;
    fault_address = task->fault_address;
    fault_rip = task->fault_rip;
#endif
  }
  pass_on_output(host, run, allocator);
  *exit_code = code;

  String unhandled(allocator);
  if (host.take_unhandled(&unhandled)) {
    message->assign("it asked for something this OS does not have: ");
    message->append(unhandled.view());
  }
  if (host.dropped() != 0) {
    if (!message->empty()) message->append("; ");
    message->append("some of its output was dropped");
  }
  if (killed) return RunResult::Interrupted;
  if (faulted) {
    String text(allocator);
    String import_name(allocator);
    if (fault_vector == 6 && program.describe_missing_import(fault_rip, &import_name)) {
      text.assign("the program called ");
      text.append(import_name.view());
      text.append(", which this OS does not provide");
      message->assign(text.view());
      return RunResult::Faulted;
    }
    text.assign(plan == Plan::Emulated ? "stopped by the emulated processor: " : "stopped by the processor: ");
    text.append(fault_name(fault_vector));
    if (fault_vector == 14) {
      text.append(" at address 0x");
      int shift = 60;
      while (shift > 0 && ((fault_address >> shift) & 15) == 0) shift -= 4;      // no leading zeros
      for (; shift >= 0; shift -= 4) text.push_back("0123456789abcdef"[(fault_address >> shift) & 15]);
    }
    if (!message->empty()) {
      text.append("; ");
      text.append(message->view());
    }
    message->assign(text.view());
    return RunResult::Faulted;
  }
  return RunResult::Exited;
}

namespace {

// The terminal's way into the program's console.
class HostControl final : public IProgramControl {
 public:
  explicit HostControl(ProgramHost& host) : host_(host) {}
  void send_input(StringView text) override { host_.push_input(text); }
  void end_input() override { host_.close_input(); }
  void interrupt() override { host_.interrupt(); }

 private:
  ProgramHost& host_;
};

void forward_output(void* sink, StringView text) { static_cast<IProgramSink*>(sink)->program_output(text); }

}  // namespace

void KernelProgramLauncher::run(const ProgramLaunch& launch, IProgramSink& sink, ProgramOutcome* outcome) {
  ProgramHost host(allocator_);
  HostControl control(host);
  ProgramRun run;
  run.allocator = &allocator_;
  run.files = launch.files;
  run.path = launch.path;
  run.working_directory = launch.working_directory;
  run.arguments = launch.arguments;
  run.kind = launch.kind;
  run.output = &forward_output;
  run.context = &sink;
  run.host = &host;
  uint32_t code = 0;
  String message(allocator_);
  sink.program_started(&control);
  const RunResult result = run_program(run, &code, &message);
  sink.program_ended();
  outcome->exit_code = code;
  outcome->message.assign(message.view().first(message.size() < 255 ? message.size() : 255));
  switch (result) {
    case RunResult::Exited: outcome->kind = ProgramOutcome::Kind::Exited; break;
    case RunResult::Faulted: outcome->kind = ProgramOutcome::Kind::Faulted; break;
    case RunResult::Interrupted: outcome->kind = ProgramOutcome::Kind::Interrupted; break;
    case RunResult::CouldNotRun: outcome->kind = ProgramOutcome::Kind::CouldNotRun; break;
  }
}

}  // namespace desktop
