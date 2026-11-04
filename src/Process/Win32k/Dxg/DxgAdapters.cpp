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
#include <initializer_list>
#include "Process/Win32k/Dxg/DxgInternal.hpp"
#include "Process/Win32k/Dxg/DxgLayouts.hpp"

// Adapters, devices, display modes, adapter queries and video memory: NtGdiDdDDIOpenAdapterFrom*, EnumAdapters(2/3), CloseAdapter, QueryAdapterInfo, QueryVideoMemoryInfo,
// CreateDevice, DestroyDevice, GetDeviceState, GetDisplayModeList, SetDisplayMode, WaitForVerticalBlankEvent, SetQueuedLimit, WaitForIdle, TrimProcessCommitment.
namespace process::win32k::dxg {

namespace {

using namespace layout;
namespace status = dxgk::status;

// The ProcessState and the kernel are used with the process lock held.
class Locked {
 public:
  explicit Locked(Session& s) : mutex_(s.state().lock) { mutex_.lock(); }
  ~Locked() { mutex_.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  kernel::Mutex& mutex_;
};

// Opens adapter `index` for the process and notes the handle. With the process lock held.
uint32_t open_index(Session& s, uint32_t index, dxgk::Handle* handle) {
  const uint32_t st = s.kernel().open_adapter_from_index(s.pid(), index, handle);
  if (st != status::kSuccess) return st;
  AdapterRecord rec;
  rec.handle = *handle;
  rec.index = index;
  if (!s.state().adapters.push_back(rec)) {
    s.kernel().close_adapter(s.pid(), *handle);
    return status::kNoMemory;
  }
  return status::kSuccess;
}

// The adapter number a handle of the process names (with the process lock held).
bool adapter_index(Session& s, dxgk::Handle handle, uint32_t* index) {
  AdapterRecord* rec = s.state().find_adapter(handle);
  if (!rec) return false;
  *index = rec->index;
  return true;
}

// The device interface name of adapter `n`, which D3DKMTOpenAdapterFromDeviceName takes.
void device_name(uint32_t n, WideString* out) {
  out->append_ascii("\\\\?\\DISPLAY#TCOSDXG#");
  char digits[11];
  uint32_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + n % 10);
    n /= 10;
  } while (n != 0);
  while (count != 0) out->append(static_cast<char16_t>(digits[--count]));
  out->append_ascii("#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}");
}

// --- Opening ----------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t open_from_luid(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_open_from_luid)) return load_failure(arg);
  dxgk::Luid luid;
  luid.low = b.u32(0);
  luid.high = b.i32(4);
  Locked g(s);
  for (uint32_t i = 0; i < s.kernel().adapter_count(); ++i) {
    if (!(s.kernel().adapter(i)->luid() == luid)) continue;
    dxgk::Handle h = 0;
    const uint32_t st = open_index(s, i, &h);
    if (st != status::kSuccess) return st;
    if (!put32(call, arg, 8, h)) {
      s.kernel().close_adapter(s.pid(), h);
      s.state().adapters.remove_at(s.state().adapters.count() - 1);
      return kStatusAccessViolation;
    }
    return status::kSuccess;
  }
  return status::kNotFound;
}

uint32_t open_from_device_name(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_open_from_device_name)) return load_failure(arg);
  const uint64_t name_address = b.u64(0);
  if (name_address == 0) return kStatusInvalidParameter;
  WideString name(s.allocator());
  if (!call.read_wide_z(name_address, 256, &name)) return kStatusInvalidParameter;
  Locked g(s);
  for (uint32_t i = 0; i < s.kernel().adapter_count(); ++i) {
    WideString want(s.allocator());
    device_name(i, &want);
    if (!want.equals(name.view(), true)) continue;
    dxgk::Handle h = 0;
    const uint32_t st = open_index(s, i, &h);
    if (st != status::kSuccess) return st;
    const dxgk::Luid luid = s.kernel().adapter(i)->luid();
    if (!put32(call, arg, 8, h) || !put32(call, arg, 12, luid.low) || !put32(call, arg, 16, static_cast<uint32_t>(luid.high))) {
      s.kernel().close_adapter(s.pid(), h);
      s.state().adapters.remove_at(s.state().adapters.count() - 1);
      return kStatusAccessViolation;
    }
    return status::kSuccess;
  }
  return status::kNotFound;
}

