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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <vector>
#include "Emulation/GuestCpu.hpp"
#include "Process/Process.hpp"
#include "Process/ModuleLoader.hpp"
#include "Process/UserProgram.hpp"
#include "Process/Objects.hpp"
#include "Process/ProgramEnvironment.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#ifdef TCOS_EMU_X11
#include <set>
#include "Process/UnixSockets.hpp"
#include "X11/X11SocketServer.hpp"
#endif
#ifdef TCOS_EMU_WAYLAND
#include <map>
#include <set>
#include "Kernel/Clipboard.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Process/UnixSockets.hpp"
#include "Wayland/WaylandSocketServer.hpp"
#endif
// With both -DTCOS_EMU_WAYLAND and -DTCOS_EMU_DXG the compositor also takes GPU buffers (zwp_linux_dmabuf_v1: dmabufs of /dev/dxg), and the stand-in desktop
// draws every other round with a 2D renderer on the adapter's device (the image sampled in place), the others on the CPU. tests/wayland_dmabuf_emulated.sh.
// With -DTCOS_EMU_GL the program also has the kernel's OpenGL (src/OpenGl over the CPU renderer), reached through /dev/dxg's TCOS_DXG_IOCTL_GL as in the kernel:
// what src/Linux/libegl runs on. tests/linux_egl_wayland_emulated.sh.
#ifdef TCOS_EMU_GL
#include "OpenGl/GlService.hpp"
#include "OpenGl/WindowsGl.hpp"
#endif
#if defined(TCOS_EMU_WAYLAND) && defined(TCOS_EMU_DXG)
#define TCOS_EMU_DMABUF 1
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics3D/LockedDevice.hpp"
#include "Wayland/Dxg/WaylandDxgImage.hpp"
#endif
#ifdef TCOS_EMU_AUDIO
#include <cmath>
#include <dirent.h>
#include "FileSystem/SystemLayout.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/NativeServer.hpp"
#include "PipeWire/PulseServer.hpp"
#include "PipeWire/SocketServer.hpp"
#include "PipeWire/WindowsAudioService.hpp"
#include "Process/UnixSockets.hpp"
#endif
#ifdef TCOS_EMU_DBUS
#include "DBus/Bus.hpp"
#include "DBus/Notifications.hpp"
#include "DBus/SocketServer.hpp"
#include "Process/UnixSockets.hpp"
#include <functional>
#include <memory>
// Activated services run as more emulated programs: `run` (set in main, where the program's pieces are) starts one on a thread of its own.
struct EmuLauncher final : dbus::IServiceLauncher {
  struct Child {
    std::atomic<int> state{0};      // 0 running, 1 exited, 2 could not run
    std::atomic<uint32_t> code{0};
    std::atomic<bool>* stop = nullptr;      // (its run control's)
  };
  std::function<bool(const std::vector<std::string>&, const std::vector<std::string>&, Child&)> run;
  std::vector<std::unique_ptr<Child>> children;
  uint64_t start(const dbus::ServiceStart& service, String* why) override {
    const StringView name = service.name;
    std::vector<std::string> args, env;
    for (const StringView& a : service.argv) args.push_back(std::string(a.data(), a.size()));
    for (const StringView& e : service.environment) env.push_back(std::string(e.data(), e.size()));
    children.push_back(std::make_unique<Child>());
    if (!run || !run(args, env, *children.back())) {
      children.pop_back();
      why->assign("cannot load the program");
      return 0;
    }
    printf("dbus activate: %.*s -> %s\n", int(name.size()), name.data(), args[0].c_str());
    return children.size();
  }
  State poll(uint64_t handle, uint32_t* code, String*) override {
    const Child& c = *children[handle - 1];
    *code = c.code;
    return c.state == 0 ? State::Running : c.state == 1 ? State::Exited : State::CouldNotRun;
  }
  void stop(uint64_t handle) override {
    if (children[handle - 1]->stop) *children[handle - 1]->stop = true;
  }
  void forget(uint64_t) override {}
};
// (the D-Bus transport's clock; the kernel's comes from the timestamp counter)
namespace hardware {
uint64_t uptime_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace hardware
#endif
using namespace emulation;

// With -DTCOS_EMU_AUDIO the kernel's PipeWire is there: the graph over a mixer whose device drains in real time and keeps what it got, its sockets at
// /run/user/0/pipewire-0 and /run/user/0/pulse/native, NtTcosAudio for winmm.dll, and a native stream (300 Hz) that plays meanwhile. A second program
// may be given (argv[3]) to run at the same time. At the end the mixed output is checked for each program's tone (tests/pipewire_emulated.sh).
#ifdef TCOS_EMU_AUDIO
namespace {
struct RealtimeDevice final : hardware::IAudioDevice {
  std::mutex m;
  std::vector<int16_t> recorded;
  double queued = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  void advance() {
    const auto t = std::chrono::steady_clock::now();
    queued -= std::chrono::duration<double>(t - last).count() * 48000.0;
    if (queued < 0) queued = 0;
    last = t;
  }
  const char* name() const override { return "Emulated speakers"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    std::lock_guard<std::mutex> g(m);
    advance();
    uint32_t frames = uint32_t(pcm.size() / 4), room = 4800 - uint32_t(queued);
    if (frames > room) frames = room;
    for (uint32_t i = 0; i < frames * 2; ++i) recorded.push_back(int16_t(pcm[2 * i] | pcm[2 * i + 1] << 8));
    queued += frames;
    return frames * 4;
  }
  uint32_t queued_bytes() override { std::lock_guard<std::mutex> g(m); advance(); return uint32_t(queued) * 4; }
  uint32_t free_bytes() override { std::lock_guard<std::mutex> g(m); advance(); return (4800 - uint32_t(queued)) * 4; }
  uint8_t v = 100; bool mute = false;
  void set_volume(uint8_t x) override { v = x; }
  uint8_t volume() const override { return v; }
  void set_muted(bool x) override { mute = x; }
  bool muted() const override { return mute; }
  hardware::AudioStatistics statistics() override { return {}; }
};

// A microphone that hears, in real time, 880 Hz on the left at 8000 and 1320 Hz on the right at 4000 (what the recording tests check for).
struct RealtimeMicrophone final : hardware::IAudioCapture {
  std::mutex m;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  double owed = 0;        // frames heard and not yet read
  uint64_t phase = 0;     // frames made so far
  uint8_t level = hardware::kAudioMaxVolume;
  bool mute = false;
  void advance() {
    const auto t = std::chrono::steady_clock::now();
    owed += std::chrono::duration<double>(t - last).count() * 48000.0;
    if (owed > 16384) owed = 16384;      // (the hardware's ring: older audio is gone)
    last = t;
  }
  const char* name() const override { return "Emulated microphone"; }
  uint32_t read(ByteView out) override {
    std::lock_guard<std::mutex> g(m);
    advance();
    uint32_t frames = uint32_t(out.size() / 4);
    if (frames > uint32_t(owed)) frames = uint32_t(owed);
    for (uint32_t i = 0; i < frames; ++i, ++phase) {
      const double t = double(phase) / 48000.0;
      const int16_t l = mute ? 0 : int16_t(8000.0 * sin(2 * M_PI * 880.0 * t) * level / hardware::kAudioMaxVolume);
      const int16_t r = mute ? 0 : int16_t(4000.0 * sin(2 * M_PI * 1320.0 * t) * level / hardware::kAudioMaxVolume);
      out[4 * i] = uint8_t(l); out[4 * i + 1] = uint8_t(uint16_t(l) >> 8);
      out[4 * i + 2] = uint8_t(r); out[4 * i + 3] = uint8_t(uint16_t(r) >> 8);
    }
    owed -= frames;
    return frames * 4;
  }
  uint32_t available_bytes() override { std::lock_guard<std::mutex> g(m); advance(); return uint32_t(owed) * 4; }
  void flush() override { std::lock_guard<std::mutex> g(m); advance(); phase += uint64_t(owed); owed = 0; }
  void set_gain(uint8_t x) override { level = x; }
  uint8_t gain() const override { return level; }
  void set_muted(bool x) override { mute = x; }
  bool muted() const override { return mute; }
  hardware::AudioCaptureStatistics statistics() override { return {}; }
};

// The strength of `freq` in samples [from, from + n) of the left channel.
double tone(const std::vector<int16_t>& s, size_t from, size_t n, double freq) {
  double re = 0, im = 0;
  for (size_t k = 0; k < n; ++k) {
    const double v = s[2 * (from + k)], w = 2 * M_PI * freq * double(k) / 48000.0;
    re += v * cos(w);
    im -= v * sin(w);
  }
  return sqrt(re * re + im * im) / double(n);
}
}  // namespace
#endif

// With -DTCOS_EMU_DXG the program also has the kernel's Direct3D: win32k with the NtGdiDdDDI* services over a dxgk::Kernel that has one adapter, the software renderer (the
// way tests/Win32kDxgTest.cpp builds it). tests/emulated_dx.sh builds it that way for dxgi.dll and d3d12.dll.
#ifdef TCOS_EMU_DXG
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Process/Win32k/Win32k.hpp"
#include "Process/Win32k/Dxg/DxgInternal.hpp"
namespace {
class FakeScreen final : public hardware::IGraphicsProcessingUnit {
 public:
  FakeScreen(uint32_t w, uint32_t h) : w_(w), h_(h), pixels_(uint64_t{w} * h, 0) {}
  const char* name() const override { return "emulated screen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 2; }
  bool mode_info(uint32_t i, hardware::DisplayMode* out) const override { if (i > 1) return false; *out = {w_ * (i + 1), h_ * (i + 1)}; return true; }
  uint32_t current_mode() const override { return mode_; }
  bool set_mode(uint32_t i) override { if (i > 1) return false; mode_ = i; return true; }
  uint32_t width() const override { return w_; }
  uint32_t height() const override { return h_; }
  void put_pixel(uint32_t x, uint32_t y, uint32_t c) override { if (x < w_ && y < h_) pixels_[uint64_t{y} * w_ + x] = c; }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) override { for (uint32_t j = 0; j < h; ++j) for (uint32_t i = 0; i < w; ++i) put_pixel(x + i, y + j, c); }
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t c) override { for (uint32_t& p : pixels_) p = c; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
  void present() override {}

 private:
  uint32_t w_, h_, mode_ = 0;
  std::vector<uint32_t> pixels_;
};
}  // namespace
#endif

// Runs a Linux ELF or a Windows PE on the CPU emulator with real host threads, printing what the program writes to standard output and, last,
// "[exit=... faulted=... killed=... vector=... addr=... pc=...]".   Usage: EmulationRun <program> [<folder with the DLLs>]
// Used by aarch64_diff.sh and emulated_windows.sh. Gives up after 60 seconds.
struct A final : allocators::IAllocator { void* allocate(uint64_t s) override { return malloc(s); } void* allocate_aligned(uint64_t s, uint64_t al) override { if (al < 16) al = 16; return aligned_alloc(al, (s + al - 1) / al * al); } void deallocate(void* p) override { free(p); } };
static uint64_t now() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
struct H final : process::IProcessHost {
  std::mutex m;
  bool write_console(uint32_t, ConstByteView d) override { std::lock_guard<std::mutex> g(m); fwrite(d.data(), 1, d.size(), stdout); fflush(stdout); return true; }
  bool read_console(ByteView, uint64_t* g) override { *g = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull + now(); } uint64_t monotonic_ns() override { return now(); }
  void sleep_ns(uint64_t ns) override { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); } void yield() override { std::this_thread::yield(); }
  bool random_bytes(ByteView o) override { std::lock_guard<std::mutex> g(m); static std::mt19937_64 rng(12345); for (uint8_t& b : o) b = uint8_t(rng()); return true; }
};
struct Sync final : process::ISystemSync {
  std::mutex m; std::condition_variable_any cv;
  void lock() override { m.lock(); } void unlock() override { m.unlock(); }
  bool wait(uint64_t ns) override { if (ns == ~uint64_t{0}) { cv.wait(m); return true; } return cv.wait_for(m, std::chrono::nanoseconds(ns)) != std::cv_status::timeout; }
  void notify_all() override { cv.notify_all(); } uint64_t now_ns() override { return now(); }
};
struct Threads final : IThreadSpawner {
  struct Hd { std::thread t; };
  void* spawn(void (*fn)(void*), void* a, const char*) override { return new Hd{std::thread(fn, a)}; }
  void join(void* h) override { static_cast<Hd*>(h)->t.join(); delete static_cast<Hd*>(h); }
  uint64_t self() override { return std::hash<std::thread::id>()(std::this_thread::get_id()); }
};
struct Ctl final : IRunControl {
  std::atomic<bool> stop{false};
  bool trace = getenv("TCOS_EMU_STRACE") != nullptr;      // every system call on stderr, with its result
  bool stop_requested() override { return stop; }
  void yield() override { std::this_thread::yield(); }
  void on_syscall(uint64_t tid, const process::SyscallRequest& r, bool done, uint64_t result) override {
    if (!trace) return;
    if (!done) fprintf(stderr, "[%llx] > %llu(%llx, %llx, %llx, %llx)\n", (unsigned long long)tid, (unsigned long long)r.number, (unsigned long long)r.args[0], (unsigned long long)r.args[1], (unsigned long long)r.args[2], (unsigned long long)r.args[3]);
    else fprintf(stderr, "[%llx] < %llu = %lld\n", (unsigned long long)tid, (unsigned long long)r.number, (long long)result);
  }
};
static std::vector<uint8_t> slurp(const std::string& p, bool* ok) { FILE* f = fopen(p.c_str(), "rb"); *ok = f; std::vector<uint8_t> b; if (!f) return b; uint8_t buf[4096]; size_t n; while ((n = fread(buf, 1, 4096, f)) > 0) b.insert(b.end(), buf, buf + n); fclose(f); return b; }
struct Libs final : process::ILibraryProvider {
  std::string dir; std::map<std::string, std::vector<uint8_t>> cache;
  bool find(StringView name, ConstByteView* image) override {
    std::string n(name.data(), name.size());
    auto it = cache.find(n);
    if (it == cache.end()) { bool ok; auto b = slurp(dir + "/" + n, &ok); if (!ok) return false; it = cache.emplace(n, b).first; }
    *image = ConstByteView(it->second.data(), it->second.size());
    return true;
  }
};
int main(int argc, char** argv) {
  bool ok; auto b = slurp(argv[1], &ok); if (!ok) { printf("cannot read\n"); return 2; }
  A a; H h; Libs libs; if (argc > 2) libs.dir = argv[2];
  filesystem::RamFileSystem fs(a);
  process::prepare_program_folders(fs);
  // TCOS_EMU_LINUX_FILES: host files (colon separated) put at the same paths in the Linux tree, links resolved on the host: what a dynamically linked
  // program needs (its interpreter and libraries, tests/linux_dynamic_emulated.sh). An entry host=path puts the host file at that path of the Linux tree instead
  // (a library installed under its system name: tests/linux_vulkan_wayland_emulated.sh). A missing folder on the way is made.
  if (const char* list = getenv("TCOS_EMU_LINUX_FILES")) {
    std::string all = list;
    for (size_t at = 0; at <= all.size();) {
      size_t end = all.find(':', at);
      if (end == std::string::npos) end = all.size();
      std::string host = all.substr(at, end - at), inside;
      at = end + 1;
      if (const size_t eq = host.find('='); eq != std::string::npos) inside = host.substr(eq + 1), host = host.substr(0, eq);
      if (host.empty()) continue;
      if (inside.empty()) inside = host;
      bool read_ok = false;
      const std::vector<uint8_t> bytes = slurp(host, &read_ok);
      if (!read_ok) { printf("linux files: cannot read %s\n", host.c_str()); continue; }
      const std::string path = std::string(filesystem::layout::kLinux) + inside;
      for (size_t k = 1; k < path.size(); ++k)
        if (path[k] == '/') fs.create_directory(StringView(path.substr(0, k).c_str()));
      UniquePointer<filesystem::IFile> file;
      uint64_t written = 0;
      if (fs.open_file(StringView(path.c_str()), filesystem::OpenMode::CreateOrOpen, &file) != filesystem::Status::Ok ||
          file->write(0, ConstByteView(bytes.data(), bytes.size()), &written) != filesystem::Status::Ok)
        printf("linux files: cannot write %s\n", path.c_str());
    }
  }
#ifdef TCOS_EMU_AUDIO
  // the libraries in System32 as well, for what a program loads while it runs (ole32 loading dsound.dll or mmdevapi.dll for CoCreateInstance)
  if (argc > 2)
    if (DIR* d = opendir(argv[2])) {
      while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() < 5 || n.substr(n.size() - 4) != ".dll") continue;
        bool read_ok = false;
        const std::vector<uint8_t> image = slurp(std::string(argv[2]) + "/" + n, &read_ok);
        const std::string path = std::string(filesystem::layout::kWindowsSystem32) + "/" + n;
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        if (read_ok && fs.open_file(StringView(path.c_str()), filesystem::OpenMode::CreateOrOpen, &file) == filesystem::Status::Ok)
          file->write(0, ConstByteView(image.data(), image.size()), &written);
      }
      closedir(d);
    }
