// Host-side tests for USB printer hot-plug: SystemPrinters::poll_usb() against a fake host controller with USB printers that are plugged in and
// pulled out - a printer that is added gets jobs delivered, one that leaves loses what was queued (and says how much), a port that gets a
// different printer before anyone noticed never passes the old printer's jobs on, and a printer pulled out in the middle of a job is dealt with.
#include <string>
#include <vector>
#include "FakeUsbSticks.hpp"
#include "Hardware/Printer/SystemPrinters.hpp"
#include "Hardware/Printer/Usb/UsbPrinter.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static FakeStickHost::Stick printer_stick(const char* model = "HL-2070N series") {
  FakeStickHost::Stick s = make_stick(Bytes());
  s.hid = 5;
  s.device_id = std::string("MFG:Brother;CMD:PJL,PCL;MDL:") + model + ";CLS:PRINTER;";
  return s;
}

// A printer that is not on USB (stands in for a network printer).
struct FakeNetworkPrinter final : IPrinter {
  PrinterInfo printer_info;
  int polls = 0;
  const char* name() const override { return "network printer"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  const PrinterInfo& info() const override { return printer_info; }
  PrinterState state() const override { return PrinterState::Ready; }
  bool submit(ConstByteView, const char*) override { return true; }
  void poll() override { ++polls; }
  uint32_t pending_jobs() const override { return 0; }
  void cancel_all() override {}
  PrinterStatistics statistics() const override { return {}; }
  uint32_t ipv4_address() const override { return 0x0A000005; }
};

static Bytes job(size_t n, uint8_t fill) { return Bytes(n, fill); }

struct Fixture {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  Fixture() { usb.hosts.push_back(&host); }
};

static void deliver(SystemPrinters& printers, int rounds = 40) {
  for (int i = 0; i < rounds; ++i) printers.poll();
}

static void test_plug_print_unplug() {
  Fixture f;
  {
    SystemPrinters printers(f.alloc);
    CHECK(printers.discover_usb(f.usb) == 0 && printers.count() == 0);
    PrinterEvent e;
    CHECK(!printers.poll_usb(&e));

    FakeStickHost::Stick p = printer_stick();
    f.host.plug(1, &p);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Added && e.id == 1 && e.printer && e.printer->present());
    CHECK(strcmp(e.printer->name(), "Brother HL-2070N series") == 0);
    CHECK(printers.count() == 1 && printers.at(0) == e.printer.get() && printers.default_printer() == e.printer.get() && printers.id(0) == 1);
    CHECK(!printers.poll_usb(&e));

    // a job is delivered (16 KiB at a time)
    const Bytes data = job(40000, 0x5A);
    CHECK(printers.at(0)->submit(ConstByteView(data.data(), data.size()), "first"));
    CHECK(printers.at(0)->state() == PrinterState::Busy && printers.at(0)->pending_jobs() == 1);
    deliver(printers);
    CHECK(printers.at(0)->state() == PrinterState::Ready && p.printed == data);
    CHECK(printers.at(0)->statistics().jobs_sent == 1 && printers.at(0)->statistics().bytes_sent == 40000);
    SharedPointer<IPrinter> held = printers.share(0);

    // pulled out with two jobs waiting
    CHECK(held->submit(ConstByteView(data.data(), 100), "a") && held->submit(ConstByteView(data.data(), 200), "b"));
    f.host.unplug(1);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Removed && e.id == 1 && e.lost_jobs == 2 && e.printer.get() == held.get());
    CHECK(!held->present() && printers.count() == 0 && printers.at(0) == nullptr && !printers.share(0) && printers.default_printer() == nullptr);
    CHECK(!held->submit(ConstByteView(data.data(), 10), "late"));
    CHECK(held->statistics().jobs_failed == 2 && held->pending_jobs() == 0);
    held->poll();
    CHECK(!printers.poll_usb(&e));
    printers.poll();
    CHECK(p.printed.size() == 40000);   // nothing more went out
    e = PrinterEvent();
    held.reset();
    printers.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_at_start_up_and_default() {
  Fixture f;
  {
    FakeStickHost::Stick boot = printer_stick("BOOT");
    f.host.plug(0, &boot);
    SystemPrinters printers(f.alloc);
    UniquePointer<FakeNetworkPrinter> network = UniquePointer<FakeNetworkPrinter>::make(f.alloc);
    FakeNetworkPrinter* raw = network.get();
    CHECK(printers.discover_usb(f.usb) == 1 && printers.count() == 1);
    CHECK(printers.add(UniquePointer<IPrinter>(util::move(network))) && printers.count() == 2);
    CHECK(strstr(printers.at(0)->name(), "BOOT") && printers.at(1)->name()[0] == 'n');   // the USB one was found first: the default
    PrinterEvent e;
    CHECK(!printers.poll_usb(&e));   // (the printer that was plugged in at start-up is not new)

    // a printer plugged in later goes to the end; the default stays
    FakeStickHost::Stick later = printer_stick("LATER");
    f.host.plug(2, &later);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Added && printers.count() == 3);
    CHECK(strstr(printers.at(0)->name(), "BOOT") && strstr(printers.at(2)->name(), "LATER"));
    printers.set_default(2);
    CHECK(strstr(printers.at(0)->name(), "LATER") && strstr(printers.at(1)->name(), "BOOT") && printers.at(2) == raw);

    // the default is pulled out: the next one is the default; the network printer is never touched by hot-plug
    f.host.unplug(2);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Removed && strstr(e.printer->name(), "LATER"));
    CHECK(printers.count() == 2 && strstr(printers.default_printer()->name(), "BOOT") && printers.at(1) == raw);
    e = PrinterEvent();
    f.host.unplug(0);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Removed);
    CHECK(printers.count() == 1 && printers.default_printer() == raw);
    e = PrinterEvent();
    printers.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_yanked_mid_job() {
  Fixture f;
  {
    SystemPrinters printers(f.alloc);
    printers.discover_usb(f.usb);
    FakeStickHost::Stick p = printer_stick();
    p.unplug_after = 3;   // the printer goes during the fourth chunk of the job
    f.host.plug(0, &p);
    PrinterEvent e;
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Added);
    const Bytes data = job(100000, 0x33);
    CHECK(e.printer->submit(ConstByteView(data.data(), data.size()), "big"));
    deliver(printers, 20);
    CHECK(p.printed.size() == 3 * 16384);                 // the chunks that got out before it left
    CHECK(!e.printer->present() && e.printer->pending_jobs() == 1);   // (the job waits to be reported as lost)
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Removed && e.lost_jobs == 1 && e.printer->statistics().jobs_failed == 1);
    e = PrinterEvent();
    printers.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_replaced_before_noticed() {
  Fixture f;
  {
    SystemPrinters printers(f.alloc);
    printers.discover_usb(f.usb);
    FakeStickHost::Stick a = printer_stick("AAA"), b = printer_stick("BBB");
    f.host.plug(3, &a);
    PrinterEvent e;
    CHECK(printers.poll_usb(&e));
    SharedPointer<IPrinter> first = e.printer;
    const Bytes secret = job(5000, 0xA1);
    CHECK(first->submit(ConstByteView(secret.data(), secret.size()), "for A"));
    // the printer in the port is replaced between two looks
    f.host.unplug(3);
    f.host.plug(3, &b);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Removed && e.printer.get() == first.get() && e.lost_jobs == 1);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Added && strstr(e.printer->name(), "BBB") && e.id != 1);
    const Bytes mine = job(3000, 0xB2);
    CHECK(e.printer->submit(ConstByteView(mine.data(), mine.size()), "for B"));
    CHECK(!first->submit(ConstByteView(secret.data(), secret.size()), "again"));
    first->poll();
    deliver(printers);
    CHECK(b.printed == mine && a.printed.empty());   // A's job never reached B
    first.reset();
    e = PrinterEvent();
    printers.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_not_printers() {
  Fixture f;
  {
    SystemPrinters printers(f.alloc);
    printers.discover_usb(f.usb);
    FakeStickHost::Stick stick = make_stick(stick_with_files()), keyboard = make_stick(Bytes());
    keyboard.hid = 1;
    f.host.plug(0, &stick);
    f.host.plug(1, &keyboard);
    PrinterEvent e;
    CHECK(!printers.poll_usb(&e) && printers.count() == 0);
    f.host.unplug(0);
    f.host.unplug(1);
    CHECK(!printers.poll_usb(&e));
    printers.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_job_retry_and_two_printers() {
  Fixture f;
  {
    SystemPrinters printers(f.alloc);
    printers.discover_usb(f.usb);
    FakeStickHost::Stick a = printer_stick("AAA"), b = printer_stick("BBB");
    a.fail_out = 2;   // two bulk transfers fail (and are tried again)
    f.host.plug(0, &a);
    f.host.plug(1, &b);
    PrinterEvent e;
    CHECK(printers.poll_usb(&e) && printers.poll_usb(&e) && printers.count() == 2);
    const Bytes da = job(20000, 0x11), db = job(20000, 0x22);
    CHECK(printers.at(0)->submit(ConstByteView(da.data(), da.size()), "a") && printers.at(1)->submit(ConstByteView(db.data(), db.size()), "b"));
    deliver(printers);
    CHECK(a.printed == da && b.printed == db);
    CHECK(printers.at(0)->statistics().jobs_sent == 1 && printers.at(1)->statistics().jobs_sent == 1);
    // one leaves, the other keeps printing
    f.host.unplug(0);
    CHECK(printers.poll_usb(&e) && e.kind == PrinterEvent::Kind::Removed);
    e = PrinterEvent();
    CHECK(printers.count() == 1 && printers.at(0)->submit(ConstByteView(db.data(), 100), "more"));
    deliver(printers);
    CHECK(b.printed.size() == 20100);
    printers.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

int main() {
  test_plug_print_unplug();
  test_at_start_up_and_default();
  test_yanked_mid_job();
  test_replaced_before_noticed();
  test_not_printers();
  test_job_retry_and_two_printers();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("PrinterHotplugTest: all passed\n");
  return 0;
}