uint32_t close_adapter(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_handle)) return load_failure(arg);
  const dxgk::Handle h = b.u32(0);
  if (h == 0) return kStatusInvalidParameter;
  Locked g(s);
  const uint32_t st = s.kernel().close_adapter(s.pid(), h);
  if (st == status::kSuccess) {
    for (uint64_t i = 0; i < s.state().adapters.count(); ++i)
      if (s.state().adapters[i].handle == h) {
        s.state().adapters.remove_at(i);
        break;
      }
  }
  return st;
}

// --- Enumeration -------------------------------------------------------------------------------------------------------------------------------------------

// Opens every adapter and fills `out` (D3DKMT_ADAPTERINFO entries) at `address`; on failure closes what was opened. With the process lock held.
uint32_t enum_into(Session& s, Call& call, uint64_t address, uint32_t count) {
  List<dxgk::Handle> opened(s.allocator());
  uint32_t st = status::kSuccess;
  for (uint32_t i = 0; i < count && st == status::kSuccess; ++i) {
    dxgk::Handle h = 0;
    st = open_index(s, i, &h);
    if (st != status::kSuccess) break;
    if (!opened.push_back(h)) {
      st = status::kNoMemory;
      break;
    }
    dxgk::Adapter* a = s.kernel().adapter(i);
    const uint64_t entry = element_address(address, i, sizeof_adapter_info);
    if (!put32(call, entry, 0, h) || !put32(call, entry, 4, a->luid().low) || !put32(call, entry, 8, static_cast<uint32_t>(a->luid().high)) ||
        !put32(call, entry, 12, a->display() ? 1 : 0) || !put32(call, entry, 16, 0))
      st = kStatusAccessViolation;
  }
  if (st != status::kSuccess) {
    for (dxgk::Handle h : opened.view()) {
      s.kernel().close_adapter(s.pid(), h);
      for (uint64_t i = 0; i < s.state().adapters.count(); ++i)
        if (s.state().adapters[i].handle == h) {
          s.state().adapters.remove_at(i);
          break;
        }
    }
  }
  return st;
}

uint32_t enum_adapters(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_enum_adapters)) return load_failure(arg);
  Locked g(s);
  const uint32_t count = s.kernel().adapter_count();
  if (count > kMaxAdaptersPerCall) return status::kBufferTooSmall;
  // (the array is part of the structure: it must be writable as a whole)
  uint32_t st = enum_into(s, call, element_address(arg, 4, 1), count);
  if (st == status::kSuccess && !put32(call, arg, 0, count)) st = kStatusAccessViolation;
  return st;
}

// EnumAdapters2 and EnumAdapters3 differ in a filter in front; `array_offset` is where the pointer is, `count_offset` the count.
uint32_t enum_adapters_ptr(Session& s, Call& call, uint64_t arg, uint64_t size, uint64_t count_offset, uint64_t array_offset) {
  UserBlock b;
  if (!b.load(call, arg, size)) return load_failure(arg);
  const uint32_t capacity = b.u32(count_offset);
  const uint64_t array = b.u64(array_offset);
  Locked g(s);
  const uint32_t count = s.kernel().adapter_count();
  if (array == 0) return put32(call, arg, count_offset, count) ? status::kSuccess : kStatusAccessViolation;
  if (capacity < count) {
    put32(call, arg, count_offset, count);
    return status::kBufferTooSmall;
  }
  uint32_t st = enum_into(s, call, array, count);
  if (st == status::kSuccess && !put32(call, arg, count_offset, count)) st = kStatusAccessViolation;
  return st;
}

uint32_t enum_adapters2(Session& s, Call& call, uint64_t arg) { return enum_adapters_ptr(s, call, arg, sizeof_enum_adapters2, 0, 8); }

uint32_t dxgk_enum_adapters3(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_enum_adapters3)) return load_failure(arg);
  if (b.u64(0) & ~uint64_t{3}) return kStatusInvalidParameter;      // IncludeComputeOnly, IncludeDisplayOnly: the only filter bits (every adapter here is both, so both filters match)
  return enum_adapters_ptr(s, call, arg, sizeof_enum_adapters3, 8, 16);
}

