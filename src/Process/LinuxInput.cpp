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

// /dev/input for Linux programs (LinuxInput.hpp). Nothing here runs on its own: each open node is a client that looks at its pad's state when the
// program reads, polls or makes an ioctl, and turns the difference from what it last reported into events, as the Linux drivers would have sent them.
#include "Process/LinuxInput.hpp"
#include "FileSystem/IDirectory.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/Array.hpp"
#include "Process/ProcessFiles.hpp"
#include "Utility/ByteStream.hpp"

namespace process {

namespace {

using filesystem::DirectoryEntry;
using filesystem::Status;

constexpr int64_t kEnoent = 2, kEintr = 4, kEbadf = 9, kEagain = 11, kEfault = 14, kEbusy = 16, kEnodev = 19, kEnotdir = 20, kEisdir = 21,
                  kEinval = 22, kEmfile = 24, kEnospc = 28, kEnosys = 38, kEnomem = 12;
uint64_t error(int64_t value) { return uint64_t(-value); }

constexpr uint64_t kONonBlock = 0x800, kOAccMode = 3, kOWrite = 1, kORdOnly = 0;
constexpr uint64_t kPollNs = 4000000;      // how often a waiting read looks at the pad again (the service polls the pads every 4 ms)
constexpr uint32_t kSIfDir = 0040000, kSIfChr = 0020000;
constexpr uint32_t kInputMajor = 13, kJoystickMinor = 0, kEventMinor = 64;

const InputPadProvider* g_provider = nullptr;
const InputPadProvider* provider() { return __atomic_load_n(&g_provider, __ATOMIC_ACQUIRE); }

// --- the Linux codes --------------------------------------------------------------------------------------------------------------------------------

constexpr uint16_t kEvSyn = 0, kEvKey = 1, kEvAbs = 3, kEvFf = 0x15;
constexpr uint16_t kSynReport = 0;
constexpr uint16_t kAbsX = 0x00, kAbsY = 0x01, kAbsZ = 0x02, kAbsRx = 0x03, kAbsRy = 0x04, kAbsRz = 0x05, kAbsHat0X = 0x10, kAbsHat0Y = 0x11;
constexpr uint16_t kBtnSouth = 0x130, kBtnEast = 0x131, kBtnNorth = 0x133, kBtnWest = 0x134, kBtnZ = 0x135, kBtnTl = 0x136, kBtnTr = 0x137,
                   kBtnTl2 = 0x138, kBtnTr2 = 0x139, kBtnSelect = 0x13a, kBtnStart = 0x13b, kBtnMode = 0x13c, kBtnThumbL = 0x13d, kBtnThumbR = 0x13e;
constexpr uint16_t kBtnMisc = 0x100, kKeyMax = 0x2ff;
constexpr uint16_t kFfRumble = 0x50, kFfGain = 0x60;
constexpr uint32_t kMaxEffects = 16;      // (ff-memless's FF_MEMLESS_EFFECTS)
constexpr uint32_t kEventBytes = 24;      // struct input_event on the 64-bit ABIs: timeval (16), type, code, value
constexpr uint32_t kJsEventBytes = 8;     // struct js_event: time (ms), value, type, number
constexpr uint32_t kFfEffectBytes = 48;   // struct ff_effect on the 64-bit ABIs (the union is 8-aligned, at 16)
constexpr uint8_t kJsEventButton = 0x01, kJsEventAxis = 0x02, kJsEventInit = 0x80;

// Bitmap sizes as evdev hands them out (whole longs): EV_MAX 0x1f, KEY_MAX 0x2ff, ABS_MAX 0x3f, FF_MAX 0x7f, the others 0x0f / 0x1f.
constexpr uint32_t kKeyBitmapBytes = 96, kFfBitmapBytes = 16, kSmallBitmapBytes = 8;

// --- the drivers' layouts ---------------------------------------------------------------------------------------------------------------------------

enum class KeySource : uint8_t { Button, LeftTrigger, RightTrigger };
struct KeyMap { uint16_t code; uint32_t button; KeySource source; };
enum class AxisSource : uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, HatX, HatY };
struct AxisMap { uint16_t code; AxisSource source; };

// (sorted by code: the joystick API numbers the buttons and axes in that order)
constexpr KeyMap kXpadKeys[] = {
    {kBtnSouth, kPadSouth, KeySource::Button},      {kBtnEast, kPadEast, KeySource::Button},          {kBtnNorth, kPadWest, KeySource::Button},   // BTN_X
    {kBtnWest, kPadNorth, KeySource::Button},       {kBtnTl, kPadLeftShoulder, KeySource::Button},    {kBtnTr, kPadRightShoulder, KeySource::Button},
    {kBtnSelect, kPadBack, KeySource::Button},      {kBtnStart, kPadStart, KeySource::Button},        {kBtnMode, kPadGuide, KeySource::Button},
    {kBtnThumbL, kPadLeftStick, KeySource::Button}, {kBtnThumbR, kPadRightStick, KeySource::Button},
};
constexpr KeyMap kPlayStationKeys[] = {
    {kBtnSouth, kPadSouth, KeySource::Button},      {kBtnEast, kPadEast, KeySource::Button},          {kBtnNorth, kPadNorth, KeySource::Button},
    {kBtnWest, kPadWest, KeySource::Button},        {kBtnTl, kPadLeftShoulder, KeySource::Button},    {kBtnTr, kPadRightShoulder, KeySource::Button},
    {kBtnTl2, 0, KeySource::LeftTrigger},           {kBtnTr2, 0, KeySource::RightTrigger},            {kBtnSelect, kPadBack, KeySource::Button},
    {kBtnStart, kPadStart, KeySource::Button},      {kBtnMode, kPadGuide, KeySource::Button},         {kBtnThumbL, kPadLeftStick, KeySource::Button},
    {kBtnThumbR, kPadRightStick, KeySource::Button},
};
constexpr KeyMap kNintendoKeys[] = {
    {kBtnSouth, kPadSouth, KeySource::Button},      {kBtnEast, kPadEast, KeySource::Button},          {kBtnNorth, kPadNorth, KeySource::Button},
    {kBtnWest, kPadWest, KeySource::Button},        {kBtnZ, kPadMisc, KeySource::Button},             {kBtnTl, kPadLeftShoulder, KeySource::Button},
    {kBtnTr, kPadRightShoulder, KeySource::Button}, {kBtnTl2, 0, KeySource::LeftTrigger},             {kBtnTr2, 0, KeySource::RightTrigger},
    {kBtnSelect, kPadBack, KeySource::Button},      {kBtnStart, kPadStart, KeySource::Button},        {kBtnMode, kPadGuide, KeySource::Button},
    {kBtnThumbL, kPadLeftStick, KeySource::Button}, {kBtnThumbR, kPadRightStick, KeySource::Button},
};
constexpr AxisMap kAnalogAxes[] = {
    {kAbsX, AxisSource::LeftX},  {kAbsY, AxisSource::LeftY},   {kAbsZ, AxisSource::LeftTrigger},  {kAbsRx, AxisSource::RightX},
    {kAbsRy, AxisSource::RightY}, {kAbsRz, AxisSource::RightTrigger}, {kAbsHat0X, AxisSource::HatX}, {kAbsHat0Y, AxisSource::HatY},
};
constexpr AxisMap kNintendoAxes[] = {      // (digital ZL/ZR: buttons only, as hid-nintendo has them)
    {kAbsX, AxisSource::LeftX}, {kAbsY, AxisSource::LeftY}, {kAbsRx, AxisSource::RightX}, {kAbsRy, AxisSource::RightY},
    {kAbsHat0X, AxisSource::HatX}, {kAbsHat0Y, AxisSource::HatY},
};
constexpr uint32_t kMaxKeys = 16, kMaxAxes = 8;
static_assert(sizeof(kNintendoKeys) / sizeof(KeyMap) <= kMaxKeys && sizeof(kAnalogAxes) / sizeof(AxisMap) <= kMaxAxes);

struct Profile {
  ArrayView<const KeyMap> keys;
  ArrayView<const AxisMap> axes;
};
Profile profile_of(InputPadLayout layout) {
  switch (layout) {
    case InputPadLayout::PlayStation: return Profile{ArrayView<const KeyMap>(kPlayStationKeys), ArrayView<const AxisMap>(kAnalogAxes)};
    case InputPadLayout::Nintendo: return Profile{ArrayView<const KeyMap>(kNintendoKeys), ArrayView<const AxisMap>(kNintendoAxes)};
    case InputPadLayout::Xpad: break;
  }
  return Profile{ArrayView<const KeyMap>(kXpadKeys), ArrayView<const AxisMap>(kAnalogAxes)};
}

struct AbsInfo { int32_t minimum = 0, maximum = 0, fuzz = 0, flat = 0; };
AbsInfo abs_info(AxisSource source) {
  switch (source) {
    case AxisSource::LeftTrigger:
    case AxisSource::RightTrigger: return AbsInfo{0, 255, 0, 0};
    case AxisSource::HatX:
    case AxisSource::HatY: return AbsInfo{-1, 1, 0, 0};
    default: return AbsInfo{-32768, 32767, 16, 128};      // (xpad's sticks)
  }
}

int32_t axis_value(AxisSource source, const InputPadState& s) {
  switch (source) {
    case AxisSource::LeftX: return s.left_x;
    case AxisSource::LeftY: return -1 - int32_t{s.left_y};      // (Linux has down positive: xpad reports ~y)
    case AxisSource::RightX: return s.right_x;
    case AxisSource::RightY: return -1 - int32_t{s.right_y};
    case AxisSource::LeftTrigger: return s.left_trigger;
    case AxisSource::RightTrigger: return s.right_trigger;
    case AxisSource::HatX: return ((s.buttons & kPadDpadRight) ? 1 : 0) - ((s.buttons & kPadDpadLeft) ? 1 : 0);
    case AxisSource::HatY: return ((s.buttons & kPadDpadDown) ? 1 : 0) - ((s.buttons & kPadDpadUp) ? 1 : 0);
  }
  return 0;
}

bool key_pressed(const KeyMap& key, const InputPadState& s) {
  switch (key.source) {
    case KeySource::LeftTrigger: return s.left_trigger != 0;
    case KeySource::RightTrigger: return s.right_trigger != 0;
    case KeySource::Button: break;
  }
  return (s.buttons & key.button) != 0;
}

// joydev's default correction (joydev_connect, joydev_correct): the axis's range onto -32767..32767, with its flat part around the centre as 0.
int32_t joystick_value(AxisSource source, int32_t value) {
  const AbsInfo a = abs_info(source);
  const int64_t centre = (int64_t{a.maximum} + a.minimum) / 2;
  const int64_t low = centre - a.flat, high = centre + a.flat;
  const int64_t half = (int64_t{a.maximum} - a.minimum) / 2 - 2 * int64_t{a.flat};
  const int64_t coefficient = half ? (int64_t{1} << 29) / half : 0;
  int64_t v;
  if (value > low) v = value < high ? 0 : (coefficient * (value - high)) >> 14;
  else v = (coefficient * (value - low)) >> 14;
  if (v < -32767) v = -32767;
  if (v > 32767) v = 32767;
  return int32_t(v);
}

bool same_pad(const InputPadInfo& a, const InputPadInfo& b) {
  return a.vendor == b.vendor && a.product == b.product && a.layout == b.layout && a.name.view() == b.name.view();
}

// --- which slots have pads; the grabs; when the set last changed ------------------------------------------------------------------------------------

struct Registry {
  Array<uint64_t, kInputPadSlots> grab_owner;      // the client holding EVIOCGRAB on a slot (0: none)
  uint32_t mask = 0;                               // the slots occupied when last looked at
  uint64_t changed = 0;                            // seconds since 1970 of the last change seen (/dev/input's modification time)
};
kernel::Mutex g_registry_mutex;      // guards g_registry; never held while the provider is called
Registry g_registry;
uint64_t g_next_client = 0;

uint32_t occupied_mask() {
  const InputPadProvider* pads = provider();
  if (!pads || !pads->info) return 0;
  uint32_t mask = 0;
  for (uint32_t slot = 0; slot < kInputPadSlots; ++slot) {
    InputPadInfo info;
    if (pads->info(pads->context, slot, &info)) mask |= 1u << slot;
  }
  return mask;
}

// /dev/input's modification time: moved on whenever the set of pads is seen to differ (SDL without udev looks for that every few seconds).
uint64_t directory_time(uint64_t now_seconds) {
  const uint32_t mask = occupied_mask();
  kernel::ScopedMutex hold(g_registry_mutex);
  if (g_registry.changed == 0) g_registry.changed = now_seconds;
  if (mask != g_registry.mask) {
    g_registry.mask = mask;
    g_registry.changed = now_seconds > g_registry.changed ? now_seconds : g_registry.changed + 1;
  }
  return g_registry.changed;
}

enum class NodeKind : uint8_t { Event, Joystick };

// "event3" / "js0" under /dev/input: the kind and the slot. False for any other name.
bool parse_node(StringView name, NodeKind* kind, uint32_t* slot) {
  StringView digits;
  if (name.starts_with(StringView("event"))) { *kind = NodeKind::Event; digits = name.without_prefix(5); }
  else if (name.starts_with(StringView("js"))) { *kind = NodeKind::Joystick; digits = name.without_prefix(2); }
  else return false;
  if (digits.empty() || digits.size() > 3 || (digits.size() > 1 && digits[0] == '0')) return false;
  uint32_t n = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + uint32_t(c - '0');
  }
  if (n >= kInputPadSlots) return false;
  *slot = n;
  return true;
}

