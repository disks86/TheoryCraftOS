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
#include "Process/Win32k/Win32kUserMisc.hpp"

// Monitors and the display: the one monitor (sized from the screen surface of the GDI module), its DPI, the display devices and modes, ChangeDisplaySettings (which resizes the
// screen surface), the display configuration queries and the DPI awareness of processes and threads.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kMonitorHandle = 0x00010001;
constexpr uint32_t kDevModeSize = 220, kDisplayDeviceSize = 840;
constexpr uint32_t kDmPosition = 0x20, kDmDisplayOrientation = 0x80, kDmBitsPerPel = 0x40000, kDmPelsWidth = 0x80000, kDmPelsHeight = 0x100000, kDmDisplayFlags = 0x200000,
                   kDmDisplayFrequency = 0x400000;
constexpr uint32_t kEnumCurrent = 0xFFFFFFFFu, kEnumRegistry = 0xFFFFFFFEu;
constexpr int32_t kDispChangeSuccessful = 0, kDispChangeFailed = -1, kDispChangeBadMode = -2, kDispChangeBadFlags = -4, kDispChangeBadParam = -5;
constexpr uint32_t kCdsTest = 2, kCdsUpdateRegistry = 1, kCdsValid = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | 0x200 | 0x10000000u | 0x20000000u | 0x40000000u;
constexpr uint32_t kStatusInvalidParameter = 0xC000000Du, kStatusUnsuccessful = 0xC0000001u;
constexpr uint32_t kQdcAllPaths = 1, kQdcOnlyActive = 2, kQdcDatabase = 4;
constexpr uint32_t kAdapterLuidLow = 1;
constexpr uint32_t kMaxModes = 32;

// The monitors the kernel's desktop has (set_monitors: their rectangles and work areas on the screen, the primary first); none set: one monitor, the screen.
constexpr uint32_t kMaxMonitors = 8;
struct MonitorState {
  Rect work;
  bool work_set = false;
  uint32_t registry_width = 0, registry_height = 0;
  Rect rects[kMaxMonitors], works[kMaxMonitors];
  uint32_t count = 0;
};
MonitorState g_monitor;

Rect screen_rect() { return Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())}; }

uint32_t monitor_count() { return g_monitor.count ? g_monitor.count : 1; }
Rect bounds_of(uint32_t index) { return g_monitor.count ? g_monitor.rects[index] : screen_rect(); }
uint32_t index_of(uint32_t handle) { return handle - kMonitorHandle; }      // (is_monitor first)
int64_t overlap(const Rect& a, const Rect& b) {
  const int64_t w = int64_t(a.right < b.right ? a.right : b.right) - (a.left > b.left ? a.left : b.left);
  const int64_t h = int64_t(a.bottom < b.bottom ? a.bottom : b.bottom) - (a.top > b.top ? a.top : b.top);
  return w > 0 && h > 0 ? w * h : 0;
}
// How far a point lies outside a rectangle (squared; 0 inside).
int64_t distance2(const Rect& r, int32_t x, int32_t y) {
  const int64_t dx = x < r.left ? int64_t(r.left) - x : x >= r.right ? int64_t(x) - r.right + 1 : 0;
  const int64_t dy = y < r.top ? int64_t(r.top) - y : y >= r.bottom ? int64_t(y) - r.bottom + 1 : 0;
  return dx * dx + dy * dy;
}

void put32(uint8_t* buf, uint32_t offset, uint32_t value) { for (uint32_t i = 0; i < 4; ++i) buf[offset + i] = uint8_t(value >> (8 * i)); }
void put16(uint8_t* buf, uint32_t offset, uint32_t value) { buf[offset] = uint8_t(value); buf[offset + 1] = uint8_t(value >> 8); }
void put_text(uint8_t* buf, uint32_t offset, uint32_t capacity_chars, const char* text) {
  uint32_t i = 0;
  for (; text[i] && i + 1 < capacity_chars; ++i) put16(buf, offset + 2 * i, uint8_t(text[i]));
  put16(buf, offset + 2 * i, 0);
}
uint32_t get32(const uint8_t* buf, uint32_t offset) { return uint32_t(buf[offset]) | uint32_t(buf[offset + 1]) << 8 | uint32_t(buf[offset + 2]) << 16 | uint32_t(buf[offset + 3]) << 24; }