// --- Adapter queries ---------------------------------------------------------------------------------------------------------------------------------------

// KMTQUERYADAPTERINFOTYPE values (d3dkmthk.h): the ones answered, and the capability queries that are answered "nothing" by zeroing the buffer.
enum QueryType : uint32_t {
  kQaiDriverPrivate = 0, kQaiGetSegmentSize = 3, kQaiAdapterGuid = 4, kQaiDriverVersion = 13, kQaiAdapterType = 15, kQaiWddm12Caps = 17, kQaiDirectFlipSupport = 19,
  kQaiMultiPlaneOverlaySupport = 20, kQaiWddm13Caps = 22, kQaiMultiPlaneOverlayHudSupport = 23, kQaiWddm20Caps = 24, kQaiIndependentFlipSupport = 28, kQaiPhysicalAdapterCount = 30,
  kQaiTrackedWorkloadSupport = 72,
};
constexpr uint32_t kDriverVersionWddm13 = 1300;

uint32_t query_adapter_info(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_query_adapter_info)) return load_failure(arg);
  const dxgk::Handle handle = b.u32(0);
  const uint32_t type = b.u32(4);
  const uint64_t data = b.u64(8);
  const uint32_t size = b.u32(16);
  if (handle == 0) return kStatusInvalidParameter;
  Locked g(s);
  uint32_t index = 0;
  if (!adapter_index(s, handle, &index)) return status::kInvalidHandle;
  dxgk::Adapter* adapter = s.kernel().adapter(index);
  if (!adapter) return status::kInvalidHandle;
  if (data == 0 || size == 0) return kStatusInvalidParameter;
  ByteBuffer out(s.allocator());
  uint32_t needed = 0;
  switch (type) {
    case kQaiDriverVersion:
      out.put32(kDriverVersionWddm13);
      break;
    case kQaiAdapterType: {
      const dxgk::AdapterCaps& caps = adapter->caps();
      out.put32(1u | (adapter->display() ? 2u : 0u) | (caps.accelerated ? 0u : 4u));      // RenderSupported, DisplaySupported, SoftwareDevice
      break;
    }
    case kQaiPhysicalAdapterCount:
      out.put32(1);
      break;
    case kQaiAdapterGuid: {
      // A stable identifier derived from the LUID: {low, high, 'TC', "TheoryCr"}.
      const uint8_t tail[8] = {'T', 'h', 'e', 'o', 'r', 'y', 'C', 'r'};
      out.put32(adapter->luid().low);
      out.put16(static_cast<uint32_t>(adapter->luid().high));
      out.put16(0x4354);
      out.append(ConstByteView(tail, 8));
      break;
    }
    case kQaiGetSegmentSize: {
      // D3DKMT_SEGMENTSIZEINFO: dedicated video memory (the local segment), dedicated system memory, shared system memory (the non-local segment).
      dxgk::VideoMemoryInfo local, shared;
      uint32_t st = s.kernel().query_video_memory_info(s.pid(), handle, dxgk::MemorySegmentGroup::Local, &local);
      if (st == status::kSuccess) st = s.kernel().query_video_memory_info(s.pid(), handle, dxgk::MemorySegmentGroup::NonLocal, &shared);
      if (st != status::kSuccess) return st;
      for (uint64_t v : {local.budget, uint64_t{0}, shared.budget}) {
        out.put32(static_cast<uint32_t>(v));
        out.put32(static_cast<uint32_t>(v >> 32));
      }
      break;
    }
    case kQaiDriverPrivate: {
      // What the user-mode driver of this kernel asks for: the adapter's capabilities (DxgAbi.hpp, "adapter caps").
      dxgk::AdapterCaps caps;
      const uint32_t st = s.kernel().query_adapter_caps(s.pid(), handle, &caps);
      if (st != status::kSuccess) return st;
      const uint32_t flags = (caps.accelerated ? 1u : 0u) | (caps.stencil ? 2u : 0u) | (caps.mipmaps ? 4u : 0u) | (caps.textures ? 8u : 0u) | (caps.programmable_vertex ? 16u : 0u) |
                             (caps.programmable_pixel ? 32u : 0u);
      for (uint32_t w : {caps.max_texture_dimension, caps.max_vertex_attributes, caps.max_varyings, caps.max_texture_units, caps.max_push_constant_bytes, caps.max_vertex_stride, flags, caps.features})
        out.put32(w);
      uint8_t name[64] = {};
      for (uint32_t i = 0; i < 63 && caps.name[i]; ++i) name[i] = static_cast<uint8_t>(caps.name[i]);
      out.append(ConstByteView(name, 64));
      for (uint32_t w : {caps.max_color_targets, caps.max_vertex_buffers}) out.put32(w);      // (after the name: what a caller built before them reads is unchanged)
      // After the name (a user-mode driver that reads only the head does not see them): the multisample counts, the kind of device, the vendor and device numbers. The Vulkan driver reports them.
      for (uint32_t w : {caps.sample_counts, caps.device_kind, caps.vendor_id, caps.device_id}) out.put32(w);
      break;
    }
    case kQaiWddm12Caps: case kQaiWddm13Caps: case kQaiWddm20Caps: case kQaiDirectFlipSupport: case kQaiMultiPlaneOverlaySupport: case kQaiMultiPlaneOverlayHudSupport:
    case kQaiIndependentFlipSupport: case kQaiTrackedWorkloadSupport: {
      // No preemption, no overlays, no flip, no tracked workloads: the answer is all zero whatever the structure's exact size.
      const uint32_t n = size < 256 ? size : 256;
      out.put_zeros(n);
      needed = n;
      break;
    }
    default:
      return kStatusInvalidParameter;
  }
  if (needed == 0) needed = static_cast<uint32_t>(out.size());
  // The capabilities blob grew at its tail: a caller that asks for the 96 bytes it always did gets them.
  if (type == kQaiDriverPrivate && size >= 96 && size < needed) needed = size;
  if (size < needed) return status::kBufferTooSmall;
  return call.write(data, out.view().first(needed)) ? status::kSuccess : kStatusAccessViolation;
}

