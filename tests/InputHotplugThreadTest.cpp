// USB HID hot-plug with real threads, run under ThreadSanitizer: one thread plugs keyboards, mice and tablets in and out and makes them send
// reports, one runs SystemInput::poll() the way the "input-hotplug" service does, and two poll the merged keyboard and mouse the way the
// desktop does. Events are followed like a GUI would: a key is down from its press to its release. Whatever the devices were doing when they
// were pulled out, nothing may be left down at the end; no race may be reported; no memory may be left.
#include <time.h>
#include "FakeUsbSticks.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Hardware/SystemInput.hpp"
#include "Kernel/Thread.hpp"

using namespace hardware;

static int g_failed = 0;
static void fail(int line, const char* what) { printf("FAILED line %d: %s\n", line, what); __atomic_add_fetch(&g_failed, 1, __ATOMIC_RELAXED); }
#define CHECK(...) do { if (!(__VA_ARGS__)) fail(__LINE__, #__VA_ARGS__); } while (0)

static void nap(long us) {
  timespec ts{0, us * 1000};
  nanosleep(&ts, nullptr);
}

struct World {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  SystemInput* input = nullptr;
  bool stop = false;
  // the plugger's devices: one per port at most, a different key for each
  FakeStickHost::Stick* in_port[FakeStickHost::kPorts] = {};
  std::vector<FakeStickHost::Stick*> all;   // every device ever made (plugger only)
  long plugs = 0, unplugs = 0, key_events = 0, mouse_events = 0, changes = 0;
  // what the consumers have seen (a key is down between its press and release; only the consumer threads touch these)
  std::mutex seen_mutex;
  bool key_down[256] = {};
  uint8_t last_buttons = 0;
  bool saw_mouse = false;
};

static bool stopping(World* w) { return __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE); }

static void plugger(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 4242;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (int i = 0; i < 600; ++i) {
    const uint32_t port = rnd() % FakeStickHost::kPorts;
    if (w->in_port[port]) {
      w->host.unplug(port);   // (the device object stays allocated: the fake host and the driver may still look at it a moment)
      w->in_port[port] = nullptr;
      __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED);
    } else {
      const int kind = 1 + rnd() % 3;   // keyboard, mouse, tablet
      FakeStickHost::Stick* s = new FakeStickHost::Stick(make_stick(Bytes()));
      s->hid = kind;
      w->in_port[port] = s;
      w->all.push_back(s);
      w->host.plug(port, s);
      __atomic_add_fetch(&w->plugs, 1, __ATOMIC_RELAXED);
    }
    // the devices in the ports do things
    for (int n = 0; n < 4; ++n) {
      const uint32_t p = rnd() % FakeStickHost::kPorts;
      FakeStickHost::Stick* s = w->in_port[p];
      if (!s) continue;
      const bool down = rnd() % 2;
      if (s->hid == 1) {
        Bytes r(8, 0);
        if (down) r[2] = static_cast<uint8_t>(key::kA + p);   // each port has its own key
        w->host.send_report(p, r);
      } else if (s->hid == 2) {
        w->host.send_report(p, Bytes({static_cast<uint8_t>(down ? 1 : 0), static_cast<uint8_t>(rnd()), static_cast<uint8_t>(rnd())}));
      } else {
        w->host.send_report(p, Bytes({static_cast<uint8_t>(down ? 2 : 0), static_cast<uint8_t>(rnd()), 0x10, static_cast<uint8_t>(rnd()), 0x10}));
      }
    }
    nap(100 + rnd() % 1200);
  }
}

static void poller(void* arg) {
  World* w = static_cast<World*>(arg);
  while (!stopping(w)) {
    const uint32_t n = w->input->poll();
    __atomic_add_fetch(&w->changes, n, __ATOMIC_RELAXED);
    nap(200);
  }
}

static void consume(World* w) {
  std::lock_guard<std::mutex> g(w->seen_mutex);   // (polling and recording as one step keeps the order the events came in)
  KeyEvent k;
  while (w->input->keyboard().poll(&k)) {
    w->key_down[k.usage] = k.pressed;
    __atomic_add_fetch(&w->key_events, 1, __ATOMIC_RELAXED);
  }
  MouseEvent m;
  while (w->input->mouse().poll(&m)) {
    w->last_buttons = m.buttons;
    w->saw_mouse = true;
    __atomic_add_fetch(&w->mouse_events, 1, __ATOMIC_RELAXED);
  }
  (void)w->input->keyboard().modifiers();
  (void)w->input->keyboards().count();
  (void)w->input->mice().count();
}

static void consumer(void* arg) {
  World* w = static_cast<World*>(arg);
  while (!stopping(w)) {
    consume(w);
    nap(100);
  }
}

static void test_stress() {
  World* w = new World;
  w->usb.hosts.push_back(&w->host);
  {
    SystemInput input(w->pci, w->usb, w->alloc, nullptr);
    w->input = &input;
    kernel::Thread* threads[3];
    threads[0] = kernel::spawn(poller, w, "poller");
    threads[1] = kernel::spawn(consumer, w, "consumer1");
    threads[2] = kernel::spawn(consumer, w, "consumer2");
    kernel::Thread* plug = kernel::spawn(plugger, w, "plugger");
    kernel::join(plug);
    for (uint32_t p = 0; p < FakeStickHost::kPorts; ++p)
      if (w->in_port[p]) { w->host.unplug(p); w->in_port[p] = nullptr; __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED); }
    for (int i = 0; i < 200 && (input.keyboards().count() > 0 || input.mice().count() > 0); ++i) nap(2000);
    nap(20000);   // (the consumers collect the last releases)
    __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
    for (int i = 0; i < 3; ++i) kernel::join(threads[i]);
    consume(w);

    CHECK(input.keyboards().count() == 0 && input.mice().count() == 0);
    int down = 0;
    for (int i = 0; i < 256; ++i) down += w->key_down[i];
    CHECK(down == 0);                                  // nothing is left pressed
    CHECK(!w->saw_mouse || w->last_buttons == 0);      // nor a button
    CHECK(input.keyboard().modifiers() == 0);
    CHECK(w->plugs > 150 && w->unplugs == w->plugs);
    CHECK(w->key_events > 50 && w->mouse_events > 50);
    printf("  %ld plugs, %ld devices taken up or dropped, %ld key and %ld mouse events\n", w->plugs, w->changes, w->key_events, w->mouse_events);
    w->input = nullptr;
    input.shutdown();
  }
  CHECK(w->alloc.live == 0);
  for (FakeStickHost::Stick* s : w->all) delete s;
  delete w;
}

// The service thread itself.
static void test_service() {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  usb.hosts.push_back(&host);
  FakeStickHost::Stick kb = make_stick(Bytes());
  kb.hid = 1;
  {
    SystemInput input(pci, usb, alloc, nullptr);
    CHECK(input.start_service() && input.start_service());
    host.plug(2, &kb);
    bool seen = false;
    for (int i = 0; i < 60 && !seen; ++i) {
      nap(100000);
      seen = input.keyboards().count() == 1;
    }
    CHECK(seen);
    host.unplug(2);
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
      nap(100000);
      gone = input.keyboards().count() == 0;
    }
    CHECK(gone);
    input.stop_service();
    input.stop_service();
    input.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_stress();
  test_service();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("InputHotplugThreadTest: all passed\n");
  return 0;
}