constexpr char kAdapterName[] = "\\\\.\\DISPLAY1";

// Is the UNICODE_STRING at `address` (null = none) the display device "\\.\DISPLAY1"? `*given` tells whether there was a name.
bool device_is_ours(Call& c, uint64_t address, bool* given) {
  *given = address != 0;
  if (address == 0) return true;
  WideString name(*c.allocator());
  if (!c.read_unicode_string(address, &name)) return false;
  if (name.empty()) { *given = false; return true; }
  WideString ours(*c.allocator());
  ours.append_ascii(kAdapterName);
  return name.equals(ours.view(), true);
}

struct Mode { uint32_t width, height; };
uint32_t mode_list(Mode* out) {
  static const Mode standard[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800}, {1280, 960}, {1280, 1024}, {1360, 768}, {1366, 768},
                                  {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {3840, 2160}};
  uint32_t n = 0;
  const Mode current{gdi::screen_width(), gdi::screen_height()};
  bool added_current = false;
  for (const Mode& m : standard) {
    if (!added_current && (current.width < m.width || (current.width == m.width && current.height <= m.height))) {
      if (current.width != m.width || current.height != m.height) out[n++] = current;
      added_current = true;
    }
    out[n++] = m;
  }
  if (!added_current) out[n++] = current;
  return n;
}

void write_devmode(uint8_t* out, uint32_t width, uint32_t height) {
  for (uint32_t i = 0; i < kDevModeSize; ++i) out[i] = 0;
  put_text(out, 0, 32, kAdapterName);
  put16(out, 64, 0x0401);      // dmSpecVersion
  put16(out, 66, 0x0401);      // dmDriverVersion
  put16(out, 68, kDevModeSize);
  put32(out, 72, kDmPosition | kDmDisplayOrientation | kDmBitsPerPel | kDmPelsWidth | kDmPelsHeight | kDmDisplayFlags | kDmDisplayFrequency);
  put32(out, 168, 32);
  put32(out, 172, width);
  put32(out, 176, height);
  put32(out, 184, 60);
}

uint64_t svc_enum_display_settings(Call& c) {
  bool given = false;
  if (!device_is_ours(c, c.arg(0), &given)) return c.fail(c.arg(0) ? kErrorNoAccess : kErrorInvalidParameter);
  const uint32_t mode = c.arg32(1);
  uint32_t width = 0, height = 0;
  if (mode == kEnumCurrent) { width = gdi::screen_width(); height = gdi::screen_height(); }
  else if (mode == kEnumRegistry) { width = g_monitor.registry_width ? g_monitor.registry_width : gdi::screen_width(); height = g_monitor.registry_height ? g_monitor.registry_height : gdi::screen_height(); }
  else {
    Mode modes[kMaxModes];
    const uint32_t n = mode_list(modes);
    if (mode >= n) return 0;
    width = modes[mode].width;
    height = modes[mode].height;
  }
  uint8_t out[kDevModeSize];
  write_devmode(out, width, height);
  return c.write(c.arg(2), ConstByteView(out, kDevModeSize)) ? 1 : c.fail(kErrorNoAccess);
}

