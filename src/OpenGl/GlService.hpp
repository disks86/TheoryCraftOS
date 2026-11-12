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

// The system's OpenGL: contexts and window surfaces of every process, for every personality (the Windows service NtTcosOpenGl is one front end; a Linux one would call the same
// methods). A process's contexts, share groups and surfaces are its own; the calls of one process are serialized by its own lock, so separate programs render at the same time.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlSurface.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace opengl {

struct ContextRequest {
  uint32_t major = 4, minor = 6;
  bool core_profile = false, forward_compatible = false, debug = false;
  bool es = false;         // OpenGL ES (EGL): the version strings, extensions and shading language of OpenGL ES 3.0
  uint32_t share = 0;      // a context of the same process whose objects the new one shares (0: none)
};

// What the window needs: its client size and the pixel format chosen for it.
struct SurfaceRequest {
  uint64_t window = 0;      // the personality's name for the window (an HWND); 0 = no surface
  uint32_t width = 1, height = 1;
  bool double_buffered = true;
  uint32_t depth_bits = 24, stencil_bits = 8, alpha_bits = 8, accum_bits = 0;
};

enum class GlStatus : uint32_t { Ok, InvalidHandle, InvalidParameter, NoMemory, NoContext, Busy };

class GlService {
 public:
  explicit GlService(allocators::IAllocator& allocator) : a_(allocator), processes_(allocator) {}
  GlService(const GlService&) = delete;
  GlService& operator=(const GlService&) = delete;

  GlStatus create_context(uint64_t pid, const ContextRequest& request, uint32_t* handle);
  GlStatus delete_context(uint64_t pid, uint32_t handle);
  // wglShareLists: `context` (which has no objects yet) takes the objects of `source`.
  GlStatus share_objects(uint64_t pid, uint32_t source, uint32_t context);
  // Makes `context` current on thread `thread` with the surface (0: none current). A context current on another thread is Busy.
  GlStatus make_current(uint64_t pid, uint64_t thread, uint32_t context, const SurfaceRequest& surface);
  // Runs a command stream in the thread's current context; *result is the last result.
  GlStatus execute(uint64_t pid, uint64_t thread, ConstByteView stream, IClientMemory& memory, uint64_t* result);
  // Swaps the surface of a window (swap false: only shows the front buffer as it is). `present` gets the picture to show (rows top first, R, G, B, A bytes) and may be null;
  // it runs after the service's locks are let go, so it may take the window system's.
  using PresentFunction = void (*)(void* context, uint64_t window, ConstByteView rgba, uint32_t width, uint32_t height);
  GlStatus swap(uint64_t pid, uint64_t window, uint32_t width, uint32_t height, bool swap, PresentFunction present, void* present_context);
  // The picture a window shows, top row first, 0xAARRGGBB (B, G, R, A bytes), `pitch` bytes a row.
  GlStatus read_front(uint64_t pid, uint64_t window, ByteView out, uint32_t pitch, uint32_t* width, uint32_t* height);
  // A single-buffered surface that was flushed since the last call (glFlush): shown like a swap would.
  bool take_flushed(uint64_t pid, uint64_t window);
  // The surface of a window goes (eglDestroySurface); a context drawing to it is left with no drawable. InvalidHandle if there is none.
  GlStatus destroy_surface(uint64_t pid, uint64_t window);
  void process_exited(uint64_t pid);
  uint32_t context_count(uint64_t pid);
  // How a context's draws have gone so far (on the 3D hardware or on the CPU); false if there is no such context. `accelerator` (optional): the name of the 3D hardware
  // its draws go to, null when there is none.
  bool hardware_stats(uint64_t pid, uint32_t context, webgl::GlContext::HardwareStats* out, const char** accelerator = nullptr);
  // Told, for each context that goes (deleted, or its process exited) and drew something, how many of its draws the 3D hardware took (the kernel logs it).
  using ReportFunction = void (*)(uint64_t pid, const webgl::GlContext::HardwareStats& stats);
  void set_report(ReportFunction report) { report_ = report; }
  allocators::IAllocator& allocator() { return a_; }

 private:
  struct Context {
    Context() = default;
    uint32_t handle = 0;
    SharedPointer<ShareGroup> group;
    UniquePointer<GlApi> api;
    uint64_t thread = 0;      // current on (0: none)
    uint64_t window = 0;
    ContextRequest request;
  };
  struct Surface {
    uint64_t window = 0;
    UniquePointer<GlSurface> surface;
  };
  struct Process {
    explicit Process(allocators::IAllocator& a) : contexts(a), surfaces(a) {}
    uint64_t pid = 0;
    kernel::Mutex lock;
    List<UniquePointer<Context>> contexts;
    List<UniquePointer<Surface>> surfaces;
    uint32_t next_handle = 1;
    Context* find(uint32_t handle);
    Context* current(uint64_t thread);
    Surface* surface(uint64_t window);
  };
  SharedPointer<Process> process(uint64_t pid, bool create);
  Surface* surface_for(Process& p, const SurfaceRequest& request);
  static void sync_hardware(Process& p, GlSurface* surface);

  void report(uint64_t pid, const Context& c) const;
  allocators::IAllocator& a_;
  ReportFunction report_ = nullptr;
  kernel::Mutex lock_;      // the list of processes only
  List<SharedPointer<Process>> processes_;
};

void set_system_gl(GlService* service);
GlService* system_gl();

}  // namespace opengl