uint32_t query_video_memory_info(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_query_video_memory_info)) return load_failure(arg);
  const uint64_t process = b.u64(0);
  const dxgk::Handle handle = b.u32(8);
  const uint32_t group = b.u32(12);
  if (handle == 0 || group > 1 || b.u32(48) != 0) return kStatusInvalidParameter;
  if (process != 0 && process != ~uint64_t{0}) return kStatusNotSupported;      // (other processes: the handle would have to be resolved by the object manager)
  dxgk::VideoMemoryInfo info;
  const uint32_t st = s.kernel().query_video_memory_info(s.pid(), handle, group == 0 ? dxgk::MemorySegmentGroup::Local : dxgk::MemorySegmentGroup::NonLocal, &info);
  if (st != status::kSuccess) return st;
  if (!put64(call, arg, 16, info.budget) || !put64(call, arg, 24, info.current_usage) || !put64(call, arg, 32, info.current_reservation) ||
      !put64(call, arg, 40, info.available_for_reservation))
    return kStatusAccessViolation;
  return status::kSuccess;
}

// --- Devices ---------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t create_device(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_create_device)) return load_failure(arg);
  const dxgk::Handle adapter = b.u32(0);      // (the union with the legacy pointer: the handle is its low half)
  if (adapter == 0) return kStatusInvalidParameter;
  Locked g(s);
  uint32_t index = 0;
  if (!adapter_index(s, adapter, &index)) return status::kInvalidHandle;
  dxgk::Handle device = 0;
  uint32_t st = s.kernel().create_device(s.pid(), adapter, &device);
  if (st != status::kSuccess) return st;
  DeviceRecord rec;
  rec.handle = device;
  rec.adapter_index = index;
  if (!s.state().devices.push_back(rec)) {
    s.kernel().destroy_device(s.pid(), device);
    return status::kNoMemory;
  }
  // No DMA buffers at device level (the contexts have them), as on a WDDM 2 adapter.
  bool wrote = put32(call, arg, 12, device);
  for (uint64_t off : {uint64_t{16}, uint64_t{32}, uint64_t{48}}) wrote = wrote && put64(call, arg, off, 0);
  for (uint64_t off : {uint64_t{24}, uint64_t{40}, uint64_t{56}}) wrote = wrote && put32(call, arg, off, 0);
  if (!wrote) {
    forget_device(s, call, device);
    s.kernel().destroy_device(s.pid(), device);
    return kStatusAccessViolation;
  }
  return status::kSuccess;
}