#endif
  Sync osync; process::ObjectManager objects(osync); process::set_system_objects(&objects);      // (before the program: its sections go back to the manager when it is destroyed)
  objects.set_allocator(&a);
  process::UserProgram p(a, 100, h);
  p.set_file_system(&fs);
  p.set_library_provider(&libs);
  p.set_processor_count(1);
  StringView av[] = {"C:\\test.exe"}; uint8_t rnd[16] = {};
  // TCOS_EMU_ENV: more environment variables for the program (space separated NAME=value, such as LD_DEBUG=libs).
  std::vector<StringView> environment;
  for (const StringView& e : process::default_environment(process::abi_for_executable(exe::identify_executable(ConstByteView(b.data(), b.size()).at_most(exe::kIdentifyBytes))))) environment.push_back(e);
  std::vector<std::string> extra_environment;
  if (const char* extra = getenv("TCOS_EMU_ENV")) {
    std::string all = extra;
    for (size_t at = 0; at < all.size();) {
      size_t end = all.find(' ', at);
      if (end == std::string::npos) end = all.size();
      if (end > at) extra_environment.push_back(all.substr(at, end - at));
      at = end + 1;
    }
  }
  for (const std::string& e : extra_environment) environment.push_back(StringView(e.data(), e.size()));
  // TCOS_EMU_ARGS: arguments after argv[0] (space separated).
  std::vector<std::string> extra_arguments;
  std::vector<StringView> arguments{av[0]};
  if (const char* extra = getenv("TCOS_EMU_ARGS")) {
    std::string all = extra;
    for (size_t at = 0; at < all.size();) {
      size_t end = all.find(' ', at);
      if (end == std::string::npos) end = all.size();
      if (end > at) extra_arguments.push_back(all.substr(at, end - at));
      at = end + 1;
    }
  }
  for (const std::string& e : extra_arguments) arguments.push_back(StringView(e.data(), e.size()));
  auto st = p.load(ConstByteView(b.data(), b.size()), ArrayView<const StringView>(arguments.data(), arguments.size()), ArrayView<const StringView>(environment.data(), environment.size()), ConstByteView(rnd, 16));
  if (st != process::LoadStatus::Ok) { printf("load failed %d (%.*s)\n", (int)st, (int)p.load_detail().size(), p.load_detail().data()); return 2; }
  auto cpu = create_guest_cpu(p.start().arch, a);
  Sync sync; Threads th; Ctl ctl;
