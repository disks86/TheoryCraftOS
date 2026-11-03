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
#include "Process/Win32k/Dxg/DxgInternal.hpp"
#include "Process/Win32k/Dxg/DxgLayouts.hpp"

// Contexts and the work that goes to them: NtGdiDdDDICreateContext(Virtual), DestroyContext, Get/SetContextSchedulingPriority, Render, Present, Escape.
//
// A WDDM 1.x context hands the user-mode driver three buffers it fills before every Render call: the DMA buffer, the allocation list and the patch location list. Here the
// DMA buffer is the command stream of CommandStream.hpp and the allocation list names the allocations its indices refer to; the buffers are a region of the process's own
// memory that CreateContext maps (the kernel reads from there on Render, after copying: the process may change it meanwhile). The patch list is mapped for the driver's
// benefit but unused - the stream names allocations by index and the kernel decides where memory is.
namespace process::win32k::dxg {

namespace {

using namespace layout;
namespace status = dxgk::status;

class Locked {
 public:
  explicit Locked(Session& s) : mutex_(s.state().lock) { mutex_.lock(); }
  ~Locked() { mutex_.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  kernel::Mutex& mutex_;
};

// --- Contexts -------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t create_context_common(Session& s, Call& call, uint64_t arg, bool with_buffers) {
  UserBlock b;
  if (!b.load(call, arg, with_buffers ? sizeof_create_context : sizeof_create_context_virtual)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t node = b.u32(4), affinity = b.u32(8);
  if (device == 0) return kStatusInvalidParameter;
  if (node != 0 || affinity > 1) return kStatusInvalidParameter;      // (one engine)
  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  dxgk::Handle context = 0;
  uint32_t st = s.kernel().create_context(s.pid(), device, dxgk::ContextPriority::Normal, &context);
  if (st != status::kSuccess) return st;
  ContextRecord rec;
  rec.context = context;
  rec.device = device;
  if (with_buffers && map_user_region(call, kContextRegionBytes, &rec.region_base)) rec.region_size = kContextRegionBytes;
  const bool mapped = rec.region_size != 0;
  bool wrote = put32(call, arg, 32, context);
  if (with_buffers) {
    const uint64_t base = rec.region_base;
    wrote = wrote && put64(call, arg, 40, base) && put32(call, arg, 48, mapped ? static_cast<uint32_t>(kCommandBufferBytes) : 0) &&
            put64(call, arg, 56, mapped ? base + kAllocationListOffset : 0) && put32(call, arg, 64, mapped ? kAllocationListEntries : 0) &&
            put64(call, arg, 72, mapped ? base + kPatchListOffset : 0) && put32(call, arg, 80, mapped ? kPatchListEntries : 0) && put64(call, arg, 88, 0);
  }
  if (!wrote || !ps.contexts.push_back(rec)) {
    unmap_user_region(call, rec.region_base, rec.region_size);
    s.kernel().destroy_context(s.pid(), context);
    return wrote ? status::kNoMemory : kStatusAccessViolation;
  }
  return status::kSuccess;
}

uint32_t create_context(Session& s, Call& call, uint64_t arg) { return create_context_common(s, call, arg, true); }
uint32_t create_context_virtual(Session& s, Call& call, uint64_t arg) { return create_context_common(s, call, arg, false); }

uint32_t destroy_context(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_handle)) return load_failure(arg);
  const dxgk::Handle context = b.u32(0);
  if (context == 0) return kStatusInvalidParameter;
  Locked g(s);
  const uint32_t st = s.kernel().destroy_context(s.pid(), context);
  if (st != status::kSuccess) return st;
  ProcessState& ps = s.state();
  for (uint64_t i = 0; i < ps.contexts.count(); ++i)
    if (ps.contexts[i].context == context) {
      unmap_user_region(call, ps.contexts[i].region_base, ps.contexts[i].region_size);
      ps.contexts.remove_at(i);
      break;
    }
  return status::kSuccess;
}

// The scheduling priority a process asked for is kept with the context; the kernel's scheduler has the three levels it was created with (Normal) and does not change them.
constexpr int32_t kMinPriority = -7, kMaxPriority = 7;

uint32_t get_priority(Session& s, Call& call, uint64_t arg, bool in_process) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_context_priority)) return load_failure(arg);
  if (b.u32(0) == 0) return kStatusInvalidParameter;
  Locked g(s);
  ContextRecord* c = s.state().find_context(b.u32(0));
  if (!c) return status::kInvalidHandle;
  return put32(call, arg, 4, static_cast<uint32_t>(in_process ? c->in_process_priority : c->priority)) ? status::kSuccess : kStatusAccessViolation;
}