uint64_t device_number(NodeKind kind, uint32_t slot) {      // (glibc's makedev for a major and minor below 256)
  return (uint64_t{kInputMajor} << 8) | ((kind == NodeKind::Event ? kEventMinor : kJoystickMinor) + slot);
}

bool slot_present(uint32_t slot, InputPadInfo* info) {
  const InputPadProvider* pads = provider();
  return pads && pads->info && pads->info(pads->context, slot, info);
}

void node_meta(NodeKind kind, uint32_t slot, linuxabi::InputNodeMeta* meta) {
  *meta = linuxabi::InputNodeMeta();
  meta->mode = kSIfChr | 0666;
  meta->rdev = device_number(kind, slot);
  meta->ino = 0x494E5055ull * 256 + (kind == NodeKind::Event ? 64 : 0) + slot;
}

const StringView kDirectory("/dev/input");

// /dev/input listed: the nodes of the slots occupied when it was opened (or rewound).
class InputDirectory final : public filesystem::IDirectory {
 public:
  InputDirectory() { fill(); }
  Status next(DirectoryEntry* entry) override {
    while (index_ < 2 * kInputPadSlots) {
      const uint32_t i = index_++;
      const uint32_t slot = i % kInputPadSlots;
      if (!(mask_ & (1u << slot))) continue;
      *entry = DirectoryEntry();
      const bool event = i < kInputPadSlots;
      if (!entry->name.assign(event ? StringView("event") : StringView("js")) || !entry->name.append_uint(slot)) return Status::OutOfMemory;
      entry->type = filesystem::NodeType::File;
      entry->special = filesystem::SpecialNode::CharDevice;
      return Status::Ok;
    }
    return Status::EndOfDirectory;
  }
  void rewind() override { fill(); }