#ifdef TCOS_EMU_DXG
  FakeScreen screen(640, 480);
  graphics::SoftwareDevice device(a, &screen);
#ifdef TCOS_EMU_DMABUF
  graphics::LockedDevice shared_device(device);      // (the program's thread and the desktop's both draw on it)
  graphics::IGraphicsDevice& adapter_device = shared_device;
#else
  graphics::IGraphicsDevice& adapter_device = device;
#endif
  dxgk::Kernel dxkernel(a);
  dxgk::AdapterConfig adapter_config;
  adapter_config.display = &screen;
  adapter_config.local_memory_budget = 256ull << 20;
  dxkernel.add_adapter(adapter_device, adapter_config);
  dxgk::set_system_kernel(&dxkernel);
  process::win32k::install();
  process::win32k::install_dxg();
#endif
#ifdef TCOS_EMU_GL
  opengl::GlService gl_service(a);
  opengl::install_linux_gl(gl_service);
#endif
#ifdef TCOS_EMU_X11
  // With -DTCOS_EMU_X11 the kernel's X server listens for the program at DISPLAY=:0 (the abstract name and /tmp/.X11-unix/X0 of the Linux tree), on a thread
  // of its own as in the kernel; a stand-in desktop notes the titles of the top-level windows it is shown.
  x11::Server x11_server(a);
  x11_server.attach(6, 19);
  x11::SocketServer x11_sockets(x11_server, *process::system_unix_sockets(), a);
  if (!x11_sockets.listen(0, &fs)) printf("x11: could not listen\n");
  std::atomic<bool> x11_stop{false};
  std::set<std::string> x11_titles;
  std::thread x11_thread([&] {
    while (!x11_stop) {
      while (x11_sockets.pump()) {}
      x11_sockets.wait(2'000'000);
      List<uint32_t> windows(a);
      x11_server.windows(&windows);
      for (uint32_t w : windows.view()) {
        desktop::WindowsWindowInfo info;
        String title(a);
        if (x11_server.window_info(w, &info, &title) && info.visible && info.caption) x11_titles.insert(std::string(title.c_str()));
      }
    }
  });