uint32_t set_priority(Session& s, Call& call, uint64_t arg, bool in_process) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_context_priority)) return load_failure(arg);
  if (b.u32(0) == 0) return kStatusInvalidParameter;
  const int32_t priority = b.i32(4);
  if (priority < kMinPriority || priority > kMaxPriority) return kStatusInvalidParameter;
  Locked g(s);
  ContextRecord* c = s.state().find_context(b.u32(0));
  if (!c) return status::kInvalidHandle;
  (in_process ? c->in_process_priority : c->priority) = priority;
  return status::kSuccess;
}

uint32_t get_context_priority(Session& s, Call& call, uint64_t arg) { return get_priority(s, call, arg, false); }
uint32_t set_context_priority(Session& s, Call& call, uint64_t arg) { return set_priority(s, call, arg, false); }
uint32_t get_in_process_priority(Session& s, Call& call, uint64_t arg) { return get_priority(s, call, arg, true); }
uint32_t set_in_process_priority(Session& s, Call& call, uint64_t arg) { return set_priority(s, call, arg, true); }

// --- Render ---------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kRenderNullRendering = 1u << 3, kRenderKm = 1u << 5;

uint32_t render(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_render_head)) return load_failure(arg);
  const dxgk::Handle context = b.u32(0);
  const uint32_t offset = b.u32(4), length = b.u32(8), allocation_count = b.u32(12), patch_count = b.u32(16);
  const uint32_t flags = b.u32(68);
  if (context == 0) return kStatusInvalidParameter;
  if (b.u32(80) != 0 || (flags & kRenderKm)) return kStatusNotSupported;      // broadcast contexts; kernel-mode rendering
  if (length == 0 || length % 4 != 0 || offset > kCommandBufferBytes || length > kCommandBufferBytes - offset) return kStatusInvalidParameter;
  if (allocation_count > kAllocationListEntries || patch_count > kPatchListEntries) return kStatusInvalidParameter;

  Locked g(s);
  ContextRecord* rec = s.state().find_context(context);
  if (!rec) return status::kInvalidHandle;
  if (rec->region_size == 0) return kStatusInvalidParameter;      // (a virtual context has no DMA buffer)
  // Copies out of the caller's buffers: what is run is what was read here.
  ByteBuffer commands(s.allocator());
  if (!commands.put_zeros(length)) return status::kNoMemory;
  if (!call.read(rec->region_base + offset, commands.writable_view())) return kStatusAccessViolation;
  List<dxgk::Handle> allocations(s.allocator());
  for (uint32_t i = 0; i < allocation_count; ++i) {
    uint32_t handle = 0;
    if (!call.read_u32(element_address(rec->region_base + kAllocationListOffset, i, 8), &handle) || !allocations.push_back(handle)) return kStatusAccessViolation;
  }
  const uint32_t st = (flags & kRenderNullRendering) ? status::kSuccess
                                                       : s.kernel().render(s.pid(), context, commands.view(), ArrayView<const dxgk::Handle>(allocations.view()));
  if (st != status::kSuccess) return st;
  // The driver goes on with the same buffers (the commands are copied; nothing is left in them that the kernel needs).
  const uint64_t base = rec->region_base;
  const bool wrote = put64(call, arg, 24, base) && put32(call, arg, 32, static_cast<uint32_t>(kCommandBufferBytes)) && put64(call, arg, 40, base + kAllocationListOffset) &&
                     put32(call, arg, 48, kAllocationListEntries) && put64(call, arg, 56, base + kPatchListOffset) && put32(call, arg, 64, kPatchListEntries);
  return wrote ? status::kSuccess : kStatusAccessViolation;
}

// --- Present ---------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kPresentBlt = 1u << 0, kPresentColorFill = 1u << 1, kPresentFlip = 1u << 2, kPresentDstRectValid = 1u << 6, kPresentSrcRectValid = 1u << 7;

