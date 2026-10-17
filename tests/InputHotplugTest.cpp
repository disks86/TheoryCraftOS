// Host-side tests for USB HID hot-plug: SystemInput::poll() against a fake host controller with keyboards, mice and tablets that are plugged in
// and pulled out. Keys and buttons held when a device leaves are released, the merged keyboard and mouse keep working, a port that gets a
// different device before anyone noticed is handled, and the storage driver and the input driver watch the same ports without taking events
// from each other.
#include <string>
#include <vector>
#include "FakeUsbSticks.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Hardware/SystemInput.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Notes {
  std::vector<std::string> lines;
  static void add(void* self, StringView text) { static_cast<Notes*>(self)->lines.push_back(std::string(text.data(), text.size())); }
  bool has(const char* needle) const {
    for (const std::string& s : lines) if (s.find(needle) != std::string::npos) return true;
    return false;
  }
};

static FakeStickHost::Stick device(int hid) {
  FakeStickHost::Stick s = make_stick(Bytes());
  s.hid = hid;
  return s;
}

static Bytes keys(uint8_t modifiers, std::initializer_list<uint8_t> pressed) {
  Bytes r(8, 0);
  r[0] = modifiers;
  size_t i = 2;
  for (uint8_t k : pressed) if (i < 8) r[i++] = k;
  return r;
}

static std::vector<KeyEvent> drain_keys(SystemInput& in) {
  std::vector<KeyEvent> events;
  KeyEvent e;
  while (in.keyboard().poll(&e)) events.push_back(e);
  return events;
}

static std::vector<MouseEvent> drain_mouse(SystemInput& in) {
  std::vector<MouseEvent> events;
  MouseEvent e;
  while (in.mouse().poll(&e)) events.push_back(e);
  return events;
}

static bool has_key(const std::vector<KeyEvent>& v, uint8_t usage, bool pressed) {
  for (const KeyEvent& e : v) if (e.usage == usage && e.pressed == pressed) return true;
  return false;
}

struct Fixture {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  Fixture() { usb.hosts.push_back(&host); }
};