#endif
#ifdef TCOS_EMU_WAYLAND
  // With -DTCOS_EMU_WAYLAND the kernel's Wayland compositor listens for the program at /run/user/0/wayland-0 of the Linux tree, on a thread of its own
  // as in the kernel; a stand-in desktop draws the windows (which answers their frame callbacks) and notes the framed ones' titles and a pixel of each.
  wayland::Server wl_server(a);
  wl_server.attach(4, 24);
  kernel::Clipboard wl_clipboard(a);      // (the desktop's clipboard: its text is pasted into the program, what it copies lands here)
  wl_clipboard.set_text("text of the emulated desktop");
  wl_server.set_clipboard(&wl_clipboard);
  std::set<uint32_t> wl_activated;
  wayland::SocketServer wl_sockets(wl_server, *process::system_unix_sockets(), a);
#ifdef TCOS_EMU_DMABUF
  wl_sockets.set_gpu_images(&wayland::make_dxg_image, &wayland::make_dxg_timeline, &dxkernel);
  std::atomic<int> wl_gpu_draws{0}, wl_cpu_draws{0};
#endif
  if (!wl_sockets.listen(fs)) printf("wayland: could not listen\n");
  std::atomic<bool> wl_stop{false};
  std::map<std::string, uint32_t> wl_titles;
  std::thread wl_thread([&] {
    std::vector<uint32_t> canvas(512 * 512);
    graphics::Surface target;
    target.pixels = ArrayView<uint32_t>(canvas.data(), canvas.size());
    target.width = target.height = target.pitch = 512;
#ifdef TCOS_EMU_DMABUF
    graphics::DeviceRenderer2D on_device(adapter_device, a, {512, 512});
    bool gpu_round = false;
#endif
    while (!wl_stop) {
      while (wl_sockets.pump()) {}
      wl_sockets.wait(2'000'000);
      List<uint32_t> windows(a), changed(a);
      List<desktop::IWindowsSession::Repaint> repainted(a);
      wl_server.take_changes(&changed, &repainted);
      wl_server.windows(&windows);
      for (uint32_t w : windows.view()) {
        desktop::WindowsWindowInfo info;
        String title(a);
        if (wl_server.window_info(w, &info, &title) && info.visible && info.caption) wl_titles[std::string(title.c_str())] = wl_server.window_pixel(w, 2, 2);
        if (!info.popup && wl_activated.insert(w).second) wl_server.activate_window(w);      // (a new window gets the keyboard, as on the desktop)
        graphics::SoftwareRenderer2D renderer(target);
        uint32_t dw = 0, dh = 0;
#ifdef TCOS_EMU_DMABUF
        gpu_round = !gpu_round && on_device.valid();
        if (gpu_round) {
          if (wl_server.draw_window(w, on_device, 0, 0, &dw, &dh)) ++wl_gpu_draws;
          continue;
        }
        if (wl_server.draw_window(w, renderer, 0, 0, &dw, &dh)) ++wl_cpu_draws;
#else
        wl_server.draw_window(w, renderer, 0, 0, &dw, &dh);
#endif
      }
    }
  });