 private:
  void fill() { mask_ = occupied_mask(); index_ = 0; }
  uint32_t mask_ = 0, index_ = 0;
};

}  // namespace

// --- one open node ----------------------------------------------------------------------------------------------------------------------------------

class InputClient {
 public:
  struct Event {
    uint64_t ns = 0;      // evdev: in the client's clock; joystick: monotonic
    uint16_t type = 0, code = 0;
    int32_t value = 0;
  };
  struct Effect {
    bool used = false, playing = false;
    uint16_t strong = 0, weak = 0, length_ms = 0, delay_ms = 0;
    uint64_t start_ns = 0, end_ns = 0;      // (end 0: until stopped)
  };
  static constexpr uint32_t kQueue = 32;

  InputClient(NodeKind kind, uint32_t slot, const InputPadInfo& info, uint64_t id)
      : kind(kind), slot(slot), info(info), profile(profile_of(info.layout)), id(id) {}
  ~InputClient();
  InputClient(const InputClient&) = delete;
  InputClient& operator=(const InputClient&) = delete;

  kernel::Mutex mutex;      // guards everything below (a client may be used by several threads of a process, or by a parent and its fork)
  const NodeKind kind;
  const uint32_t slot;
  const InputPadInfo info;      // the pad it was opened on: a different one in the slot later is not it
  const Profile profile;
  const uint64_t id;
  bool non_blocking = false, gone = false, revoked = false, grabbed = false;
  uint32_t clock = 0;           // EVIOCSCLOCKID: CLOCK_REALTIME 0, CLOCK_MONOTONIC 1, CLOCK_BOOTTIME 7
  uint32_t keys = 0;            // what was last reported: bit i = profile.keys[i] down
  Array<int32_t, kMaxAxes> axes;      // (joystick: the corrected values)
  uint32_t startup = 0;         // joystick: how many of the JS_EVENT_INIT burst were handed out
  Array<Event, kQueue> queue;
  uint32_t head = 0, count = 0;
  Array<Effect, kMaxEffects> effects;
  uint16_t gain = 0xFFFF;
  uint16_t applied_strong = 0, applied_weak = 0;

  bool push(uint16_t type, uint16_t code, int32_t value, uint64_t ns) {
    if (count == kQueue) return false;
    Event& e = queue[(head + count) % kQueue];
    e.ns = ns;
    e.type = type;
    e.code = code;
    e.value = value;
    ++count;
    return true;
  }
  Event pop() {
    const Event e = queue[head];
    head = (head + 1) % kQueue;
    --count;
    return e;
  }
  uint32_t startup_total() const { return uint32_t(profile.keys.size() + profile.axes.size()); }
};