uint32_t destroy_device(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_handle)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  if (device == 0) return kStatusInvalidParameter;
  Locked g(s);
  if (s.state().find_device(device)) forget_device(s, call, device);
  return s.kernel().destroy_device(s.pid(), device);
}

// D3DKMT_DEVICESTATE_TYPE and D3DKMT_DEVICEEXECUTION_STATE.
constexpr uint32_t kStateExecution = 1, kStateReset = 3;
constexpr uint32_t kExecActive = 1, kExecHung = 3, kExecOutOfMemory = 5, kExecPageFault = 7;

uint32_t get_device_state(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_get_device_state)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t type = b.u32(4);
  if (device == 0) return kStatusInvalidParameter;
  dxgk::DeviceState state;
  const uint32_t st = s.kernel().get_device_state(s.pid(), device, &state);
  if (st != status::kSuccess) return st;
  uint32_t value = 0;
  switch (type) {
    case kStateExecution:
      switch (state.execution) {
        case dxgk::DeviceExecState::Running: value = kExecActive; break;
        case dxgk::DeviceExecState::ErrorOutOfMemory: value = kExecOutOfMemory; break;
        case dxgk::DeviceExecState::Hung: value = kExecHung; break;
        case dxgk::DeviceExecState::PageFault: value = kExecPageFault; break;
      }
      break;
    case kStateReset:
      value = state.reset_count ? 1u : 0u;      // TdrReset
      break;
    default:
      return kStatusNotSupported;      // (present statistics and the present queue state: no display scheduler to report)
  }
  return put32(call, arg, 8, value) ? status::kSuccess : kStatusAccessViolation;
}

// --- Display ---------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kFormatA8R8G8B8 = 21;      // D3DDDIFMT_A8R8G8B8
constexpr uint32_t kRefreshHz = 60;

uint32_t get_display_mode_list(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_get_display_mode_list)) return load_failure(arg);
  const dxgk::Handle adapter = b.u32(0);
  const uint32_t source = b.u32(4);
  const uint64_t list = b.u64(8);
  const uint32_t capacity = b.u32(16);
  if (adapter == 0 || source != 0) return kStatusInvalidParameter;
  uint32_t count = 0;
  uint32_t st = s.kernel().display_mode_count(s.pid(), adapter, &count);
  if (st != status::kSuccess) return st;
  if (list == 0) return put32(call, arg, 16, count) ? status::kSuccess : kStatusAccessViolation;
  if (capacity < count) {
    put32(call, arg, 16, count);
    return status::kBufferTooSmall;
  }
  for (uint32_t i = 0; i < count; ++i) {
    dxgk::DisplayModeInfo m;
    st = s.kernel().display_mode(s.pid(), adapter, i, &m);
    if (st != status::kSuccess) return st;
    // D3DKMT_DISPLAYMODE: Width, Height, Format, IntegerRefreshRate, ScanLineOrdering (progressive), DisplayOrientation (identity), DisplayFixedOutput, Flags.
    const uint32_t words[8] = {m.width, m.height, kFormatA8R8G8B8, kRefreshHz, 1, 1, 0, 0};
    const uint64_t entry = element_address(list, i, sizeof_display_mode);
    for (uint32_t w = 0; w < 8; ++w)
      if (!put32(call, entry, uint64_t{w} * 4, words[w])) return kStatusAccessViolation;
  }
  return put32(call, arg, 16, count) ? status::kSuccess : kStatusAccessViolation;
}

