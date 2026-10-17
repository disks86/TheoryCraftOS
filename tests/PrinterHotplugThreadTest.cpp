// USB printer hot-plug with real threads, run under ThreadSanitizer: printers are plugged in and pulled out (some in the middle of a job) while
// the printers service-style poller runs and two threads keep submitting jobs to whatever printer is there. Every byte a fake printer receives
// must belong to a job meant for it; no race may be reported; every printer must be accounted for; no memory may be left.
#include <time.h>
#include <string>
#include <vector>
#include "FakeUsbSticks.hpp"
#include "Hardware/Printer/SystemPrinters.hpp"
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
  SystemPrinters* printers = nullptr;
  std::vector<FakeStickHost::Stick*> all;      // every printer ever plugged in (plugger only)
  FakeStickHost::Stick* in_port[FakeStickHost::kPorts] = {};
  bool stop = false;
  long plugs = 0, unplugs = 0, submitted = 0, refused = 0, added = 0, removed = 0, lost = 0;
};

static bool stopping(World* w) { return __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE); }

static void plugger(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 99;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (int i = 0; i < 400; ++i) {
    const uint32_t port = rnd() % FakeStickHost::kPorts;
    if (w->in_port[port]) {
      w->host.unplug(port);
      w->in_port[port] = nullptr;
      __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED);
    } else {
      const int k = static_cast<int>(w->all.size());
      char model[16];
      snprintf(model, sizeof model, "P%d", k);
      FakeStickHost::Stick* s = new FakeStickHost::Stick(make_stick(Bytes()));
      s->hid = 5;
      s->device_id = std::string("MFG:Test;MDL:") + model + ";CLS:PRINTER;";
      if (rnd() % 4 == 0) s->unplug_after = static_cast<int>(rnd() % 6);   // now and then it goes by itself in the middle of a job
      w->all.push_back(s);
      w->in_port[port] = s;
      w->host.plug(port, s);
      __atomic_add_fetch(&w->plugs, 1, __ATOMIC_RELAXED);
    }
    nap(100 + rnd() % 1500);
  }
}

static void poller(void* arg) {
  World* w = static_cast<World*>(arg);
  while (!stopping(w)) {
    PrinterEvent e;
    while (w->printers->poll_usb(&e)) {
      if (e.kind == PrinterEvent::Kind::Added) __atomic_add_fetch(&w->added, 1, __ATOMIC_RELAXED);
      else {
        __atomic_add_fetch(&w->removed, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&w->lost, e.lost_jobs, __ATOMIC_RELAXED);
        if (e.printer->present()) fail(__LINE__, "a removed printer is still present");
      }
    }
    w->printers->poll();
    nap(300);
  }
}

// The model name ends in P<k>: the job for that printer is filled with k + 1.
static int index_of(const char* name) {
  const char* p = strrchr(name, 'P');
  return p ? atoi(p + 1) : -1;
}

static void submitter(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 5 + reinterpret_cast<uintptr_t>(arg) % 7;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  while (!stopping(w)) {
    const uint32_t n = w->printers->count();
    SharedPointer<IPrinter> p = n ? w->printers->share(rnd() % n) : SharedPointer<IPrinter>();
    if (p) {
      const int k = index_of(p->name());
      if (k >= 0) {
        Bytes data(2000 + rnd() % 30000, static_cast<uint8_t>(k + 1));
        if (p->submit(ConstByteView(data.data(), data.size()), "job")) __atomic_add_fetch(&w->submitted, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&w->refused, 1, __ATOMIC_RELAXED);
      }
      (void)p->state();
      (void)p->pending_jobs();
      (void)p->statistics();
      p->present();
    }
    nap(200);
  }
}