namespace {

uint64_t now_in(Process& p, uint32_t clock) { return clock == 0 ? p.host().realtime_ns() : p.host().monotonic_ns(); }

// The motors as the effects playing now want them (ff-memless adds rumble effects up and scales them by the gain); sent when that changed. Lock held.
void update_rumble(Process* p, InputClient& c, bool stop_all) {
  uint32_t strong = 0, weak = 0;
  if (!stop_all && p) {
    const uint64_t now = p->host().monotonic_ns();
    for (InputClient::Effect& e : c.effects) {
      if (!e.used || !e.playing) continue;
      if (e.end_ns != 0 && now >= e.end_ns) { e.playing = false; continue; }
      if (now < e.start_ns) continue;
      strong += e.strong;
      weak += e.weak;
    }
  }
  if (strong > 0xFFFF) strong = 0xFFFF;
  if (weak > 0xFFFF) weak = 0xFFFF;
  strong = strong * c.gain / 0xFFFF;
  weak = weak * c.gain / 0xFFFF;
  if (strong == c.applied_strong && weak == c.applied_weak) return;
  if (const InputPadProvider* pads = provider(); pads && pads->rumble && !c.gone) (void)pads->rumble(pads->context, c.slot, uint16_t(strong), uint16_t(weak));
  c.applied_strong = uint16_t(strong);
  c.applied_weak = uint16_t(weak);
}

bool grabbed_by_other(const InputClient& c) {
  kernel::ScopedMutex hold(g_registry_mutex);
  const uint64_t owner = g_registry.grab_owner[c.slot];
  return owner != 0 && owner != c.id;
}

void release_grab(InputClient& c) {
  if (!c.grabbed) return;
  kernel::ScopedMutex hold(g_registry_mutex);
  if (g_registry.grab_owner[c.slot] == c.id) g_registry.grab_owner[c.slot] = 0;
  c.grabbed = false;
}

// The pad's state now: false (and the client marked gone) when its slot emptied or holds another pad. Lock held.
bool current_state(InputClient& c, InputPadState* state) {
  if (c.gone) return false;
  const InputPadProvider* pads = provider();
  InputPadInfo now;
  if (!pads || !pads->state || !pads->info || !pads->state(pads->context, c.slot, state) || !pads->info(pads->context, c.slot, &now) || !same_pad(now, c.info)) {
    c.gone = true;
    c.applied_strong = c.applied_weak = 0;      // (the pad went with its motors; another one in the slot is not touched)
    return false;
  }
  return true;
}

// Looks at the pad and queues what changed since the last look (only when the queue is empty: a reader that falls behind skips states, never
// sees them out of order). Lock held.
void refresh(Process& p, InputClient& c) {
  if (c.revoked) return;
  InputPadState s;
  if (!current_state(c, &s)) return;
  update_rumble(&p, c, false);
  const bool silent = c.kind == NodeKind::Event ? grabbed_by_other(c) : c.startup < c.startup_total();      // (state taken in, nothing queued)
  if (c.count != 0 && !silent) return;
  const uint64_t ns = c.kind == NodeKind::Event ? now_in(p, c.clock) : p.host().monotonic_ns();
  bool any = false;
  for (uint64_t i = 0; i < c.profile.keys.size(); ++i) {
    const uint32_t bit = 1u << i;
    const bool down = key_pressed(c.profile.keys[i], s);
    if (down == ((c.keys & bit) != 0)) continue;
    c.keys ^= bit;
    if (silent) continue;
    (void)c.push(kEvKey, c.kind == NodeKind::Event ? c.profile.keys[i].code : uint16_t(i), down ? 1 : 0, ns);
    any = true;
  }
  for (uint64_t i = 0; i < c.profile.axes.size(); ++i) {
    const AxisSource source = c.profile.axes[i].source;
    int32_t v = axis_value(source, s);
    if (c.kind == NodeKind::Joystick) v = joystick_value(source, v);
    if (v == c.axes[i]) continue;
    c.axes[i] = v;
    if (silent) continue;
    (void)c.push(kEvAbs, c.kind == NodeKind::Event ? c.profile.axes[i].code : uint16_t(i), v, ns);
    any = true;
  }
  if (any && c.kind == NodeKind::Event) (void)c.push(kEvSyn, kSynReport, 0, ns);
}

bool readable(const InputClient& c) {
  if (c.kind == NodeKind::Joystick && c.startup < c.startup_total()) return true;
  return c.count != 0;
}

InputClient* client_of(Process& p, uint64_t fd, OpenObject** object_out = nullptr) {
  if (p.files().kind(fd) != DescriptorKind::InputDevice) return nullptr;
  OpenObject* object = p.files().object(fd);
  if (!object || !object->input) return nullptr;
  if (object_out) *object_out = object;
  return object->input.get();
}

// Copies out up to `room` events (whole records into `out`): evdev's input_event or the joystick's js_event, the burst after open first. Lock held.
uint64_t take_events(Process& p, InputClient& c, ByteView out) {
  const uint64_t size = c.kind == NodeKind::Event ? kEventBytes : kJsEventBytes;
  uint64_t used = 0;
  ByteWriter w(out);
  while (out.size() - used >= size) {
    if (c.kind == NodeKind::Joystick && c.startup < c.startup_total()) {      // JS_EVENT_INIT: every button, then every axis, as they are now
      const uint32_t i = c.startup++;
      const uint32_t keys = uint32_t(c.profile.keys.size());
      const bool button = i < keys;
      w.le32(uint32_t(p.host().monotonic_ns() / 1000000));
      w.le16(uint16_t(int16_t(button ? ((c.keys >> i) & 1) : c.axes[i - keys])));
      w.u8(uint8_t((button ? kJsEventButton : kJsEventAxis) | kJsEventInit));
      w.u8(uint8_t(button ? i : i - keys));
    } else if (c.count != 0) {
      const InputClient::Event e = c.pop();
      if (c.kind == NodeKind::Event) {
        w.le64(e.ns / 1000000000ull);
        w.le64((e.ns % 1000000000ull) / 1000);
        w.le16(e.type);
        w.le16(e.code);
        w.le32(uint32_t(e.value));
      } else {
        w.le32(uint32_t(e.ns / 1000000));
        w.le16(uint16_t(int16_t(e.value)));
        w.u8(e.type == kEvKey ? kJsEventButton : kJsEventAxis);
        w.u8(uint8_t(e.code));
      }
    } else {
      break;
    }
    used += size;
  }
  return w.ok() ? used : 0;
}

// Whether a waiting call should stop: the process ends, was interrupted, or a signal came for the thread.
bool wait_interrupted(Process& p) {
  IProcessRuntime* runtime = p.runtime();
  return p.exited() || (runtime && runtime->interrupted()) || p.signal_interrupts();
}

// --- ioctl helpers ----------------------------------------------------------------------------------------------------------------------------------

struct Request {
  uint32_t direction, type, number, size;      // _IOC_DIR (1 write, 2 read), _IOC_TYPE, _IOC_NR, _IOC_SIZE
};
Request decode(uint32_t code) { return Request{code >> 30, (code >> 8) & 0xFF, code & 0xFF, (code >> 16) & 0x3FFF}; }
constexpr uint32_t ioc(uint32_t direction, uint32_t type, uint32_t number, uint32_t size) { return (direction << 30) | (size << 16) | (type << 8) | number; }
constexpr uint32_t kRead = 2, kWrite = 1;

uint64_t put_int(Process& p, uint64_t address, uint32_t value) { return p.write_user_u32(address, value) ? 0 : error(kEfault); }
bool get_int(Process& p, uint64_t address, uint32_t* value) {
  uint8_t raw[4];
  if (!p.read_user(address, ByteView(raw, sizeof raw))) return false;
  ByteReader r{ConstByteView(raw, sizeof raw)};
  *value = r.le32();
  return true;
}
// Copies min(room, bytes) to the program and answers how many (evdev's bits_to_user / str_to_user).
uint64_t put_limited(Process& p, uint64_t address, uint32_t room, ConstByteView bytes) {
  const uint64_t n = bytes.size() < room ? bytes.size() : room;
  if (n == 0) return 0;
  return p.write_user(address, bytes.first(n)) ? n : error(kEfault);
}
uint64_t put_string(Process& p, uint64_t address, uint32_t room, StringView text) {
  uint8_t buffer[80] = {};
  const uint64_t length = text.size() < sizeof buffer - 1 ? text.size() : sizeof buffer - 1;
  ByteView(buffer, sizeof buffer).first(length).copy_from(text.bytes().first(length));
  return put_limited(p, address, room, ConstByteView(buffer, length + 1));      // (with the NUL)
}
void set_bit(ByteView bitmap, uint32_t bit) {
  if (bit / 8 < bitmap.size()) bitmap[bit / 8] = uint8_t(bitmap[bit / 8] | (1u << (bit % 8)));
}

void phys_of(const InputClient& c, FixedString<48>* out) {
  (void)out->assign("usb-tcos-gamepad-");
  (void)out->append_uint(c.slot);
  (void)out->append("/input0");
}

// evdev's ioctls (evdev_do_ioctl).
uint64_t event_ioctl(Process& p, InputClient& c, uint32_t code, uint64_t argument) {
  const Request r = decode(code);
  if (code == 0x5421) {      // FIONBIO (the VFS's, before the driver's)
    uint32_t value = 0;
    if (!get_int(p, argument, &value)) return error(kEfault);
    c.non_blocking = value != 0;
    return 0;
  }
  if (c.revoked || c.gone) return error(kEnodev);
  if (code == ioc(kRead, 'E', 0x01, 4)) return put_int(p, argument, 0x010001);      // EVIOCGVERSION
  if (code == ioc(kRead, 'E', 0x02, 8)) {                                             // EVIOCGID: bustype, vendor, product, version
    uint8_t id[8];
    ByteWriter w{ByteView(id, sizeof id)};
    w.le16(0x03);      // BUS_USB
    w.le16(c.info.vendor);
    w.le16(c.info.product);
    w.le16(c.info.version);
    return p.write_user(argument, ConstByteView(id, sizeof id)) ? 0 : error(kEfault);
  }
  if (code == ioc(kRead, 'E', 0x84, 4)) return put_int(p, argument, c.info.rumble ? kMaxEffects : 0);      // EVIOCGEFFECTS
  if (code == ioc(kWrite, 'E', 0x90, 4)) {      // EVIOCGRAB (the value itself, not a pointer)
    kernel::ScopedMutex hold(g_registry_mutex);
    uint64_t& owner = g_registry.grab_owner[c.slot];
    if (argument != 0) {
      if (owner != 0) return error(kEbusy);
      owner = c.id;
      c.grabbed = true;
      return 0;
    }
    if (owner != c.id) return error(kEinval);
    owner = 0;
    c.grabbed = false;
    return 0;
  }
  if (code == ioc(kWrite, 'E', 0x91, 4)) {      // EVIOCREVOKE: this descriptor is done with the device
    if (argument != 0) return error(kEinval);
    release_grab(c);
    update_rumble(&p, c, true);
    c.revoked = true;
    c.count = 0;
    return 0;
  }
  if (code == ioc(kWrite, 'E', 0xa0, 4)) {      // EVIOCSCLOCKID
    uint32_t clock = 0;
    if (!get_int(p, argument, &clock)) return error(kEfault);
    if (clock != 0 && clock != 1 && clock != 7) return error(kEinval);
    if (clock != c.clock) { c.clock = clock; c.count = 0; }      // (Linux drops what was queued in the old clock)
    return 0;
  }
  if (code == ioc(kWrite, 'E', 0x80, kFfEffectBytes)) {      // EVIOCSFF: upload a rumble effect
    if (!c.info.rumble) return error(kEnosys);
    uint8_t raw[kFfEffectBytes];
    if (!p.read_user(argument, ByteView(raw, sizeof raw))) return error(kEfault);
    ByteReader in{ConstByteView(raw, sizeof raw)};
    const uint16_t type = in.le16();
    const int16_t id = int16_t(in.le16());
    in.skip(6);      // direction, trigger
    const uint16_t length = in.le16(), delay = in.le16();
    in.skip(2);      // (padding before the union)
    const uint16_t strong = in.le16(), weak = in.le16();
    if (!in.ok() || type != kFfRumble) return error(kEinval);
    int32_t slot = id;
    if (id == -1) {
      slot = -1;
      for (uint32_t i = 0; i < kMaxEffects; ++i)
        if (!c.effects[i].used) { slot = int32_t(i); break; }
      if (slot < 0) return error(kEnospc);
    } else if (id < 0 || uint32_t(id) >= kMaxEffects || !c.effects[uint32_t(id)].used) {
      return error(kEinval);
    }
    InputClient::Effect& e = c.effects[uint32_t(slot)];
    const bool was_used = e.used;
    e.used = true;
    e.strong = strong;
    e.weak = weak;
    e.length_ms = length;
    e.delay_ms = delay;
    uint8_t written[2] = {uint8_t(slot), uint8_t(uint32_t(slot) >> 8)};
    if (!p.write_user(argument + 2, ConstByteView(written, sizeof written))) {      // (the id goes back into the program's structure)
      if (!was_used) e = InputClient::Effect();
      return error(kEfault);
    }
    update_rumble(&p, c, false);      // (a playing effect changed takes its new strength at once)
    return 0;
  }
  if (code == ioc(kWrite, 'E', 0x81, 4)) {      // EVIOCRMFF (the id itself)
    if (!c.info.rumble) return error(kEnosys);
    const int64_t id = int64_t(int32_t(uint32_t(argument)));
    if (id < 0 || uint64_t(id) >= kMaxEffects || !c.effects[uint64_t(id)].used) return error(kEinval);
    c.effects[uint64_t(id)] = InputClient::Effect();
    update_rumble(&p, c, false);
    return 0;
  }
  if (r.type != 'E') return error(kEinval);
  if (r.direction == kRead && r.number == 0x06) return put_string(p, argument, r.size, c.info.name.view());      // EVIOCGNAME(len)
  if (r.direction == kRead && r.number == 0x07) {                                                                // EVIOCGPHYS(len)
    FixedString<48> phys;
    phys_of(c, &phys);
    return put_string(p, argument, r.size, phys.view());
  }
  if (r.direction == kRead && r.number == 0x08) return error(kEnoent);      // EVIOCGUNIQ: no serial number
  if (r.direction == kRead && r.number == 0x09) {                           // EVIOCGPROP: no INPUT_PROP_* for a gamepad
    uint8_t none[kSmallBitmapBytes] = {};
    return put_limited(p, argument, r.size, ConstByteView(none, sizeof none));
  }
  if (r.direction == kRead && r.number == 0x18) {      // EVIOCGKEY(len): the buttons down now (queued button events are dropped: the program has the state)
    InputPadState s;
    if (!current_state(c, &s)) return error(kEnodev);
    uint8_t bits[kKeyBitmapBytes] = {};
    uint32_t keys = 0;
    for (uint64_t i = 0; i < c.profile.keys.size(); ++i)
      if (key_pressed(c.profile.keys[i], s)) { set_bit(ByteView(bits, sizeof bits), c.profile.keys[i].code); keys |= 1u << i; }
    c.keys = keys;
    uint32_t kept = 0;
    bool payload = false;
    const uint32_t n = c.count;
    for (uint32_t i = 0; i < n; ++i) {      // (__evdev_flush_queue: EV_KEY out, and a SYN_REPORT left with nothing before it)
      const InputClient::Event e = c.pop();
      if (e.type == kEvKey) continue;
      if (e.type == kEvSyn && !payload) continue;
      payload = e.type != kEvSyn;
      (void)c.push(e.type, e.code, e.value, e.ns);
      ++kept;
    }
    (void)kept;
    return put_limited(p, argument, r.size, ConstByteView(bits, sizeof bits));
  }
  if (r.direction == kRead && (r.number == 0x19 || r.number == 0x1a || r.number == 0x1b)) {      // EVIOCGLED, EVIOCGSND, EVIOCGSW: none
    uint8_t none[kSmallBitmapBytes] = {};
    return put_limited(p, argument, r.size, ConstByteView(none, sizeof none));
  }
  if (r.direction == kRead && r.number >= 0x20 && r.number < 0x40) {      // EVIOCGBIT(ev, len)
    const uint32_t ev = r.number - 0x20;
    uint8_t bits[kKeyBitmapBytes] = {};
    ByteView b(bits, sizeof bits);
    uint32_t bytes = kSmallBitmapBytes;
    switch (ev) {
      case 0:      // the event types
        set_bit(b, kEvSyn);
        set_bit(b, kEvKey);
        set_bit(b, kEvAbs);
        if (c.info.rumble) set_bit(b, kEvFf);
        break;
      case kEvKey:
        for (const KeyMap& k : c.profile.keys) set_bit(b, k.code);
        bytes = kKeyBitmapBytes;
        break;
      case kEvAbs:
        for (const AxisMap& a : c.profile.axes) set_bit(b, a.code);
        break;
      case kEvFf:
        if (c.info.rumble) { set_bit(b, kFfRumble); set_bit(b, kFfGain); }
        bytes = kFfBitmapBytes;
        break;
      case 0x02: case 0x04: case 0x05: case 0x11: case 0x12: break;      // EV_REL, EV_MSC, EV_SW, EV_LED, EV_SND: nothing
      default: return error(kEinval);
    }
    return put_limited(p, argument, r.size, ConstByteView(bits, bytes));
  }
  if (r.direction == kRead && r.number >= 0x40 && r.number < 0x80) {      // EVIOCGABS(axis): struct input_absinfo (as much of it as asked for)
    const uint32_t axis = r.number - 0x40;
    uint8_t raw[24] = {};
    for (const AxisMap& a : c.profile.axes) {
      if (a.code != axis) continue;
      InputPadState s;
      if (!current_state(c, &s)) return error(kEnodev);
      const AbsInfo info = abs_info(a.source);
      ByteWriter w{ByteView(raw, sizeof raw)};
      w.le32(uint32_t(axis_value(a.source, s)));
      w.le32(uint32_t(info.minimum));
      w.le32(uint32_t(info.maximum));
      w.le32(uint32_t(info.fuzz));
      w.le32(uint32_t(info.flat));
      w.le32(0);      // resolution
    }
    const uint64_t n = r.size < sizeof raw ? r.size : sizeof raw;
    if (n == 0) return 0;
    return p.write_user(argument, ConstByteView(raw, n)) ? 0 : error(kEfault);
  }
  return error(kEinval);      // (EVIOCSABS, EVIOCGKEYCODE, EVIOCGREP, EVIOCGMTSLOTS, EVIOCGMASK ...: what a gamepad here does not have)
}

// joydev's ioctls (joydev_ioctl_common).
uint64_t joystick_ioctl(Process& p, InputClient& c, uint32_t code, uint64_t argument) {
  const Request r = decode(code);
  if (code == 0x5421) {      // FIONBIO
    uint32_t value = 0;
    if (!get_int(p, argument, &value)) return error(kEfault);
    c.non_blocking = value != 0;
    return 0;
  }
  if (c.gone) return error(kEnodev);
  if (code == ioc(kRead, 'j', 0x01, 4)) return put_int(p, argument, 0x020100);      // JSIOCGVERSION
  if (code == ioc(kRead, 'j', 0x11, 1) || code == ioc(kRead, 'j', 0x12, 1)) {       // JSIOCGAXES, JSIOCGBUTTONS
    const uint8_t n = uint8_t(r.number == 0x11 ? c.profile.axes.size() : c.profile.keys.size());
    return p.write_user(argument, ConstByteView(&n, 1)) ? 0 : error(kEfault);
  }
  if (r.type != 'j' || r.direction != kRead) return error(kEinval);
  if (r.number == 0x13) return put_string(p, argument, r.size, c.info.name.view());      // JSIOCGNAME(len)
  if (r.number == 0x32) {      // JSIOCGAXMAP: the ABS_* code of each axis (__u8[ABS_CNT])
    uint8_t map[0x40] = {};
    for (uint64_t i = 0; i < c.profile.axes.size(); ++i) map[i] = uint8_t(c.profile.axes[i].code);
    return put_limited(p, argument, r.size, ConstByteView(map, sizeof map));
  }
  if (r.number == 0x34) {      // JSIOCGBTNMAP: the KEY_* code of each button (__u16[KEY_MAX - BTN_MISC + 1])
    uint8_t map[2 * (kKeyMax - kBtnMisc + 1)] = {};
    ByteWriter w{ByteView(map, sizeof map)};
    for (const KeyMap& k : c.profile.keys) w.le16(k.code);
    return put_limited(p, argument, r.size, ConstByteView(map, sizeof map));
  }
  return error(kEinval);      // (JSIOCGCORR/SCORR, JSIOCSAXMAP/SBTNMAP: not kept)
}

}  // namespace