uint32_t set_display_mode(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_set_display_mode)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const dxgk::Handle primary = b.u32(4);
  if (device == 0 || primary == 0) return kStatusInvalidParameter;
  Locked g(s);
  DeviceRecord* rec = s.state().find_device(device);
  if (!rec) return status::kInvalidHandle;
  dxgk::AllocationInfo info;
  uint32_t st = s.kernel().query_allocation(s.pid(), primary, &info);
  if (st != status::kSuccess) return st;
  if (!(info.desc.flags & dxgk::kAllocPresent)) return kStatusInvalidParameter;
  // The display takes the mode of the primary allocation's size. The adapter handle the device was made from may be closed by now: use a handle of our own.
  dxgk::Handle adapter = 0;
  st = s.kernel().open_adapter_from_index(s.pid(), rec->adapter_index, &adapter);
  if (st != status::kSuccess) return st;
  uint32_t count = 0;
  st = s.kernel().display_mode_count(s.pid(), adapter, &count);
  if (st == status::kSuccess) {
    st = kStatusInvalidParameter;
    for (uint32_t i = 0; i < count; ++i) {
      dxgk::DisplayModeInfo m;
      if (s.kernel().display_mode(s.pid(), adapter, i, &m) == status::kSuccess && m.width == info.desc.width && m.height == info.desc.height) {
        st = s.kernel().set_display_mode(s.pid(), adapter, i);
        break;
      }
    }
  }
  s.kernel().close_adapter(s.pid(), adapter);
  return st;
}

uint32_t wait_for_vertical_blank(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_wait_vblank)) return load_failure(arg);
  const dxgk::Handle adapter = b.u32(0);
  const dxgk::Handle device = b.u32(4);
  if ((adapter == 0 && device == 0) || b.u32(8) != 0) return kStatusInvalidParameter;
  uint32_t count = 0;
  if (adapter != 0) {
    const uint32_t st = s.kernel().display_mode_count(s.pid(), adapter, &count);
    if (st != status::kSuccess) return st;
  } else {
    dxgk::DeviceState state;
    const uint32_t st = s.kernel().get_device_state(s.pid(), device, &state);
    if (st != status::kSuccess) return st;
  }
  // The display refreshes at a fixed 60 Hz here: the vertical blank is the next multiple of the frame time on the monotonic clock.
  constexpr uint64_t kFrameNs = 1000000000ull / kRefreshHz;
  IProcessHost& host = call.process().host();
  host.sleep_ns(kFrameNs - host.monotonic_ns() % kFrameNs);
  return status::kSuccess;
}

uint32_t set_queued_limit(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_set_queued_limit)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t type = b.u32(4);
  if (device == 0) return kStatusInvalidParameter;
  Locked g(s);
  DeviceRecord* rec = s.state().find_device(device);
  if (!rec) return status::kInvalidHandle;
  if (type == 1) {      // D3DKMT_SET_QUEUEDLIMIT_PRESENT
    const uint32_t limit = b.u32(8);
    if (limit == 0 || limit > 16) return kStatusInvalidParameter;
    rec->queued_present_limit = limit;
    return status::kSuccess;
  }
  if (type == 2) return put32(call, arg, 8, rec->queued_present_limit) ? status::kSuccess : kStatusAccessViolation;      // D3DKMT_GET_QUEUEDLIMIT_PRESENT
  return kStatusInvalidParameter;
}

uint32_t wait_for_idle(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_handle)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  if (device == 0) return kStatusInvalidParameter;
  Process& p = call.process();
  for (uint64_t nap = 20000;;) {
    bool idle = true;
    {
      Locked g(s);
      if (!s.state().find_device(device)) return status::kInvalidHandle;
      for (ContextRecord& c : s.state().contexts.view()) {
        if (c.device != device) continue;
        bool done = true;
        const uint32_t st = s.kernel().context_idle(s.pid(), c.context, &done);
        if (st != status::kSuccess) return st;
        idle = idle && done;
      }
    }
    if (idle) return status::kSuccess;
    IProcessRuntime* runtime = p.runtime();
    if (p.exited() || (runtime && runtime->interrupted())) return nt::kThreadIsTerminating;
    p.host().sleep_ns(nap);
    if (nap < 1000000) nap *= 2;
  }
}

uint32_t trim_process_commitment(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_trim_process_commitment)) return load_failure(arg);
  const uint64_t process = b.u64(0);
  if (process != 0 && process != ~uint64_t{0}) return kStatusNotSupported;
  Locked g(s);
  uint32_t st = status::kSuccess;
  for (DeviceRecord& d : s.state().devices.view()) {
    const uint32_t t = s.kernel().trim(s.pid(), d.handle);
    if (t != status::kSuccess) st = t;
  }
  return st;
}