static void test_stress() {
  World* w = new World;
  w->usb.hosts.push_back(&w->host);
  {
    SystemPrinters printers(w->alloc);
    w->printers = &printers;
    printers.discover_usb(w->usb);
    kernel::Thread* threads[3];
    threads[0] = kernel::spawn(poller, w, "poller");
    threads[1] = kernel::spawn(submitter, w, "submitter1");
    threads[2] = kernel::spawn(submitter, w, "submitter2");
    kernel::Thread* plug = kernel::spawn(plugger, w, "plugger");
    kernel::join(plug);
    for (uint32_t p = 0; p < FakeStickHost::kPorts; ++p)
      if (w->in_port[p]) { w->host.unplug(p); w->in_port[p] = nullptr; __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED); }
    for (int i = 0; i < 300 && printers.count() > 0; ++i) nap(2000);
    __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
    for (int i = 0; i < 3; ++i) kernel::join(threads[i]);
    PrinterEvent e;
    while (printers.poll_usb(&e)) {
      if (e.kind == PrinterEvent::Kind::Added) ++w->added;
      else { ++w->removed; w->lost += e.lost_jobs; }
    }
    e = PrinterEvent();
    CHECK(printers.count() == 0);
    CHECK(w->added == w->removed && w->added <= w->plugs && w->added > 50);   // (a printer that left before it was noticed was never added)
    // no printer received a byte that was meant for another
    long bytes = 0;
    for (size_t k = 0; k < w->all.size(); ++k) {
      for (uint8_t b : w->all[k]->printed) {
        if (b != static_cast<uint8_t>(k + 1)) { fail(__LINE__, "a printer got another printer's data"); break; }
      }
      bytes += static_cast<long>(w->all[k]->printed.size());
    }
    CHECK(w->plugs > 100 && w->submitted > 50 && bytes > 100000);
    printf("  %ld plugs, %ld jobs submitted (%ld refused), %ld bytes printed, %ld jobs lost with their printers\n", w->plugs, w->submitted, w->refused, bytes, w->lost);
    w->printers = nullptr;
    printers.shutdown();
  }
  CHECK(w->alloc.live == 0);
  for (FakeStickHost::Stick* s : w->all) delete s;
  delete w;
}

struct Notes {
  std::mutex m;
  std::vector<std::string> lines;
  static void add(void* self, StringView text) {
    Notes* n = static_cast<Notes*>(self);
    std::lock_guard<std::mutex> g(n->m);
    n->lines.push_back(std::string(text.data(), text.size()));
  }
  bool has(const char* needle) {
    std::lock_guard<std::mutex> g(m);
    for (const std::string& s : lines) if (s.find(needle) != std::string::npos) return true;
    return false;
  }
};

// The service thread: it notices the printer, delivers its job and notices when it leaves.
static void test_service() {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  usb.hosts.push_back(&host);
  FakeStickHost::Stick p = make_stick(Bytes());
  p.hid = 5;
  Notes notes;
  {
    SystemPrinters printers(alloc);
    printers.discover_usb(usb);
    CHECK(printers.start_service(Notes::add, &notes) && printers.start_service());
    host.plug(2, &p);
    bool seen = false;
    for (int i = 0; i < 60 && !seen; ++i) {
      nap(100000);
      seen = printers.count() == 1;
    }
    CHECK(seen && notes.has("added"));
    SharedPointer<IPrinter> printer = printers.share(0);
    Bytes data(50000, 0x7E);
    CHECK(printer && printer->submit(ConstByteView(data.data(), data.size()), "job"));
    bool printed = false;
    for (int i = 0; i < 60 && !printed; ++i) {
      nap(50000);
      printed = printer->statistics().jobs_sent == 1;
    }
    CHECK(printed);
    CHECK(printer->submit(ConstByteView(data.data(), 100), "last"));   // (queued; the pull-out may come first: either way it is accounted for)
    host.unplug(2);
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
      nap(100000);
      gone = printers.count() == 0;
    }
    CHECK(gone && notes.has("removed"));
    CHECK(!printer->present());
    printer.reset();
    printers.stop_service();
    printers.stop_service();
    printers.shutdown();
  }
  CHECK(alloc.live == 0);
  CHECK(p.printed.size() >= 50000);
}

int main() {
  test_stress();
  test_service();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("PrinterHotplugThreadTest: all passed\n");
  return 0;
}