uint64_t svc_change_display_settings(Call& c) {
  const uint32_t flags = c.arg32(3);
  if ((flags & ~kCdsValid) != 0) return uint32_t(kDispChangeBadFlags);
  bool given = false;
  if (!device_is_ours(c, c.arg(0), &given)) return uint32_t(kDispChangeBadParam);
  if (g_monitor.registry_width == 0) { g_monitor.registry_width = gdi::screen_width(); g_monitor.registry_height = gdi::screen_height(); }
  uint32_t width = g_monitor.registry_width, height = g_monitor.registry_height;
  if (c.arg(1) != 0) {
    uint8_t in[kDevModeSize] = {};
    uint16_t size = 0;
    if (!c.read_u16(c.arg(1) + 68, &size)) return uint32_t(kDispChangeBadParam);
    if (size < 72 + 4 || size > kDevModeSize || !c.read(c.arg(1), ByteView(in, size))) return uint32_t(kDispChangeBadMode);
    const uint32_t fields = get32(in, 72);
    width = gdi::screen_width();
    height = gdi::screen_height();
    if ((fields & kDmPelsWidth) && size >= 176) width = get32(in, 172);
    if ((fields & kDmPelsHeight) && size >= 180) height = get32(in, 176);
    if ((fields & kDmBitsPerPel) && size >= 172 && get32(in, 168) != 32) return uint32_t(kDispChangeBadMode);
    if (width < 16 || height < 16 || width > 16384 || height > 16384) return uint32_t(kDispChangeBadMode);
  }
  if (flags & kCdsTest) return uint32_t(kDispChangeSuccessful);
  if (width != gdi::screen_width() || height != gdi::screen_height()) {
    if (!gdi::set_screen_size(width, height)) return uint32_t(kDispChangeFailed);
    g_monitor.work_set = false;
  }
  if ((flags & kCdsUpdateRegistry) && c.arg(1) != 0) { g_monitor.registry_width = width; g_monitor.registry_height = height; }
  return uint32_t(kDispChangeSuccessful);
}

uint64_t svc_enum_display_devices(Call& c) {
  bool given = false;
  if (!device_is_ours(c, c.arg(0), &given)) return kStatusUnsuccessful;
  const uint32_t index = c.arg32(1);
  uint32_t cb = 0;
  if (!c.read_u32(c.arg(2), &cb)) return kStatusUnsuccessful;
  if (cb != kDisplayDeviceSize) return kStatusUnsuccessful;
  if (index != 0) return kStatusUnsuccessful;      // one adapter, one monitor on it
  uint8_t out[kDisplayDeviceSize] = {};
  put32(out, 0, kDisplayDeviceSize);
  if (!given) {
    put_text(out, 4, 32, kAdapterName);
    put_text(out, 68, 128, "TheoryCraft Display Adapter");
    put32(out, 324, 1 | 4);      // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE
    put_text(out, 328, 128, "PCI\\VEN_0000&DEV_0000");
    put_text(out, 584, 128, "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\{00000000-0000-0000-0000-000000000000}\\0000");
  } else {
    put_text(out, 4, 32, "\\\\.\\DISPLAY1\\Monitor0");
    put_text(out, 68, 128, "Generic PnP Monitor");
    put32(out, 324, 1);
    put_text(out, 328, 128, "MONITOR\\Default_Monitor\\{00000000-0000-0000-0000-000000000000}\\0000");
    put_text(out, 584, 128, "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\{4d36e96e-e325-11ce-bfc1-08002be10318}\\0000");
  }
  return c.write(c.arg(2), ConstByteView(out, kDisplayDeviceSize)) ? 0 : kStatusUnsuccessful;
}

// --- display configuration
uint64_t svc_get_display_config_buffer_sizes(Call& c) {
  const uint32_t flags = c.arg32(0);
  if (flags != kQdcAllPaths && flags != kQdcOnlyActive && flags != kQdcDatabase && flags != (kQdcOnlyActive | kQdcDatabase)) return kErrorInvalidParameter;
  if (c.arg(1) == 0 || c.arg(2) == 0) return kErrorInvalidParameter;
  if (!c.write_u32(c.arg(1), 1) || !c.write_u32(c.arg(2), 2)) return kErrorInvalidParameter;
  return 0;
}