static void test_keyboard() {
  Fixture f;
  {
    SystemInput input(f.pci, f.usb, f.alloc, nullptr);
    Notes notes;
    input.set_notifier(Notes::add, &notes);
    CHECK(input.poll() == 0 && input.keyboards().count() == 0);

    FakeStickHost::Stick kb = device(1);
    f.host.plug(1, &kb);
    CHECK(input.poll() == 1 && input.keyboards().count() == 1 && input.mice().count() == 0);
    CHECK(strcmp(input.keyboards().at(0)->name(), "USB HID keyboard") == 0 && notes.has("USB HID keyboard: added"));
    CHECK(input.poll() == 0);   // (nothing new)
    CHECK(f.host.set_configurations > 0);

    f.host.send_report(1, keys(0x02, {key::kA}));   // left shift (bit 1) and A
    std::vector<KeyEvent> ev = drain_keys(input);
    CHECK(ev.size() == 2 && has_key(ev, key::kLeftShift, true) && has_key(ev, key::kA, true));
    CHECK(input.keyboard().modifiers() == 0x02);

    // pulled out with both held: both are released, and the merged keyboard has none left
    f.host.unplug(1);
    CHECK(input.poll() == 1);
    CHECK(input.keyboards().count() == 0 && notes.has("removed"));
    ev = drain_keys(input);
    CHECK(ev.size() == 2 && has_key(ev, key::kLeftShift, false) && has_key(ev, key::kA, false));
    CHECK(input.keyboard().modifiers() == 0);
    CHECK(drain_keys(input).empty() && input.poll() == 0);
    input.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_mouse_and_tablet() {
  Fixture f;
  {
    SystemInput input(f.pci, f.usb, f.alloc, nullptr);
    FakeStickHost::Stick mouse = device(2), tablet = device(3);
    f.host.plug(0, &mouse);
    f.host.plug(2, &tablet);
    CHECK(input.poll() == 2 && input.mice().count() == 2 && input.keyboards().count() == 0);
    IMouse* m = input.mice().at(0);
    IMouse* t = input.mice().at(1);
    CHECK(m && t && strcmp(m->name(), "USB HID mouse") == 0 && !m->is_absolute());
    CHECK(strcmp(t->name(), "USB HID tablet") == 0 && t->is_absolute() && t->max_x() == 32767 && t->max_y() == 32767);

    f.host.send_report(0, Bytes({0x01, 5, static_cast<uint8_t>(-3)}));   // left button, right 5, up 3
    std::vector<MouseEvent> ev = drain_mouse(input);
    CHECK(ev.size() == 1 && ev[0].dx == 5 && ev[0].dy == -3 && ev[0].buttons == 1 && !ev[0].absolute);
    f.host.send_report(2, Bytes({0x02, 0x00, 0x40, 0x00, 0x20}));   // right button, x = 0x4000, y = 0x2000
    ev = drain_mouse(input);
    CHECK(ev.size() == 1 && ev[0].absolute && ev[0].x == 0x4000 && ev[0].y == 0x2000 && ev[0].buttons == 2);

    // both pulled out with a button held: each lets go of it, in the position it was
    f.host.unplug(0);
    f.host.unplug(2);
    CHECK(input.poll() == 2 && input.mice().count() == 0);
    ev = drain_mouse(input);
    CHECK(ev.size() == 2);
    bool relative_released = false, absolute_released = false;
    for (const MouseEvent& e : ev) {
      if (!e.absolute) relative_released = e.buttons == 0 && e.dx == 0 && e.dy == 0;
      else absolute_released = e.buttons == 0 && e.x == 0x4000 && e.y == 0x2000;
    }
    CHECK(relative_released && absolute_released);

    // a mouse with no button down leaves quietly
    FakeStickHost::Stick quiet = device(2);
    f.host.plug(1, &quiet);
    CHECK(input.poll() == 1);
    f.host.send_report(1, Bytes({0x00, 1, 1}));
    drain_mouse(input);
    f.host.unplug(1);
    CHECK(input.poll() == 1 && drain_mouse(input).empty());
    input.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_several_keyboards() {
  Fixture f;
  {
    SystemInput input(f.pci, f.usb, f.alloc, nullptr);
    FakeStickHost::Stick a = device(1), b = device(1);
    f.host.plug(0, &a);
    f.host.plug(1, &b);
    CHECK(input.poll() == 2 && input.keyboards().count() == 2);
    f.host.send_report(0, keys(0x01, {key::kA}));
    f.host.send_report(1, keys(0, {(key::kA + 1)}));
    std::vector<KeyEvent> ev = drain_keys(input);
    CHECK(ev.size() == 3 && has_key(ev, key::kA, true) && has_key(ev, (key::kA + 1), true) && has_key(ev, key::kLeftControl, true));
    CHECK(input.keyboard().modifiers() == 0x01);
    // the first goes: its keys are released, the second keeps working
    f.host.unplug(0);
    CHECK(input.poll() == 1 && input.keyboards().count() == 1);
    ev = drain_keys(input);
    CHECK(ev.size() == 2 && has_key(ev, key::kA, false) && has_key(ev, key::kLeftControl, false) && !has_key(ev, (key::kA + 1), false));
    CHECK(input.keyboard().modifiers() == 0);
    f.host.send_report(1, keys(0, {}));
    ev = drain_keys(input);
    CHECK(ev.size() == 1 && has_key(ev, (key::kA + 1), false));
    f.host.unplug(1);
    CHECK(input.poll() == 1 && input.keyboards().count() == 0);
    input.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_replaced_before_noticed() {
  Fixture f;
  {
    SystemInput input(f.pci, f.usb, f.alloc, nullptr);
    FakeStickHost::Stick kb = device(1), mouse = device(2);
    f.host.plug(3, &kb);
    CHECK(input.poll() == 1);
    f.host.send_report(3, keys(0, {(key::kA + 2)}));
    drain_keys(input);
    // the keyboard is replaced by a mouse in the same port between two looks
    f.host.unplug(3);
    f.host.plug(3, &mouse);
    CHECK(input.poll() == 2);
    CHECK(input.keyboards().count() == 0 && input.mice().count() == 1);
    std::vector<KeyEvent> ev = drain_keys(input);
    CHECK(ev.size() == 1 && has_key(ev, (key::kA + 2), false));
    // and a keyboard by a keyboard: the old object goes, a new one comes, and it does not inherit the old one's held keys
    FakeStickHost::Stick k1 = device(1), k2 = device(1);
    f.host.unplug(3);
    f.host.plug(0, &k1);
    CHECK(input.poll() == 2);
    f.host.send_report(0, keys(0, {(key::kA + 3)}));
    drain_keys(input);
    f.host.unplug(0);
    f.host.plug(0, &k2);
    CHECK(input.poll() == 2 && input.keyboards().count() == 1);
    ev = drain_keys(input);
    CHECK(ev.size() == 1 && has_key(ev, (key::kA + 3), false));
    f.host.send_report(0, keys(0, {(key::kA + 3)}));
    ev = drain_keys(input);
    CHECK(ev.size() == 1 && has_key(ev, (key::kA + 3), true));   // a fresh press, not a continuation
    input.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_not_taken() {
  Fixture f;
  {
    SystemInput input(f.pci, f.usb, f.alloc, nullptr);
    FakeStickHost::Stick vendor = device(4), stick = make_stick(stick_with_files()), broken = device(1), broken_tablet = device(3);
    broken.fail_start = true;
    broken_tablet.fail_start = true;
    f.host.plug(0, &vendor);
    f.host.plug(1, &stick);
    f.host.plug(2, &broken);
    f.host.plug(3, &broken_tablet);
    CHECK(input.poll() == 0 && input.keyboards().count() == 0 && input.mice().count() == 0);
    f.host.unplug(0);
    f.host.unplug(1);
    f.host.unplug(2);
    f.host.unplug(3);
    CHECK(input.poll() == 0);
    input.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_more_than_fit() {
  // three controllers with four ports each: twelve keyboards, room for eight
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost hosts[3];
  FakeUsb usb;
  for (FakeStickHost& h : hosts) usb.hosts.push_back(&h);
  {
    SystemInput input(pci, usb, alloc, nullptr);
    FakeStickHost::Stick sticks[12];
    for (int i = 0; i < 12; ++i) {
      sticks[i] = device(1);
      hosts[i / 4].plug(i % 4, &sticks[i]);
    }
    CHECK(input.poll() == CompositeKeyboard::kMaxKeyboards && input.keyboards().count() == CompositeKeyboard::kMaxKeyboards);
    // taking one away makes room, but the devices that did not fit before are not retried (their generation was seen)
    hosts[0].unplug(0);
    CHECK(input.poll() == 1 && input.keyboards().count() == CompositeKeyboard::kMaxKeyboards - 1);
    FakeStickHost::Stick again = device(1);
    hosts[0].plug(0, &again);
    CHECK(input.poll() == 1 && input.keyboards().count() == CompositeKeyboard::kMaxKeyboards);
    for (int i = 0; i < 12; ++i) hosts[i / 4].unplug(i % 4);
    input.poll();
    CHECK(input.keyboards().count() == 0);
    input.shutdown();
  }
  CHECK(alloc.live == 0);
}

static void test_shared_bus() {
  // the storage driver and the input driver both watch the same ports; neither takes the other's changes away
  Fixture f;
  {
    SystemStorageDevices storage(f.pci, &f.usb, f.alloc, nullptr);
    SystemInput input(f.pci, f.usb, f.alloc, nullptr);
    FakeStickHost::Stick stick = make_stick(stick_with_files()), kb = device(1);
    f.host.plug(0, &stick);
    f.host.plug(1, &kb);
    StorageEvent e;
    CHECK(input.poll() == 1 && input.keyboards().count() == 1);    // (the input driver looks first and services the controller)
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added && !storage.poll(&e) && storage.count() == 1);
    f.host.unplug(1);
    f.host.unplug(0);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed);   // (now the storage driver looks first)
    CHECK(input.poll() == 1 && input.keyboards().count() == 0);
    e = StorageEvent();
    input.shutdown();
    storage.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

int main() {
  test_keyboard();
  test_mouse_and_tablet();
  test_several_keyboards();
  test_replaced_before_noticed();
  test_not_taken();
  test_more_than_fit();
  test_shared_bus();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("InputHotplugTest: all passed\n");
  return 0;
}