InputClient::~InputClient() {
  release_grab(*this);
  update_rumble(nullptr, *this, true);      // (closing the last descriptor stops its effects, as Linux's flush does)
}

void set_input_pad_provider(const InputPadProvider* pads) { __atomic_store_n(&g_provider, pads, __ATOMIC_RELEASE); }

namespace linuxabi {

int64_t input_node_at(StringView virtual_path, InputNodeMeta* meta, uint64_t now_seconds) {
  if (virtual_path == kDirectory) {
    *meta = InputNodeMeta();
    meta->directory = true;
    meta->mode = kSIfDir | 0755;
    meta->ino = 0x494E5055ull;
    meta->modified = directory_time(now_seconds);
    return 1;
  }
  if (!virtual_path.starts_with(kDirectory) || virtual_path.size() <= kDirectory.size() || virtual_path[kDirectory.size()] != '/') return 0;
  NodeKind kind;
  uint32_t slot = 0;
  InputPadInfo info;
  if (!parse_node(virtual_path.without_prefix(kDirectory.size() + 1), &kind, &slot) || !slot_present(slot, &info)) return -kEnoent;
  node_meta(kind, slot, meta);
  meta->modified = now_seconds;
  return 1;
}

bool input_device_exists(StringView virtual_path) {
  if (!virtual_path.starts_with(kDirectory) || virtual_path.size() <= kDirectory.size() || virtual_path[kDirectory.size()] != '/') return false;
  NodeKind kind;
  uint32_t slot = 0;
  InputPadInfo info;
  return parse_node(virtual_path.without_prefix(kDirectory.size() + 1), &kind, &slot) && slot_present(slot, &info);
}

int64_t input_open(Process& p, StringView virtual_path, uint64_t flags, bool cloexec, bool must_be_directory) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return -kEnoent;
  const uint64_t access = flags & kOAccMode;
  if (access == kOAccMode) return -kEinval;
  SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(files.allocator(), files.allocator());
  if (!object || !object->path.assign(virtual_path)) return -kEnomem;
  DescriptorKind kind = DescriptorKind::InputDevice;
  uint64_t token = 0;
  if (virtual_path == kDirectory) {
    if (access != kORdOnly) return -kEisdir;
    object->directory = UniquePointer<filesystem::IDirectory>::make_as<InputDirectory>(files.allocator());
    if (!object->directory) return -kEnomem;
    object->readable = true;
    kind = DescriptorKind::Directory;
  } else {
    NodeKind node;
    uint32_t slot = 0;
    InputPadInfo info;
    if (!virtual_path.starts_with(kDirectory) || virtual_path.size() <= kDirectory.size() ||
        !parse_node(virtual_path.without_prefix(kDirectory.size() + 1), &node, &slot) || !slot_present(slot, &info))
      return -kEnoent;
    if (must_be_directory) return -kEnotdir;
    token = __atomic_add_fetch(&g_next_client, 1, __ATOMIC_RELAXED);
    object->input = UniquePointer<InputClient>::make(files.allocator(), node, slot, info, token);
    if (!object->input) return -kEnomem;
    InputClient& c = *object->input;
    c.non_blocking = (flags & kONonBlock) != 0;
    {
      kernel::ScopedMutex hold(c.mutex);
      // What the pad is doing now is the starting point: evdev reports only changes from here (the program asks EVIOCGKEY/EVIOCGABS for the
      // rest); the joystick's JS_EVENT_INIT burst hands this state out first.
      InputPadState s;
      if (!current_state(c, &s)) return -kEnoent;
      for (uint64_t i = 0; i < c.profile.keys.size(); ++i)
        if (key_pressed(c.profile.keys[i], s)) c.keys |= 1u << i;
      for (uint64_t i = 0; i < c.profile.axes.size(); ++i) {
        const int32_t v = axis_value(c.profile.axes[i].source, s);
        c.axes[i] = node == NodeKind::Joystick ? joystick_value(c.profile.axes[i].source, v) : v;
      }
    }
    object->readable = access != kOWrite;
    object->writable = access != kORdOnly;
  }
  const int64_t fd = files.install(kind, object, token);
  if (fd < 0) return -kEmfile;
  files.set_close_on_exec(uint64_t(fd), cloexec);
  return fd;
}