uint64_t svc_query_display_config(Call& c) {
  const uint32_t flags = c.arg32(0);
  if (flags != kQdcAllPaths && flags != kQdcOnlyActive && flags != kQdcDatabase && flags != (kQdcOnlyActive | kQdcDatabase)) return kErrorInvalidParameter;
  uint32_t paths = 0, modes = 0;
  if (c.arg(1) == 0 || c.arg(3) == 0 || !c.read_u32(c.arg(1), &paths) || !c.read_u32(c.arg(3), &modes)) return kErrorInvalidParameter;
  if (c.arg(2) == 0 || c.arg(4) == 0) return kErrorInvalidParameter;
  if (((flags & kQdcDatabase) != 0) != (c.arg(5) != 0)) return kErrorInvalidParameter;      // the topology goes with QDC_DATABASE_CURRENT and only with it
  if (paths == 0 || modes == 0) return kErrorInvalidParameter;
  if (modes < 2) return kErrorInsufficientBuffer;      // (one path, two modes; the counts are left as they were: GetDisplayConfigBufferSizes tells them)
  const uint32_t w = gdi::screen_width(), h = gdi::screen_height();
  uint8_t path[72] = {};
  put32(path, 0, kAdapterLuidLow);        // source: adapterId.LowPart (HighPart 0), id 0, modeInfoIdx 0
  put32(path, 12, 0);
  put32(path, 16, 1);                      // DISPLAYCONFIG_SOURCE_IN_USE
  put32(path, 20, kAdapterLuidLow);       // target: adapter, id 0, modeInfoIdx 1
  put32(path, 32, 1);
  put32(path, 36, 0xFFFFFFFFu);            // DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER
  put32(path, 40, 1);                      // rotation identity
  put32(path, 44, 1);                      // scaling identity
  put32(path, 48, 60000); put32(path, 52, 1000);      // refresh rate 60/1
  put32(path, 56, 1);                      // progressive
  put32(path, 60, 1);                      // targetAvailable
  put32(path, 64, 1);                      // DISPLAYCONFIG_TARGET_IN_USE
  put32(path, 68, 1);                      // DISPLAYCONFIG_PATH_ACTIVE
  uint8_t mode[128] = {};
  put32(mode, 0, 1);                       // DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE
  put32(mode, 8, kAdapterLuidLow);
  put32(mode, 16, w); put32(mode, 20, h);
  put32(mode, 24, 4);                      // DISPLAYCONFIG_PIXELFORMAT_32BPP
  put32(mode, 64 + 0, 2);                  // DISPLAYCONFIG_MODE_INFO_TYPE_TARGET
  put32(mode, 64 + 8, kAdapterLuidLow);
  const uint64_t pixel_rate = uint64_t{w} * h * 60;
  put32(mode, 64 + 16, uint32_t(pixel_rate)); put32(mode, 64 + 20, uint32_t(pixel_rate >> 32));
  put32(mode, 64 + 24, h * 60); put32(mode, 64 + 28, 1);      // hSyncFreq
  put32(mode, 64 + 32, 60); put32(mode, 64 + 36, 1);          // vSyncFreq
  put32(mode, 64 + 40, w); put32(mode, 64 + 44, h);           // activeSize
  put32(mode, 64 + 48, w); put32(mode, 64 + 52, h);           // totalSize
  put32(mode, 64 + 56, 255);                                   // videoStandard: other
  put32(mode, 64 + 60, 1);                                     // scanLineOrdering: progressive
  if (!c.write(c.arg(2), ConstByteView(path, 72)) || !c.write(c.arg(4), ConstByteView(mode, 128))) return kErrorInvalidParameter;
  c.write_u32(c.arg(1), 1);
  c.write_u32(c.arg(3), 2);
  if (c.arg(5) != 0) c.write_u32(c.arg(5), 1);      // DISPLAYCONFIG_TOPOLOGY_INTERNAL
  return 0;
}