uint32_t present(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_present_head)) return load_failure(arg);
  const dxgk::Handle first = b.u32(0);
  const dxgk::Handle source = b.u32(20), destination = b.u32(24);
  const uint32_t flags = b.u32(88);
  if (first == 0 || source == 0) return kStatusInvalidParameter;
  if (b.u32(16) != 0) return kStatusInvalidParameter;      // VidPnSourceId (one source)
  if (flags & kPresentColorFill) return kStatusNotSupported;
  const bool blt = (flags & kPresentBlt) != 0, flip = (flags & kPresentFlip) != 0;
  if (blt == flip) return kStatusInvalidParameter;      // exactly one of the two
  if (blt && destination == 0) return kStatusInvalidParameter;
  dxgk::PresentArgs args;
  args.source = source;
  args.destination = blt ? destination : 0;
  if (flags & kPresentSrcRectValid) {
    const int32_t l = b.i32(48), t = b.i32(52), r = b.i32(56), bo = b.i32(60);
    if (l < 0 || t < 0 || r <= l || bo <= t) return kStatusInvalidParameter;
    args.source_rect = {l, t, static_cast<uint32_t>(r - l), static_cast<uint32_t>(bo - t)};
  }
  bool have_dest = false;
  int32_t dest_l = 0, dest_t = 0, dest_w = 0, dest_h = 0;
  if (flags & kPresentDstRectValid) {
    const int32_t l = b.i32(32), t = b.i32(36), r = b.i32(40), bo = b.i32(44);
    if (l < 0 || t < 0 || r <= l || bo <= t) return kStatusInvalidParameter;
    have_dest = true;
    dest_l = l, dest_t = t, dest_w = r - l, dest_h = bo - t;
    args.dest_x = l;
    args.dest_y = t;
  }
  if (have_dest) {
    // The copy is not stretched: the destination rectangle has the size of the source rectangle (the whole source when none is given).
    uint32_t w = args.source_rect.width, h = args.source_rect.height;
    if (!(flags & kPresentSrcRectValid)) {
      dxgk::AllocationInfo info;
      const uint32_t st = s.kernel().query_allocation(s.pid(), source, &info);
      if (st != status::kSuccess) return st;
      w = info.desc.width, h = info.desc.height;
    }
    if (static_cast<uint32_t>(dest_w) != w || static_cast<uint32_t>(dest_h) != h) return kStatusNotSupported;
    (void)dest_l, (void)dest_t;
  }
  Locked g(s);
  dxgk::Handle context = first;
  if (!s.state().find_context(context)) {
    // (the first field is a device handle in the old form of the call: the device's first context then)
    context = 0;
    if (s.state().find_device(first))
      for (ContextRecord& c : s.state().contexts.view())
        if (c.device == first) {
          context = c.context;
          break;
        }
    if (!context) return status::kInvalidHandle;
  }
  return s.kernel().present(s.pid(), context, args);
}

// --- Escape -----------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kEscapeDriverPrivate = 0;

uint32_t escape(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_escape)) return load_failure(arg);
  const dxgk::Handle device = b.u32(4);
  const uint32_t type = b.u32(8);
  const uint64_t data = b.u64(16);
  const uint32_t size = b.u32(24);
  if (device == 0) return kStatusInvalidParameter;
  if (type != kEscapeDriverPrivate) return kStatusNotSupported;      // VidMm, VidSch, DMM, TDR control ... are Windows' own escapes
  if (data == 0 || size == 0 || size > kMaxPrivateData) return kStatusInvalidParameter;
  ByteBuffer request(s.allocator()), reply(s.allocator());
  if (!read_blob(call, data, size, &request)) return kStatusAccessViolation;
  if (!reply.put_zeros(size)) return status::kNoMemory;
  uint32_t reply_bytes = 0;
  const uint32_t st = s.kernel().escape(s.pid(), device, request.view(), reply.writable_view(), &reply_bytes);
  if (st != status::kSuccess) return st;
  if (reply_bytes > size) return status::kInvalidDeviceState;
  // (the reply goes into the same buffer, as private driver data does)
  return reply_bytes == 0 || call.write(data, reply.view().first(reply_bytes)) ? status::kSuccess : kStatusAccessViolation;
}

}  // namespace

void register_work() {
  using N = WindowsSyscallNumber;
  add<create_context>(N::NtGdiDdDDICreateContext);
  add<create_context_virtual>(N::NtGdiDdDDICreateContextVirtual);
  add<destroy_context>(N::NtGdiDdDDIDestroyContext);
  add<get_context_priority>(N::NtGdiDdDDIGetContextSchedulingPriority);
  add<set_context_priority>(N::NtGdiDdDDISetContextSchedulingPriority);
  add<get_in_process_priority>(N::NtGdiDdDDIGetContextInProcessSchedulingPriority);
  add<set_in_process_priority>(N::NtGdiDdDDISetContextInProcessSchedulingPriority);
  add<render>(N::NtGdiDdDDIRender);
  add<present>(N::NtGdiDdDDIPresent);
  add<escape>(N::NtGdiDdDDIEscape);
}

}  // namespace process::win32k::dxg