uint64_t input_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) {
  OpenObject* object = nullptr;
  InputClient* client = client_of(p, fd, &object);
  if (!client) return error(kEbadf);
  if (!object->readable) return error(kEbadf);
  const uint64_t size = client->kind == NodeKind::Event ? kEventBytes : kJsEventBytes;
  if (count != 0 && count < size) return error(kEinval);
  uint8_t chunk[64 * kEventBytes];
  const uint64_t room = count < sizeof chunk ? count : sizeof chunk;
  for (;;) {
    uint64_t got = 0;
    bool non_blocking = false;
    {
      kernel::ScopedMutex hold(client->mutex);
      refresh(p, *client);
      if (client->gone || client->revoked) return error(kEnodev);
      got = take_events(p, *client, ByteView(chunk, room));
      non_blocking = client->non_blocking;
    }
    if (got != 0) return p.write_user(buffer, ConstByteView(chunk, got)) ? got : error(kEfault);
    if (non_blocking) return error(kEagain);
    if (count == 0) return 0;
    if (wait_interrupted(p)) return error(kEintr);
    p.block_sleep(kPollNs);
  }
}

uint64_t input_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) {
  OpenObject* object = nullptr;
  InputClient* client = client_of(p, fd, &object);
  if (!client) return error(kEbadf);
  if (!object->writable) return error(kEbadf);
  if (client->kind == NodeKind::Joystick) return error(kEinval);      // (joydev has no write)
  if (count != 0 && count < kEventBytes) return error(kEinval);
  kernel::ScopedMutex hold(client->mutex);
  if (client->gone || client->revoked) return error(kEnodev);
  uint64_t done = 0;
  while (count - done >= kEventBytes) {
    uint8_t raw[kEventBytes];
    if (!p.read_user(buffer + done, ByteView(raw, sizeof raw))) return done ? done : error(kEfault);
    ByteReader in{ConstByteView(raw, sizeof raw)};
    in.skip(16);      // (the time: ignored, as Linux ignores it)
    const uint16_t type = in.le16(), code = in.le16();
    const int32_t value = int32_t(in.le32());
    done += kEventBytes;
    if (type != kEvFf || !client->info.rumble) continue;      // (anything else would be injected into the device: a pad here takes none)
    if (code == kFfGain) {
      if (value >= 0 && value <= 0xFFFF) client->gain = uint16_t(value);
    } else if (code < kMaxEffects && client->effects[code].used) {
      InputClient::Effect& e = client->effects[code];
      if (value > 0) {
        const uint64_t now = p.host().monotonic_ns();
        e.playing = true;
        e.start_ns = now + uint64_t{e.delay_ms} * 1000000;
        e.end_ns = e.length_ms == 0 ? 0 : e.start_ns + uint64_t{e.length_ms} * 1000000 * uint64_t(value);      // (value: how many times it plays)
      } else {
        e.playing = false;
      }
    }
    update_rumble(&p, *client, false);
  }
  return done;
}