// The packet's header: type, size, adapter LUID, id. A header that cannot be read or that does not hold itself is STATUS_UNSUCCESSFUL (ERROR_GEN_FAILURE); an unknown type or
// a size that is not the type's is STATUS_INVALID_PARAMETER; an adapter or source that is not the screen's is STATUS_UNSUCCESSFUL.
uint64_t svc_display_config_get_device_info(Call& c) {
  uint8_t head[20];
  if (c.arg(0) == 0 || !c.read(c.arg(0), ByteView(head, 20))) return kStatusUnsuccessful;
  const uint32_t type = get32(head, 0), size = get32(head, 4), luid_low = get32(head, 8), id = get32(head, 16);
  if (size < 20) return kStatusUnsuccessful;
  uint32_t need = 0;
  switch (type) {
    case 1: need = 84; break;       // DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME
    case 2: need = 420; break;      // GET_TARGET_NAME
    case 3: need = 80; break;       // GET_TARGET_PREFERRED_MODE
    case 4: need = 276; break;      // GET_ADAPTER_NAME
    case 9: need = 32; break;       // GET_ADVANCED_COLOR_INFO
    default: return kStatusInvalidParameter;
  }
  if (size < need) return kStatusInvalidParameter;
  if (luid_low != kAdapterLuidLow || get32(head, 12) != 0) return kStatusUnsuccessful;
  if (type != 4 && id != 0) return kStatusUnsuccessful;
  uint8_t out[512] = {};
  for (uint32_t i = 0; i < 20; ++i) out[i] = head[i];
  switch (type) {
    case 1: put_text(out, 20, 32, kAdapterName); break;
    case 2:
      put32(out, 24, 0xFFFFFFFFu);
      put_text(out, 36, 64, "Generic PnP Monitor");
      put_text(out, 164, 128, "\\\\?\\DISPLAY#Default_Monitor#0&0&0#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
      break;
    case 3: {      // the mode the screen runs at is the preferred one: width, height and the signal of the target mode QueryDisplayConfig gives
      const uint32_t w = gdi::screen_width(), h = gdi::screen_height();
      const uint64_t pixel_rate = uint64_t{w} * h * 60;
      put32(out, 20, w); put32(out, 24, h);
      put32(out, 32, uint32_t(pixel_rate)); put32(out, 36, uint32_t(pixel_rate >> 32));
      put32(out, 40, h * 60); put32(out, 44, 1);      // hSyncFreq
      put32(out, 48, 60); put32(out, 52, 1);          // vSyncFreq
      put32(out, 56, w); put32(out, 60, h);           // activeSize
      put32(out, 64, w); put32(out, 68, h);           // totalSize
      put32(out, 72, 255);                            // videoStandard: other
      put32(out, 76, 1);                              // scanLineOrdering: progressive
      break;
    }
    case 4: put_text(out, 20, 128, "\\\\?\\TheoryCraft#Display#0"); break;
    case 9: put32(out, 20, 0); put32(out, 24, 0); put32(out, 28, 8); break;      // no advanced colour (HDR) support; RGB, 8 bits per channel
  }
  return c.write(c.arg(0), ConstByteView(out, need)) ? 0 : kStatusInvalidParameter;
}

// --- DPI
uint64_t svc_get_dpi_for_monitor(Call& c) {
  if (!is_monitor(c.arg32(0)) || (c.arg(0) >> 32) != 0) return c.fail(kErrorInvalidMonitorHandle);
  if (c.arg32(1) > 2) return c.fail(kErrorInvalidParameter);
  const uint32_t dpi = dpi_for_monitor(c.arg32(0));
  if (!c.write_u32(c.arg(2), dpi) || !c.write_u32(c.arg(3), dpi)) return c.fail(kErrorNoAccess);
  return 1;
}

uint64_t svc_get_dpi_for_current_process(Call& c) { return thread_dpi(c); }

bool is_current_process_handle(uint64_t handle) { return handle == ~uint64_t{0} || handle == 0; }

uint64_t svc_get_system_dpi_for_process(Call& c) { return is_current_process_handle(c.arg(0)) ? thread_dpi(c) : system_dpi(); }

uint32_t normalize_context(uint32_t context) {
  if ((context & 0x0F) == 1) context = (context & ~0x1FF00u) | (system_dpi() << 8);      // system aware: the dpi of the system
  return context;
}

bool valid_context(uint32_t context) {
  const uint32_t awareness = context & 0x0F, version = (context >> 4) & 0x0F, flags = context & 0xFFFE0000u;
  if (awareness > 2 || version < 1 || version > 2 || (version == 2 && awareness != 2)) return false;
  if ((flags & ~0xC0000000u) != 0) return false;
  if ((flags & 0x40000000u) != 0 && awareness != 0) return false;      // GDI scaling is a flavour of unaware
  return true;
}

uint64_t svc_get_process_dpi_awareness_context(Call& c) {
  ProcessRecord* p = is_current_process_handle(c.arg(0)) ? find_process_record(c.pid()) : nullptr;
  return p && p->dpi_context_set ? p->dpi_context : (1u << 4 | 96u << 8);
}

uint64_t svc_set_process_dpi_awareness_context(Call& c) {
  const uint32_t context = c.arg32(0);
  if (!valid_context(context)) return c.fail(kErrorInvalidParameter);
  ProcessRecord* p = process_record(c.pid());
  if (!p) return 0;
  if (p->dpi_context_set) return c.fail(kErrorAccessDenied);      // the awareness of a process is set once
  p->dpi_context = normalize_context(context & ~0x80000000u);
  p->dpi_context_set = true;
  return 1;
}

uint64_t svc_point_identity(Call& c) {
  if (!user::is_window(c.arg32(0))) return c.fail(kErrorInvalidWindowHandle);
  int32_t pt[2];
  if (!c.read_array(c.arg(1), pt, 2)) return c.fail(kErrorNoAccess);
      // (one monitor, and every thread sees the system DPI of that monitor: logical and physical coordinates are the same)
  return 1;
}

}  // namespace