#endif
#ifdef TCOS_EMU_AUDIO
  RealtimeDevice speakers;
  audio::AudioMixer mixer(speakers, a);
  mixer.initialize();
  RealtimeMicrophone microphone;
  audio::AudioCaptureHub capture_hub(microphone, a);
  capture_hub.initialize();
  pipewire::Graph graph(a, &mixer, &capture_hub);
  graph.initialize();
  pipewire::NativeServer pw_native(graph, a);
  pipewire::PulseServer pw_pulse(graph, a);
  pipewire::WindowsAudioService windows_audio(graph, a);
  pipewire::install_windows_audio(windows_audio);
  pipewire::SocketServer pw_sockets(*process::system_unix_sockets(), a);
  if (!pipewire::listen_standard_sockets(pw_sockets, pw_native, pw_pulse, &fs, a)) printf("pipewire: could not listen\n");
  std::atomic<bool> audio_stop{false};
  std::atomic<uint32_t> most_streams{0};
  std::thread audio_thread([&] {
    while (!audio_stop) {
      mixer.pump();
      capture_hub.pump();
      for (int i = 0; i < 4 && pw_sockets.pump(); ++i) {}
      const uint32_t n = graph.playback_count();
      if (n > most_streams) most_streams = n;
      pw_sockets.wait(2'000'000);
    }
  });
  // a native program's sound: 300 Hz, stereo, for 6 s (what the media player would play)
  std::thread native_thread([&] {
    SharedPointer<audio::AudioStream> s = mixer.open_stream(audio::StreamFormat(), 16384);
    if (!s) return;
    s->set_label("Media Player");
    std::vector<uint8_t> pcm(4800 * 4);
    for (uint32_t frame = 0; frame < 48000 * 6 && !audio_stop;) {
      for (uint32_t i = 0; i < 4800; ++i) {
        const int16_t v = int16_t(5000.0 * sin(2 * M_PI * 300.0 * double(frame + i) / 48000.0));
        pcm[4 * i] = pcm[4 * i + 2] = uint8_t(v);
        pcm[4 * i + 1] = pcm[4 * i + 3] = uint8_t(uint16_t(v) >> 8);
      }
      size_t done = 0;
      while (done < pcm.size() && !audio_stop) {
        done += s->write(ConstByteView(pcm.data() + done, pcm.size() - done));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      frame += 4800;
    }
    s->close();
  });
  // the second program, at the same time as the first
  process::UserProgram* second_program = nullptr;
  std::thread second_thread;
  std::atomic<int> second_exit{-1};
  std::vector<uint8_t> second_image;
  if (argc > 3) {
    bool read_ok = false;
    second_image = slurp(argv[3], &read_ok);
    if (read_ok) {
      second_program = new process::UserProgram(a, 101, h);
      second_program->set_file_system(&fs);
      second_program->set_library_provider(&libs);
      second_program->set_processor_count(1);
      const ConstByteView image(second_image.data(), second_image.size());
      StringView av2[] = {"C:\\second.exe"};
      if (second_program->load(image, ArrayView<const StringView>(av2, 1), process::default_environment(process::abi_for_executable(exe::identify_executable(image.at_most(exe::kIdentifyBytes)))), ConstByteView(rnd, 16)) == process::LoadStatus::Ok) {
        second_thread = std::thread([&] {
          auto cpu2 = create_guest_cpu(second_program->start().arch, a);
          Sync sync2; Threads th2; Ctl ctl2;
          std::thread dog([&] { for (int i = 0; i < 600 && !ctl2.stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); ctl2.stop = true; });
          auto o2 = run_emulated_threads(*second_program, *cpu2.get(), ctl2, sync2, th2, a, 20000);
          ctl2.stop = true;
          dog.join();
          second_exit = o2.faulted ? -2 : int(o2.exit_code);
        });
      } else {
        printf("second program: load failed\n");
      }
    }
  }
#endif
#ifdef TCOS_EMU_DBUS
  // With -DTCOS_EMU_DBUS the kernel's D-Bus is there: the system bus at /run/dbus/system_bus_socket and the session bus at /run/user/0/bus (with the
  // Notifications service, which prints what it shows), carried by a thread of their own (tests/dbus_emulated.sh).
  struct PrintingDisplay final : dbus::INotificationDisplay {
    std::atomic<uint32_t> shown{0};
    uint32_t show(StringView app, StringView summary, StringView body, uint32_t timeout) override {
      printf("dbus notify: %.*s | %.*s | %.*s | %u\n", int(app.size()), app.data(), int(summary.size()), summary.data(), int(body.size()), body.data(), timeout);
      return ++shown;
    }
    void close(uint32_t) override {}
  } dbus_display;
  dbus::Bus dbus_system(a, dbus::BusKind::System, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  dbus::Bus dbus_session(a, dbus::BusKind::Session, "abcdef0123456789abcdef0123456789", "fedcba9876543210fedcba9876543210");
  dbus::NotificationService dbus_notes(dbus_session, dbus_display, a);
  if (!dbus_notes.start()) printf("dbus: notifications did not start\n");
  dbus::SocketServer dbus_sockets(*process::system_unix_sockets(), a);
  if (!dbus::listen_standard_sockets(dbus_sockets, dbus_system, dbus_session, &fs, a)) printf("dbus: could not listen\n");
  // TCOS_EMU_DBUS_SERVICES: a folder of .service files (in the Linux tree) for the session bus; their programs run as more emulated programs
  EmuLauncher dbus_launcher;
  std::vector<std::unique_ptr<process::UserProgram>> service_programs;
  std::vector<std::unique_ptr<Ctl>> service_controls;
  std::vector<std::thread> service_threads;
  std::vector<std::vector<uint8_t>> service_images;
  dbus_launcher.run = [&](const std::vector<std::string>& args, const std::vector<std::string>& env, EmuLauncher::Child& child) {
    const std::string path = std::string(filesystem::layout::kLinux) + args[0];
    UniquePointer<filesystem::IFile> file;
    if (fs.open_file(StringView(path.c_str()), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) {
      printf("dbus service: no %s\n", path.c_str());
      return false;
    }
    service_images.emplace_back(file->size());
    std::vector<uint8_t>& image = service_images.back();
    uint64_t got = 0;
    file->read(0, ByteView(image.data(), image.size()), &got);
    auto program = std::make_unique<process::UserProgram>(a, 200 + uint64_t(service_programs.size()), h);
    program->set_file_system(&fs);
    program->set_library_provider(&libs);
    program->set_processor_count(1);
    std::vector<StringView> sargs, senv;
    for (const std::string& x : args) sargs.push_back(StringView(x.data(), x.size()));
    const ConstByteView bytes(image.data(), image.size());
    for (const StringView& e : process::default_environment(process::abi_for_executable(exe::identify_executable(bytes.at_most(exe::kIdentifyBytes))))) senv.push_back(e);
    std::vector<std::string> extra = env;      // (the bus's and the unit's variables, then the starter ones: a later one of a name replaces the earlier)
    extra.push_back("DBUS_STARTER_ADDRESS=unix:path=/run/user/0/bus");
    extra.push_back("DBUS_STARTER_BUS_TYPE=session");
    for (const std::string& e : extra) {
      const StringView entry(e.data(), e.size());
      const uint64_t eq = entry.find('=');
      bool replaced = false;
      for (StringView& old : senv)
        if (!replaced && eq != StringView::npos && old.starts_with(entry.first(eq + 1))) { old = entry; replaced = true; }
      if (!replaced) senv.push_back(entry);
    }
    if (program->load(bytes, ArrayView<const StringView>(sargs.data(), sargs.size()), ArrayView<const StringView>(senv.data(), senv.size()), ConstByteView(rnd, 16)) !=
        process::LoadStatus::Ok) {
      printf("dbus service: load failed (%.*s)\n", int(program->load_detail().size()), program->load_detail().data());
      return false;
    }
    process::UserProgram* raw = program.get();
    service_programs.push_back(std::move(program));
    service_controls.push_back(std::make_unique<Ctl>());
    Ctl* ctl_s = service_controls.back().get();
    child.stop = &ctl_s->stop;
    service_threads.emplace_back([&a, raw, ctl_s, &child] {
      auto cpu_s = create_guest_cpu(raw->start().arch, a);
      Sync sync_s; Threads th_s;
      auto o = run_emulated_threads(*raw, *cpu_s.get(), *ctl_s, sync_s, th_s, a, 20000);
      child.code = o.exit_code;
      child.state = 1;
      printf("dbus service: exit=%u faulted=%d\n", o.exit_code, o.faulted);
    });
    return true;
  };
  std::string services_folder;
  if (const char* folder = getenv("TCOS_EMU_DBUS_SERVICES")) {
    services_folder = std::string(filesystem::layout::kLinux) + folder;
    const StringView folders[] = {StringView(services_folder.c_str())};
    dbus_session.set_activation(&dbus_launcher, &fs, ArrayView<const StringView>(folders, 1));
  }
  std::atomic<bool> dbus_stop{false};
  std::atomic<uint32_t> dbus_most{0};
  std::thread dbus_thread([&] {
    while (!dbus_stop) {
      for (int i = 0; i < 16 && dbus_sockets.pump(hardware::uptime_us()); ++i) {}
      if (dbus_session.connections() > dbus_most) dbus_most = dbus_session.connections();
      dbus_sockets.wait(2'000'000);
    }
  });
#endif
  std::thread watchdog([&] { for (int i = 0; i < 600 && !ctl.stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); ctl.stop = true; });
  auto o = run_emulated_threads(p, *cpu.get(), ctl, sync, th, a, 20000);
  bool wasstop = ctl.stop; ctl.stop = true; watchdog.join();
#ifdef TCOS_EMU_DBUS
  for (auto& c : service_controls) c->stop = true;      // (the activated services end with the program that used them)
  for (std::thread& t : service_threads) t.join();
  printf("dbus: %zu services started\n", service_threads.size());
  for (int i = 0; i < 50 && dbus_sockets.connections() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  dbus_stop = true;
  dbus_thread.join();
  printf("dbus: most session connections %u (the service included), %u notifications shown, %u connections left\n", dbus_most.load(), dbus_display.shown.load(),
         dbus_sockets.connections());
#endif
  // TCOS_EMU_SAVE: files the program wrote, copied out of the Linux tree (space separated linux-path=host-path, such as what pw-record recorded).
  if (const char* list = getenv("TCOS_EMU_SAVE")) {
    std::string all = list;
    for (size_t at = 0; at < all.size();) {
      size_t end = all.find(' ', at);
      if (end == std::string::npos) end = all.size();
      const std::string pair = all.substr(at, end - at);
      at = end + 1;
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) continue;
      const std::string from = std::string(filesystem::layout::kLinux) + pair.substr(0, eq), to = pair.substr(eq + 1);
      UniquePointer<filesystem::IFile> file;
      if (fs.open_file(StringView(from.c_str()), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) { printf("save: no %s\n", from.c_str()); continue; }
      std::vector<uint8_t> bytes(file->size());
      uint64_t got = 0;
      file->read(0, ByteView(bytes.data(), bytes.size()), &got);
      if (FILE* f = fopen(to.c_str(), "wb")) { fwrite(bytes.data(), 1, got, f); fclose(f); printf("save: %s, %llu bytes\n", to.c_str(), (unsigned long long)got); }
    }
  }
#ifdef TCOS_EMU_DXG
  process::win32k::dxg::shutdown_for_tests();      // (the layer's records point into the kernel and the allocator, which are going away)
  dxgk::set_system_kernel(nullptr);
#endif
#ifdef TCOS_EMU_AUDIO
  if (second_thread.joinable()) second_thread.join();
  printf("second program: exit=%d\n", second_exit.load());
  native_thread.join();
  // let the mixer play out what is queued
  for (int i = 0; i < 100 && graph.playback_count() > 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); graph.refresh(); }
  audio_stop = true;
  audio_thread.join();
  {
    std::lock_guard<std::mutex> g(speakers.m);
    const std::vector<int16_t>& rec = speakers.recorded;
    const size_t frames = rec.size() / 2, window = 4800;
    double best_all = 0, best[3] = {0, 0, 0};
    const double freqs[3] = {300, 400, 500};
    for (size_t at = 0; at + window <= frames; at += window / 2) {
      double m[3];
      for (int k = 0; k < 3; ++k) { m[k] = tone(rec, at, window, freqs[k]); if (m[k] > best[k]) best[k] = m[k]; }
      const double all = std::min(m[0], std::min(m[1], m[2]));
      if (all > best_all) best_all = all;
    }
    printf("audio: %zu frames mixed, most streams at once %u\n", frames, most_streams.load());
    printf("audio: native 300 Hz %.0f, windows 400 Hz %.0f, linux 500 Hz %.0f; all three at once %.0f\n", best[0], best[1], best[2], best_all);
    // every program's tone on its own, and the ones in TCOS_EMU_TONES (default 300,400,500) at the same moment
    std::vector<double> together;
    {
      const char* list = getenv("TCOS_EMU_TONES");
      std::string text = list ? list : "300,400,500";
      for (size_t at = 0; at < text.size();) {
        const size_t comma = text.find(',', at);
        together.push_back(atof(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str()));
        at = comma == std::string::npos ? text.size() : comma + 1;
      }
    }
    double best_tone[5] = {0, 0, 0, 0, 0}, best_together = 0;
    for (size_t at = 0; at + window <= frames; at += window / 2) {
      for (int k = 0; k < 5; ++k) best_tone[k] = std::max(best_tone[k], tone(rec, at, window, 300.0 + 100.0 * k));
      double all = 1e18;
      for (double f : together) all = std::min(all, tone(rec, at, window, f));
      best_together = std::max(best_together, all);
    }
    printf("audio: tones 300:%.0f 400:%.0f 500:%.0f 600:%.0f 700:%.0f; together %.0f\n", best_tone[0], best_tone[1], best_tone[2], best_tone[3], best_tone[4], best_together);
  }
  delete second_program;
#endif
#ifdef TCOS_EMU_X11
  x11_stop = true;
  x11_thread.join();
  for (const std::string& t : x11_titles) printf("x11-desktop: framed window \"%s\"\n", t.c_str());
#endif
#ifdef TCOS_EMU_WAYLAND
  wl_stop = true;
  wl_thread.join();
  for (const auto& t : wl_titles) printf("wayland-desktop: framed window \"%s\" pixel %08x\n", t.first.c_str(), t.second);
  {
    String text(a);
    if (wl_clipboard.get_text(&text)) printf("wayland-desktop: clipboard \"%s\"\n", text.c_str());
  }
  wl_server.set_clipboard(nullptr);
#ifdef TCOS_EMU_DMABUF
  printf("wayland-desktop: %d draws on the device, %d on the CPU\n", wl_gpu_draws.load(), wl_cpu_draws.load());
  // (a dmabuf the program still has open holds an export of the kernel, which goes before the program: its descriptors are closed first)
  if (process::Process* proc = p.process())
    for (uint64_t fd = 0; fd < process::ProcessFiles::kMaxDescriptors; ++fd) proc->files().close(fd);
#endif
#endif
  printf("[exit=%u faulted=%d killed=%d vector=%d addr=%llx pc=%llx]\n", o.exit_code, o.faulted, o.killed, o.fault.vector, (unsigned long long)o.fault.address, (unsigned long long)o.fault.pc);
}