// --- Source ownership --------------------------------------------------------------------------------------------------------------------------------------------

uint32_t set_vidpn_source_owner(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_set_vidpn_source_owner)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint64_t types_ptr = b.u64(8), sources_ptr = b.u64(16);
  const uint32_t count = b.u32(24);
  if (device == 0 || count > 1) return kStatusInvalidParameter;      // (one source per adapter)
  Locked g(s);
  DeviceRecord* rec = s.state().find_device(device);
  if (!rec) return status::kInvalidHandle;
  if (count == 0) {
    // No list: the device gives up everything it owns.
    release_source_owner(s.pid(), device);
    return status::kSuccess;
  }
  uint32_t type = 0, source = 0;
  if (types_ptr == 0 || sources_ptr == 0) return kStatusInvalidParameter;
  if (!call.read_u32(types_ptr, &type) || !call.read_u32(sources_ptr, &source)) return kStatusAccessViolation;
  if (source != 0) return kStatusInvalidParameter;
  return set_source_owner(rec->adapter_index, s.pid(), device, type);
}

uint32_t check_vidpn_exclusive_ownership(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_check_vidpn_exclusive_ownership)) return load_failure(arg);
  const dxgk::Handle adapter = b.u32(0);
  if (adapter == 0 || b.u32(4) != 0) return kStatusInvalidParameter;
  Locked g(s);
  uint32_t index = 0;
  if (!adapter_index(s, adapter, &index)) return status::kInvalidHandle;
  return source_is_exclusive(index) ? kStatusPresentOccluded : status::kSuccess;
}

uint32_t evict(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_evict)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t count = b.u32(4);
  const uint64_t list = b.u64(8);
  if (device == 0 || count == 0 || count > kMaxAllocationsPerCall || list == 0) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  if (!read_u32_array(call, list, count, &handles)) return kStatusAccessViolation;
  Locked g(s);
  if (!s.state().find_device(device)) return status::kInvalidHandle;
  for (dxgk::Handle h : handles.view()) {
    AllocationRecord* rec = s.state().find_allocation(h);
    if (!rec || rec->device != device) return status::kInvalidHandle;
  }
  return s.kernel().evict(s.pid(), device, ArrayView<const dxgk::Handle>(handles.view()));
}

}  // namespace

void register_adapters() {
  using N = WindowsSyscallNumber;
  add<open_from_luid>(N::NtGdiDdDDIOpenAdapterFromLuid);
  add<open_from_device_name>(N::NtGdiDdDDIOpenAdapterFromDeviceName);
  add<close_adapter>(N::NtGdiDdDDICloseAdapter);
  add<enum_adapters>(N::NtGdiDdDDIEnumAdapters);
  add<enum_adapters2>(N::NtGdiDdDDIEnumAdapters2);
  add<dxgk_enum_adapters3>(N::NtDxgkEnumAdapters3);
  add<query_adapter_info>(N::NtGdiDdDDIQueryAdapterInfo);
  add<query_video_memory_info>(N::NtGdiDdDDIQueryVideoMemoryInfo);
  add<create_device>(N::NtGdiDdDDICreateDevice);
  add<destroy_device>(N::NtGdiDdDDIDestroyDevice);
  add<get_device_state>(N::NtGdiDdDDIGetDeviceState);
  add<get_display_mode_list>(N::NtGdiDdDDIGetDisplayModeList);
  add<set_display_mode>(N::NtGdiDdDDISetDisplayMode);
  add<wait_for_vertical_blank>(N::NtGdiDdDDIWaitForVerticalBlankEvent);
  add<set_queued_limit>(N::NtGdiDdDDISetQueuedLimit);
  add<wait_for_idle>(N::NtGdiDdDDIWaitForIdle);
  add<trim_process_commitment>(N::NtGdiDdDDITrimProcessCommitment);
  add<set_vidpn_source_owner>(N::NtGdiDdDDISetVidPnSourceOwner);
  add<check_vidpn_exclusive_ownership>(N::NtGdiDdDDICheckVidPnExclusiveOwnership);
  add<evict>(N::NtGdiDdDDIEvict);
}

}  // namespace process::win32k::dxg