uint32_t monitor_handle() { return kMonitorHandle; }
bool is_monitor(uint32_t handle) { return handle >= kMonitorHandle && handle - kMonitorHandle < monitor_count(); }
Rect primary_monitor_rect() { return bounds_of(0); }
Rect virtual_screen_rect() {
  Rect u = bounds_of(0);
  for (uint32_t i = 1; i < monitor_count(); ++i) {
    const Rect r = bounds_of(i);
    if (r.left < u.left) u.left = r.left;
    if (r.top < u.top) u.top = r.top;
    if (r.right > u.right) u.right = r.right;
    if (r.bottom > u.bottom) u.bottom = r.bottom;
  }
  return u;
}

bool set_monitors(ArrayView<const Rect> rects, ArrayView<const Rect> works) {
  if (rects.size() != works.size() || rects.size() > kMaxMonitors) return false;
  for (uint64_t i = 0; i < rects.size(); ++i)
    if (rects[i].left >= rects[i].right || rects[i].top >= rects[i].bottom || overlap(rects[i], works[i]) == 0) return false;
  g_monitor.count = uint32_t(rects.size());
  for (uint64_t i = 0; i < rects.size(); ++i) {
    g_monitor.rects[i] = rects[i];
    g_monitor.works[i] = works[i];
  }
  g_monitor.work_set = false;
  return true;
}

bool set_monitor_work_area(const Rect& rect) {
  const Rect screen = bounds_of(0);
  Rect r{rect.left < screen.left ? screen.left : rect.left, rect.top < screen.top ? screen.top : rect.top, rect.right > screen.right ? screen.right : rect.right,
         rect.bottom > screen.bottom ? screen.bottom : rect.bottom};
  if (r.left >= r.right || r.top >= r.bottom) return false;
  if (g_monitor.count) {      // (the primary monitor's)
    g_monitor.works[0] = r;
    return true;
  }
  g_monitor.work = r;
  g_monitor.work_set = true;
  return true;
}

// The monitor a rectangle overlaps most (a point: the one it is on); none: by `flags` nothing (MONITOR_DEFAULTTONULL 0), the primary (1) or the nearest (2).
uint32_t monitor_from_rect(const Rect& rect, uint32_t flags) {
  const bool point = rect.left >= rect.right || rect.top >= rect.bottom;
  int64_t best = 0;
  uint32_t found = 0;
  for (uint32_t i = 0; i < monitor_count(); ++i) {
    const Rect m = bounds_of(i);
    const int64_t a = point ? (distance2(m, rect.left, rect.top) == 0 ? 1 : 0) : overlap(rect, m);
    if (a > best) { best = a; found = kMonitorHandle + i; }
  }
  if (found) return found;
  if (flags == 1) return kMonitorHandle;
  if (flags != 2) return 0;
  const int32_t cx = point ? rect.left : rect.left + (rect.right - rect.left) / 2, cy = point ? rect.top : rect.top + (rect.bottom - rect.top) / 2;
  int64_t nearest = -1;
  for (uint32_t i = 0; i < monitor_count(); ++i) {
    const int64_t d = distance2(bounds_of(i), cx, cy);
    if (nearest < 0 || d < nearest) { nearest = d; found = kMonitorHandle + i; }
  }
  return found;
}

uint32_t monitor_from_point(int32_t x, int32_t y, uint32_t flags) { return monitor_from_rect(Rect{x, y, x, y}, flags); }

uint32_t dpi_for_monitor(uint32_t monitor) { return is_monitor(monitor) ? system_dpi() : 0; }