uint64_t input_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument) {
  InputClient* client = client_of(p, fd);
  if (!client) return error(kEbadf);
  kernel::ScopedMutex hold(client->mutex);
  refresh(p, *client);
  const uint32_t code = uint32_t(request);      // (the request is an unsigned int)
  return client->kind == NodeKind::Event ? event_ioctl(p, *client, code, argument) : joystick_ioctl(p, *client, code, argument);
}

uint16_t input_poll(Process& p, uint64_t fd) {
  constexpr uint16_t kIn = 0x1, kOut = 0x4, kErr = 0x8, kHup = 0x10, kNval = 0x20, kRdNorm = 0x40, kWrNorm = 0x100;
  InputClient* client = client_of(p, fd);
  if (!client) return kNval;
  kernel::ScopedMutex hold(client->mutex);
  refresh(p, *client);
  uint16_t out = 0;
  if (readable(*client)) out |= kIn | kRdNorm;
  if (client->gone || client->revoked) out |= kHup | kErr;
  if (client->kind == NodeKind::Event) out |= kOut | kWrNorm;
  return out;
}

bool input_non_blocking(Process& p, uint64_t fd) {
  InputClient* client = client_of(p, fd);
  if (!client) return false;
  kernel::ScopedMutex hold(client->mutex);
  return client->non_blocking;
}

void set_input_non_blocking(Process& p, uint64_t fd, bool value) {
  if (InputClient* client = client_of(p, fd)) {
    kernel::ScopedMutex hold(client->mutex);
    client->non_blocking = value;
  }
}

bool input_descriptor_meta(Process& p, uint64_t fd, InputNodeMeta* meta) {
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Directory) {
    const OpenObject* object = files.object(fd);
    if (!object || object->path.view() != kDirectory) return false;
    return input_node_at(kDirectory, meta, p.host().realtime_ns() / 1000000000ull) == 1;
  }
  InputClient* client = client_of(p, fd);
  if (!client) return false;
  node_meta(client->kind, client->slot, meta);
  return true;
}

}  // namespace linuxabi
}  // namespace process