Rect monitor_rect(uint32_t monitor, bool work_area) {
  if (!is_monitor(monitor)) return Rect{};
  if (g_monitor.count) return work_area ? g_monitor.works[index_of(monitor)] : g_monitor.rects[index_of(monitor)];
  if (work_area && g_monitor.work_set) {
    // a screen that changed size since the work area was set: the work area is the whole screen again
    const Rect screen = screen_rect();
    if (g_monitor.work.right <= screen.right && g_monitor.work.bottom <= screen.bottom) return g_monitor.work;
    g_monitor.work_set = false;
  }
  return screen_rect();
}

bool get_monitor_info(Call& c, uint32_t monitor, uint64_t info) {
  if (!is_monitor(monitor)) { c.set_last_error(kErrorInvalidMonitorHandle); return false; }
  uint32_t size = 0;
  if (!c.read_u32(info, &size)) { c.set_last_error(kErrorNoAccess); return false; }
  if (size != 40 && size != 104) { c.set_last_error(kErrorInvalidParameter); return false; }
  uint8_t out[104] = {};
  put32(out, 0, size);
  const Rect m = monitor_rect(monitor, false), w = monitor_rect(monitor, true);
  const int32_t v[8] = {m.left, m.top, m.right, m.bottom, w.left, w.top, w.right, w.bottom};
  for (uint32_t i = 0; i < 8; ++i) put32(out, 4 + 4 * i, uint32_t(v[i]));
  put32(out, 36, index_of(monitor) == 0 ? 1 : 0);      // MONITORINFOF_PRIMARY
  if (size == 104) {
    char name[] = "\\\\.\\DISPLAY1";      // \\.\DISPLAY<n>
    name[sizeof name - 2] = char('1' + index_of(monitor));
    put_text(out, 40, 32, name);
  }
  if (!c.write(info, ConstByteView(out, size))) { c.set_last_error(kErrorNoAccess); return false; }
  return true;
}

uint32_t set_thread_dpi_context(Call& c, uint32_t context) {
  ThreadRecord* t = thread_record(c.tid(), c.pid());
  if (!t) return 0;
  uint32_t previous = t->dpi_context;
  if (previous == 0) {
    ProcessRecord* p = process_record(c.pid());
    previous = (p && p->dpi_context_set ? p->dpi_context : (1u << 4 | 96u << 8)) | 0x80000000u;      // the process's context, marked as such
  }
  if (context == 0) { t->dpi_context = 0; return previous; }
  if (!valid_context(context)) { c.set_last_error(kErrorInvalidParameter); return 0; }
  // a context marked as the process's (what an earlier call answered for a thread with none) makes the thread follow the process again
  t->dpi_context = (context & 0x80000000u) != 0 ? 0 : normalize_context(context);
  return previous;
}

void install_monitors() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserEnumDisplaySettings, svc_enum_display_settings);
  register_handler(N::NtUserChangeDisplaySettings, svc_change_display_settings);
  register_handler(N::NtUserEnumDisplayDevices, svc_enum_display_devices);
  register_handler(N::NtUserGetDisplayConfigBufferSizes, svc_get_display_config_buffer_sizes);
  register_handler(N::NtUserQueryDisplayConfig, svc_query_display_config);
  register_handler(N::NtUserDisplayConfigGetDeviceInfo, svc_display_config_get_device_info);
  register_handler(N::NtUserGetDpiForMonitor, svc_get_dpi_for_monitor);
  register_handler(N::NtUserGetDpiForCurrentProcess, svc_get_dpi_for_current_process);
  register_handler(N::NtUserGetSystemDpiForProcess, svc_get_system_dpi_for_process);
  register_handler(N::NtUserGetProcessDpiAwarenessContext, svc_get_process_dpi_awareness_context);
  register_handler(N::NtUserSetProcessDpiAwarenessContext, svc_set_process_dpi_awareness_context);
  register_handler(N::NtUserLogicalToPerMonitorDPIPhysicalPoint, svc_point_identity);
  register_handler(N::NtUserPerMonitorDPIPhysicalToLogicalPoint, svc_point_identity);
}

}  // namespace process::win32k::user_misc
