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

// Host-side tests for printing: PackBits, halftoning, the PCL/PJL job builder (the output is parsed back and compared with the
// page), text layout, the Brother HL-2070N's limits, and delivery over raw TCP port 9100 and LPD to scripted servers on the fake
// network.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "Hardware/Printer/Brother/BrotherHl2070n.hpp"
#include "Hardware/Printer/PrintJob.hpp"
#include "Hardware/Printer/SystemPrinters.hpp"
#include "Hardware/Printer/Usb/UsbPrinter.hpp"
#include "NetworkHarness.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static std::vector<uint8_t> unpackbits(const uint8_t* d, uint32_t n) {
  std::vector<uint8_t> out;
  for (uint32_t i = 0; i < n;) {
    const int8_t c = static_cast<int8_t>(d[i++]);
    if (c >= 0) { for (int k = 0; k <= c && i < n; ++k) out.push_back(d[i++]); }
    else if (c != -128) { for (int k = 0; k < 1 - c; ++k) out.push_back(d[i]); ++i; }
  }
  return out;
}

static void test_packbits() {
  uint32_t seed = 5;
  for (int round = 0; round < 500; ++round) {
    uint8_t data[300], packed[300 + 3];
    const uint32_t n = 1 + (seed = seed * 1664525 + 1013904223) % 299;
    for (uint32_t i = 0; i < n; ++i) {
      seed = seed * 1664525 + 1013904223;
      data[i] = (round % 3 == 0) ? static_cast<uint8_t>(seed >> 24) : static_cast<uint8_t>((seed >> 24) % (round % 3 == 1 ? 3 : 2));
    }
    const uint32_t m = packbits(data, n, packed);
    CHECK(m <= n + n / 128 + 1);
    const std::vector<uint8_t> back = unpackbits(packed, m);
    CHECK(back.size() == n && memcmp(back.data(), data, n) == 0);
  }
  uint8_t zeros[100] = {0}, out[8];
  CHECK(packbits(zeros, 100, out) == 2);  // one repeat code
}

static graphics::Surface make_surface(ArrayView<uint32_t> store, uint32_t w, uint32_t h, uint32_t argb) {
  for (uint32_t& p : store) p = argb;
  graphics::Surface s;
  s.pixels = store;
  s.width = w;
  s.height = h;
  s.pitch = w;
  return s;
}

static void test_pages_and_halftone() {
  TestAllocator a;
  PageBitmap page(a);
  CHECK(page.create_for_paper(PaperSize::A4, 600, 42, false));
  CHECK(page.width() > 4700 && page.width() < 4800 && page.height() > 6800 && page.height() < 6900);
  CHECK(!page.create(0, 10) && !page.create(100000, 10));
  CHECK(page.create(64, 40));
  uint32_t px[100 * 50];
  ArrayView<uint32_t> store(px, 100 * 50);
  {
    graphics::Surface white = make_surface(store, 100, 50, 0xFFFFFFFF);
    for (Halftone h : {Halftone::Threshold, Halftone::Ordered, Halftone::ErrorDiffusion}) {
      CHECK(render_surface(white, &page, Fit::Stretch, h, a) && page.black_dots() == 0);
    }
    graphics::Surface black = make_surface(store, 100, 50, 0xFF000000);
    for (Halftone h : {Halftone::Threshold, Halftone::Ordered, Halftone::ErrorDiffusion}) {
      CHECK(render_surface(black, &page, Fit::Stretch, h, a) && page.black_dots() == 64 * 40);
    }
    graphics::Surface grey = make_surface(store, 100, 50, 0xFF808080);
    CHECK(render_surface(grey, &page, Fit::Stretch, Halftone::Threshold, a) && page.black_dots() == 0);  // 128 is not below 128
    CHECK(render_surface(grey, &page, Fit::Stretch, Halftone::Ordered, a));
    const uint64_t ordered = page.black_dots();
    CHECK(ordered > 64 * 40 * 45 / 100 && ordered < 64 * 40 * 55 / 100);
    CHECK(render_surface(grey, &page, Fit::Stretch, Halftone::ErrorDiffusion, a));
    const uint64_t diffused = page.black_dots();
    CHECK(diffused > 64 * 40 * 46 / 100 && diffused < 64 * 40 * 54 / 100);
    graphics::Surface clear = make_surface(store, 100, 50, 0x00000000);  // fully transparent: white
    CHECK(render_surface(clear, &page, Fit::Stretch, Halftone::Threshold, a) && page.black_dots() == 0);
    // A 100x50 picture on a 64x40 page keeps its proportions: 64x32 dots, centred, so the top and bottom rows stay white.
    black = make_surface(store, 100, 50, 0xFF000000);  // (the surfaces share one store)
    CHECK(render_surface(black, &page, Fit::Contain, Halftone::Threshold, a) && page.black_dots() == 64 * 32);
    CHECK(!page.get(0, 0) && page.get(0, 4) && page.get(63, 35) && !page.get(0, 39));
  }
  // A picture bigger than the page is averaged: a 2x2 checker of black and white at half scale is mid grey.
  {
    uint32_t big[128 * 80];
    for (uint32_t y = 0; y < 80; ++y)
      for (uint32_t x = 0; x < 128; ++x) big[y * 128 + x] = ((x ^ y) & 1) ? 0xFFFFFFFF : 0xFF000000;
    graphics::Surface s;
    s.pixels = ArrayView<uint32_t>(big, 128 * 80);
    s.width = 128; s.height = 80; s.pitch = 128;
    CHECK(render_surface(s, &page, Fit::Stretch, Halftone::Threshold, a));  // each dot averages 2x2 = 128: not black
    CHECK(page.black_dots() == 64 * 40);  // 127.5 rounds down to 127: black
    CHECK(render_surface(s, &page, Fit::Stretch, Halftone::ErrorDiffusion, a));
  }
  }

// Finds `needle` in `hay` from `from`; npos if absent.
static size_t find(const std::string& hay, const std::string& needle, size_t from = 0) { return hay.find(needle, from); }

struct ParsedPage {
  uint32_t rows = 0;
  std::vector<std::vector<uint8_t>> data;
};

// Reads the raster rows (ESC * b # W ...) of a PCL job.
static std::vector<ParsedPage> parse_pages(const std::string& job) {
  std::vector<ParsedPage> pages;
  size_t i = 0;
  ParsedPage cur;
  bool in = false;
  while (i < job.size()) {
    if (job.compare(i, 5, "\x1b*r1A") == 0) { in = true; cur = ParsedPage(); i += 5; continue; }
    if (in && job.compare(i, 3, "\x1b*b") == 0) {
      size_t j = i + 3;
      uint32_t n = 0;
      bool digits = false;
      while (j < job.size() && job[j] >= '0' && job[j] <= '9') { n = n * 10 + (job[j] - '0'); ++j; digits = true; }
      if (digits && j < job.size() && job[j] == 'W') {
        const uint8_t* d = reinterpret_cast<const uint8_t*>(job.data()) + j + 1;
        cur.data.push_back(unpackbits(d, n));
        ++cur.rows;
        i = j + 1 + n;
        continue;
      }
    }
    if (in && job.compare(i, 4, "\x1b*rB") == 0) { pages.push_back(cur); in = false; }
    ++i;
  }
  return pages;
}

static void test_pcl_job() {
  TestAllocator a;
  const PrinterInfo& info = BrotherHl2070n::model_info();
  CHECK(info.supports(PrinterLanguage::Pcl) && info.supports(PrinterLanguage::PostScript) && info.supports(PaperSize::A4) && !info.supports(PaperSize::Monarch));
  CHECK(!info.duplex && !info.color && info.max_dpi == 1200 && info.pages_per_minute == 23);

  JobSettings s;
  s.name = "My \"test\" job";
  s.copies = 3;
  s.toner_save = true;
  s.source = PaperSource::Manual;
  PrintJob job(a, info, s);
  PageBitmap page(a);
  CHECK(page.create(job.page_width(), job.page_height()));
  for (uint32_t y = 0; y < page.height(); ++y)
    for (uint32_t x = 0; x < page.width(); ++x)
      if ((y > 100 && y < 120 && x < 500) || (y % 97 == 0 && x % 3 == 0) || (y > 3000 && y < 3010)) page.set(x, y, true);
  CHECK(job.begin() && job.add_page(page) && job.add_page(page) && job.finish() && !job.failed() && job.pages() == 2);
  const std::string out(reinterpret_cast<const char*>(job.bytes().data()), job.bytes().size());
  CHECK(out.compare(0, 9, "\x1b%-12345X") == 0 && find(out, "@PJL JOB NAME=\"My test job\"\r\n") != std::string::npos);
  CHECK(find(out, "@PJL SET RESOLUTION=600\r\n") != std::string::npos && find(out, "@PJL SET ECONOMODE=ON\r\n") != std::string::npos);
  CHECK(find(out, "@PJL ENTER LANGUAGE=PCL\r\n\x1b" "E\x1b&u600D") != std::string::npos);
  CHECK(find(out, "\x1b&l26A\x1b&l0O\x1b&l2H\x1b&l3X\x1b*t600R") != std::string::npos);  // A4, portrait, manual feed, 3 copies
  CHECK(out.size() > 20 && out.compare(out.size() - 9, 9, "\x1b%-12345X") == 0 && find(out, "@PJL EOJ\r\n") != std::string::npos);
  const std::vector<ParsedPage> pages = parse_pages(out);
  CHECK(pages.size() == 2);
  for (const ParsedPage& p : pages) {
    CHECK(p.rows == page.height());
    bool same = p.rows == page.height();
    for (uint32_t y = 0; same && y < page.height(); ++y) {
      std::vector<uint8_t> want(page.row(y), page.row(y) + page.stride());
      std::vector<uint8_t> got = p.data[y];
      got.resize(page.stride(), 0);  // trailing white bytes are not sent
      same = want == got;
    }
    CHECK(same);
  }
  // Refusals: unsupported paper, resolution, copies, language mismatch, a page that is too big.
  {
    JobSettings bad;
    bad.paper = PaperSize::Monarch;
    PrintJob j(a, info, bad);
    CHECK(!j.begin());
    JobSettings dpi;
    dpi.dpi = 2400;
    PrintJob j2(a, info, dpi);
    CHECK(!j2.begin());
    JobSettings none;
    none.copies = 0;
    PrintJob j3(a, info, none);
    CHECK(!j3.begin());
    PrintJob j4(a, info, JobSettings());
    CHECK(!j4.add_page(page));  // before begin()
    CHECK(j4.begin());
    PageBitmap huge(a);
    CHECK(huge.create(j4.page_width() + 8, 10) && !j4.add_page(huge) && j4.failed());
    JobSettings ps;
    ps.language = PrinterLanguage::PostScript;
    PrintJob j5(a, info, ps);
    CHECK(j5.begin() && !j5.add_page(page));
  }
  // 1200 dpi doubles the dots; 300 dpi halves them.
  {
    JobSettings hi;
    hi.dpi = 1200;
    PrintJob j(a, info, hi);
    JobSettings lo;
    lo.dpi = 300;
    PrintJob l(a, info, lo);
    CHECK(j.page_width() > 9400 && j.page_width() < 9600 && l.page_width() > 2350 && l.page_width() < 2400);
    CHECK(j.begin() && j.finish());
    const std::string o(reinterpret_cast<const char*>(j.bytes().data()), j.bytes().size());
    CHECK(find(o, "@PJL SET RESOLUTION=1200") != std::string::npos && find(o, "\x1b&u1200D") != std::string::npos);
  }
  // PostScript.
  {
    JobSettings ps;
    ps.language = PrinterLanguage::PostScript;
    PrintJob j(a, info, ps);
    const char prog[] = "%!PS\n/Courier findfont 12 scalefont setfont 72 700 moveto (Hello) show showpage\n";
    CHECK(j.begin() && j.add_postscript(ConstByteView(reinterpret_cast<const uint8_t*>(prog), sizeof prog - 1)) && j.finish());
    const std::string o(reinterpret_cast<const char*>(j.bytes().data()), j.bytes().size());
    CHECK(find(o, "@PJL ENTER LANGUAGE=POSTSCRIPT\r\n%!PS") != std::string::npos && find(o, "showpage\n\x04\x1b%-12345X@PJL EOJ") != std::string::npos);
    CHECK(!j.add_text("x"));
  }
}

static void test_text() {
  TestAllocator a;
  const PrinterInfo& info = BrotherHl2070n::model_info();
  JobSettings s;
  PrintJob job(a, info, s);
  CHECK(job.begin());
  std::string text = "Caf\xC3\xA9\tx\n";
  for (int i = 0; i < 130; ++i) text += "line " + std::to_string(i) + "\n";
  text += std::string(200, 'W') + "\n";   // wraps
  text += "\xE2\x82\xAC\n";                // a euro sign: not in Latin-1
  CHECK(job.add_text(StringView(text.c_str(), text.size())) && job.finish());
  const std::string out(reinterpret_cast<const char*>(job.bytes().data()), job.bytes().size());
  CHECK(find(out, "Caf\xE9    x") != std::string::npos);                    // UTF-8 in, Latin-1 out; tab to column 8
  CHECK(find(out, "(19U") != std::string::npos && find(out, "&k2G") != std::string::npos);
  size_t ff = 0, pos = 0;
  while ((pos = out.find('\f', pos)) != std::string::npos) { ++ff; ++pos; }
  CHECK(job.pages() == 2 && ff == job.pages());
  CHECK(find(out, std::string(78, 'W') + "\r\n" + std::string(78, 'W')) != std::string::npos);  // wrapped at 78 columns
  CHECK(find(out, "?\r\n") != std::string::npos);
  // An empty text prints nothing.
  PrintJob empty(a, info, s);
  CHECK(empty.begin() && empty.add_text("") && empty.pages() == 0);
}

// Runs the fake network until `done` or a deadline, also polling the printer.
static bool pump(Lan& lan, NetworkPrinter& printer, const std::function<bool()>& done, int max_ms = 60000) {
  for (int i = 0; i < max_ms; ++i) {
    if (done()) return true;
    lan.step();
    printer.poll();
  }
  return done();
}

static void test_raw_delivery() {
  Lan lan;
  lan.configure_static();
  Server server(lan, 9100);
  BrotherHl2070n printer(lan.a, Ipv4Address::make(10, 0, 0, 2), lan.alloc);
  CHECK(printer.initialize() && printer.state() == PrinterState::Ready && printer.pending_jobs() == 0);
  std::string job(30000, 'x');
  for (size_t i = 0; i < job.size(); ++i) job[i] = static_cast<char>(i * 7);
  CHECK(printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>(job.data()), job.size()), "raw job") && printer.pending_jobs() == 1 && printer.state() == PrinterState::Busy);
  CHECK(!printer.submit(ConstByteView(), "empty"));
  CHECK(pump(lan, printer, [&] { return printer.pending_jobs() == 0; }));
  lan.step(50);
  CHECK(server.received == job);
  CHECK(printer.state() == PrinterState::Ready && printer.statistics().jobs_sent == 1 && printer.statistics().bytes_sent == job.size());
  // The queue holds eight jobs; they arrive one after another.
  server.received.clear();
  std::vector<SharedPointer<ITcpSocket>> extra;   // every job is a new connection
  server.app = [&](Server& s) {
    while (SharedPointer<ITcpSocket> c = s.listener->accept()) extra.push_back(c);
    for (auto& c : extra) { uint8_t b[256]; for (uint32_t n; (n = c->receive(ByteView(b))) > 0;) s.received.append(reinterpret_cast<char*>(b), n); }
  };
  if (server.conn) { uint8_t b[64]; while (server.conn->receive(ByteView(b)) > 0) {} }
  for (int i = 0; i < 8; ++i) { const char c = static_cast<char>('a' + i); CHECK(printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>(&c), 1), "j")); }
  CHECK(!printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>("z"), 1), "overflow"));
  CHECK(pump(lan, printer, [&] { return printer.pending_jobs() == 0; }));
  lan.step(50);
  CHECK(server.received == "abcdefgh" && printer.statistics().jobs_sent == 9);
  printer.shutdown();
}

static void test_lpd_delivery() {
  Lan lan;
  lan.configure_static();
  Server server(lan, 515);
  // A scripted lpd: acknowledges the receive-job command, the control file subcommand and body, the data file subcommand and body.
  size_t consumed = 0;
  int step = 0;
  size_t need = 0;
  std::string command, control, data;
  server.app = [&](Server& s) {
    for (int guard = 0; guard < 8 && s.conn; ++guard) {
      const std::string& r = s.received;
      if (step == 0 || step == 1 || step == 3) {
        const size_t nl = r.find('\n', consumed);
        if (nl == std::string::npos) return;
        const std::string line = r.substr(consumed, nl + 1 - consumed);
        consumed = nl + 1;
        if (step == 0) command = line;
        if (step == 1 || step == 3) need = static_cast<size_t>(atoi(line.c_str() + 1)) + 1;
        const uint8_t ack = 0;
        s.conn->send(ConstByteView(&ack, 1));
        ++step;
      } else {
        if (r.size() - consumed < need) return;
        (step == 2 ? control : data) = r.substr(consumed, need - 1);
        consumed += need;
        const uint8_t ack = 0;
        s.conn->send(ConstByteView(&ack, 1));
        ++step;
      }
    }
  };
  BrotherHl2070n printer(lan.a, Ipv4Address::make(10, 0, 0, 2), lan.alloc, PrintProtocol::Lpd);
  std::string job(5000, 'q');
  for (size_t i = 0; i < job.size(); i += 13) job[i] = '\0';
  CHECK(printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>(job.data()), job.size()), "lpd job"));
  CHECK(pump(lan, printer, [&] { return printer.pending_jobs() == 0; }));
  CHECK(printer.statistics().jobs_sent == 1 && printer.state() == PrinterState::Ready);
  CHECK(command == "\x02" "BINARY_P1\n");
  CHECK(control.find("Htheorycraftos\n") == 0 && control.find("\nPuser\n") != std::string::npos && control.find("\nJlpd_job\n") != std::string::npos);
  CHECK(control.find("\nldfA001theorycraftos\n") != std::string::npos && step == 5 && data == job);
}

static void test_failures_and_registry() {
  Lan lan;
  lan.configure_static();   // nothing listens on 9100: the connection is refused
  BrotherHl2070n printer(lan.a, Ipv4Address::make(10, 0, 0, 2), lan.alloc);
  CHECK(printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>("hello"), 5), "doomed"));
  CHECK(pump(lan, printer, [&] { return printer.pending_jobs() == 0; }, 120000));
  CHECK(printer.state() == PrinterState::Error && printer.statistics().jobs_failed == 1 && printer.statistics().jobs_sent == 0);
  Server server(lan, 9100);   // now it listens: the next job goes through and clears the error
  CHECK(printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>("again"), 5), "ok"));
  CHECK(pump(lan, printer, [&] { return printer.pending_jobs() == 0; }));
  lan.step(50);
  CHECK(printer.state() == PrinterState::Ready && server.received == "again");
  // cancel_all drops the queue.
  CHECK(printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>("x"), 1), "a") && printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>("y"), 1), "b"));
  printer.cancel_all();
  CHECK(printer.pending_jobs() == 0);

  SystemPrinters printers(lan.alloc);
  CHECK(printers.count() == 0 && printers.default_printer() == nullptr);
  CHECK(printers.add(UniquePointer<IPrinter>(UniquePointer<BrotherHl2070n>::make(lan.alloc, lan.a, Ipv4Address::make(10, 0, 0, 2), lan.alloc))));
  CHECK(printers.add(UniquePointer<IPrinter>(UniquePointer<BrotherHl2070n>::make(lan.alloc, lan.a, Ipv4Address::make(10, 0, 0, 3), lan.alloc, PrintProtocol::Lpd))));
  CHECK(printers.count() == 2 && strcmp(printers.default_printer()->name(), "Brother HL-2070N") == 0);
  IPrinter* second = printers.at(1);
  printers.set_default(1);
  CHECK(printers.default_printer() == second);
  printers.poll();
  printers.shutdown();
  CHECK(printers.count() == 0);
}


// ---- USB ----

// A host controller with scripted devices: port 0 a printer (one unidirectional and one bidirectional setting of its interface),
// port 1 a mass-storage stick. Everything the driver sends is recorded.
struct FakeUsbHost final : IUsbHostController {
  struct Control { uint8_t type, request; uint16_t value, index; };
  std::vector<uint8_t> printer_config, stick_config;
  std::string device_id = "MFG:Brother;CMD:PJL,PCL,PCLXL,POSTSCRIPT;MDL:HL-2070N series;CLS:PRINTER;";
  uint8_t port_status = 0x18;   // selected, no error
  std::string printed, reply;
  std::vector<Control> controls;
  std::vector<uint8_t> started;
  std::vector<uint32_t> out_sizes;
  int fail_out = 0;             // bulk OUT transfers still to fail
  int halts_cleared = 0, endpoint_resets = 0;

  FakeUsbHost(bool bidirectional = true, bool with_alternates = true) {
    auto put = [](std::vector<uint8_t>& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b.begin(), b.end()); };
    std::vector<uint8_t>& c = printer_config;
    put(c, {9, 2, 0, 0, 1, 1, 0, 0x80, 50});
    if (bidirectional && with_alternates) {
      put(c, {9, 4, 0, 0, 1, 7, 1, 1, 0});   // interface 0, setting 0: unidirectional
      put(c, {7, 5, 0x01, 2, 0x00, 0x02, 0});
      put(c, {9, 4, 0, 1, 2, 7, 1, 2, 0});   // setting 1: bidirectional
      put(c, {7, 5, 0x01, 2, 0x00, 0x02, 0});
      put(c, {7, 5, 0x82, 2, 0x00, 0x02, 0});
    } else if (bidirectional) {
      put(c, {9, 4, 0, 0, 2, 7, 1, 2, 0});
      put(c, {7, 5, 0x01, 2, 0x00, 0x02, 0});
      put(c, {7, 5, 0x82, 2, 0x00, 0x02, 0});
    } else {
      put(c, {9, 4, 0, 0, 1, 7, 1, 1, 0});
      put(c, {7, 5, 0x01, 2, 0x00, 0x02, 0});
    }
    c[2] = static_cast<uint8_t>(c.size());
    put(stick_config, {9, 2, 32, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 2, 8, 6, 0x50, 0, 7, 5, 0x81, 2, 0, 2, 0, 7, 5, 0x02, 2, 0, 2, 0});
  }
  const char* name() const override { return "fake xhci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 2; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = true; out->speed = UsbSpeed::High; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override { *out = UsbDeviceDescriptor(); return true; }
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override {
    const std::vector<uint8_t>& c = port == 0 ? printer_config : stick_config;
    const size_t n = c.size() < buffer.size() ? c.size() : buffer.size();
    memcpy(buffer.data(), c.data(), n);
    *length = static_cast<uint16_t>(c.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return true; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index) override {
    controls.push_back({type, request, value, index});
    if (request == 1) ++halts_cleared;
    return true;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    controls.push_back({type, request, value, index});
    if (type != 0xA1) return false;
    if (request == 0) {
      const uint16_t total = static_cast<uint16_t>(device_id.size() + 2);
      std::vector<uint8_t> r = {static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total)};
      r.insert(r.end(), device_id.begin(), device_id.end());
      memcpy(buffer.data(), r.data(), r.size() < buffer.size() ? r.size() : buffer.size());
      return true;
    }
    if (request == 1) { buffer[0] = port_status; return true; }
    return false;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t endpoint, uint16_t) override { started.push_back(endpoint); return true; }
  bool bulk_transfer(uint32_t, uint8_t endpoint, ByteView data, uint32_t* transferred) override {
    // The driver must hand over a block that does not cross a 64 KiB boundary.
    const uintptr_t start = reinterpret_cast<uintptr_t>(data.data());
    if ((start >> 16) != ((start + data.size() - 1) >> 16)) return false;
    if (endpoint & 0x80) {
      const size_t n = reply.size() < data.size() ? reply.size() : data.size();
      memcpy(data.data(), reply.data(), n);
      reply.erase(0, n);
      *transferred = static_cast<uint32_t>(n);
      return true;
    }
    if (fail_out > 0) { --fail_out; return false; }
    printed.append(reinterpret_cast<const char*>(data.data()), data.size());
    out_sizes.push_back(static_cast<uint32_t>(data.size()));
    *transferred = static_cast<uint32_t>(data.size());
    return true;
  }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { ++endpoint_resets; return true; }
};

struct FakeUsbBus final : IUsbBus {
  FakeUsbHost* host;
  explicit FakeUsbBus(FakeUsbHost* h) : host(h) {}
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 2; }
  uint32_t controller_count() const override { return 1; }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t) const override { return 2; }
  bool port_info(uint32_t, uint32_t p, UsbPortInfo* o) const override { return host->port_info(p, o); }
  bool device_descriptor(uint32_t, uint32_t p, UsbDeviceDescriptor* o) const override { return host->device_descriptor(p, o); }
  IUsbHostController* host_controller(uint32_t c) override { return c == 0 ? host : nullptr; }
};

static void test_device_id() {
  PrinterIdentity a = parse_device_id(ConstByteView(reinterpret_cast<const uint8_t*>("MFG:Brother;CMD:PJL,PCL,PCLXL,POSTSCRIPT;MDL:HL-2070N series;CLS:PRINTER;"), 69));
  CHECK(strcmp(a.manufacturer, "Brother") == 0 && strcmp(a.model, "HL-2070N series") == 0 && a.pjl && a.pcl_xl && a.languages == 3);
  const char longform[] = "MANUFACTURER:ACME;COMMAND SET:PCL5E,ESCP2;MODEL:Jet 9;";
  PrinterIdentity b = parse_device_id(ConstByteView(reinterpret_cast<const uint8_t*>(longform), sizeof longform - 1));
  CHECK(strcmp(b.manufacturer, "ACME") == 0 && strcmp(b.model, "Jet 9") == 0 && b.languages == 1 && !b.pjl);
  const char spaced[] = "MFG:A; MDL:B ;CMD:PS;;junk;:;MDL";
  PrinterIdentity c = parse_device_id(ConstByteView(reinterpret_cast<const uint8_t*>(spaced), sizeof spaced - 1));
  CHECK(strcmp(c.manufacturer, "A") == 0 && strcmp(c.model, "B ") == 0 && c.languages == 2);
  PrinterIdentity d = parse_device_id(ConstByteView());
  CHECK(d.manufacturer[0] == 0 && d.languages == 0);
  std::string long_value = "MDL:" + std::string(200, 'x') + ";";
  PrinterIdentity e = parse_device_id(ConstByteView(reinterpret_cast<const uint8_t*>(long_value.data()), long_value.size()));
  CHECK(strlen(e.model) == sizeof e.model - 1);
}

static void test_usb_printer() {
  TestAllocator a;
  FakeUsbHost host;
  FakeUsbBus bus(&host);
  List<UniquePointer<IPrinter>> found(a);
  UsbPrinter::discover(bus, a, found);
  CHECK(found.count() == 1);   // the stick is not a printer; the two settings of the interface make one printer
  if (found.count() != 1) return;
  IPrinter& p = *found[0];
  UsbPrinter& usb = static_cast<UsbPrinter&>(p);
  CHECK(strcmp(p.name(), "Brother HL-2070N series") == 0 && usb.bidirectional() && p.info().max_dpi == 1200 && p.info().supports(PaperSize::A4));
  CHECK(host.started.size() == 2 && host.started[0] == 0x01 && host.started[1] == 0x82);
  bool set_interface = false, got_id = false;
  for (const auto& c : host.controls) {
    if (c.type == 0x01 && c.request == 0x0B && c.value == 1 && c.index == 0) set_interface = true;   // the bidirectional setting
    if (c.type == 0xA1 && c.request == 0 && c.index == 0x0001) got_id = true;                       // interface 0, setting 1
  }
  CHECK(set_interface && got_id);
  CHECK(p.state() == PrinterState::Ready);

  // A real job, built for this printer, goes out in chunks.
  JobSettings s;
  PrintJob job(a, p.info(), s);
  PageBitmap page(a);
  CHECK(page.create(job.page_width(), job.page_height()));
  for (uint32_t y = 0; y < page.height(); y += 3)
    for (uint32_t x = (y * 7) % 100; x < page.width(); x += 101) page.set(x, y, true);
  CHECK(job.begin() && job.add_page(page) && job.finish());
  const std::string bytes(reinterpret_cast<const char*>(job.bytes().data()), job.bytes().size());
  CHECK(bytes.size() > 3 * UsbPrinter::kChunk);
  CHECK(p.submit(job.bytes(), "page") && p.pending_jobs() == 1 && p.state() == PrinterState::Busy);
  int polls = 0;
  while (p.pending_jobs() && polls < 1000) { p.poll(); ++polls; }
  CHECK(host.printed == bytes && polls == static_cast<int>((bytes.size() + UsbPrinter::kChunk - 1) / UsbPrinter::kChunk));
  CHECK(p.statistics().jobs_sent == 1 && p.statistics().bytes_sent == bytes.size() && p.state() == PrinterState::Ready);
  CHECK(!p.submit(ConstByteView(), "empty"));

  // A transfer that fails twice is retried (clearing the halt each time); one that always fails drops the job and resets the printer.
  host.printed.clear();
  host.fail_out = 2;
  const char small[] = "small job";
  CHECK(p.submit(ConstByteView(reinterpret_cast<const uint8_t*>(small), sizeof small - 1), "small"));
  for (int i = 0; i < 10 && p.pending_jobs(); ++i) p.poll();
  CHECK(host.printed == "small job" && host.halts_cleared == 2 && host.endpoint_resets == 2 && p.state() == PrinterState::Ready);
  host.printed.clear();
  host.fail_out = 1000;
  CHECK(p.submit(ConstByteView(reinterpret_cast<const uint8_t*>(small), sizeof small - 1), "doomed"));
  for (int i = 0; i < 10 && p.pending_jobs(); ++i) p.poll();
  bool reset = false;
  for (const auto& c : host.controls) if (c.type == 0x21 && c.request == 2 && c.index == 0) reset = true;
  CHECK(p.pending_jobs() == 0 && p.state() == PrinterState::Error && p.statistics().jobs_failed == 1 && reset && host.printed.empty());
  host.fail_out = 0;
  CHECK(p.submit(ConstByteView(reinterpret_cast<const uint8_t*>(small), sizeof small - 1), "again"));
  for (int i = 0; i < 10 && p.pending_jobs(); ++i) p.poll();
  CHECK(host.printed == "small job" && p.state() == PrinterState::Ready);

  // Status and the way back.
  UsbPrinterStatus st;
  CHECK(usb.read_status(&st) && st.ok());
  host.port_status = 0x28;   // paper empty (bit 5), not selected (bit 4 clear), no error
  CHECK(usb.read_status(&st) && st.paper_empty && !st.selected && st.no_error && !st.ok());
  host.port_status = 0x00;
  CHECK(usb.read_status(&st) && !st.no_error);
  host.reply = "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"READY\"\r\nONLINE=TRUE\r\n";
  uint8_t back[256];
  uint32_t n = 0;
  CHECK(usb.read_back(ByteView(back), &n) && n == host.reply.size() + n - host.reply.size() && n > 20 && memcmp(back, "@PJL INFO STATUS", 16) == 0);
  CHECK(usb.soft_reset());
  p.shutdown();
  CHECK(!p.submit(ConstByteView(reinterpret_cast<const uint8_t*>("x"), 1), "after shutdown"));
}

static void test_usb_variants() {
  TestAllocator a;
  {
    FakeUsbHost host(false);   // unidirectional only: no way back
    host.device_id = "MFG:Generic;CMD:PCL;MDL:Plain 1;";
    FakeUsbBus bus(&host);
    List<UniquePointer<IPrinter>> found(a);
    UsbPrinter::discover(bus, a, found);
    CHECK(found.count() == 1);
    UsbPrinter& u = static_cast<UsbPrinter&>(*found[0]);
    uint8_t b[8];
    uint32_t n;
    CHECK(!u.bidirectional() && !u.read_back(ByteView(b), &n) && strcmp(u.name(), "Generic Plain 1") == 0);
    CHECK(u.info().supports(PrinterLanguage::Pcl) && !u.info().supports(PrinterLanguage::PostScript) && u.info().max_dpi == 600);
    bool no_set_interface = true;
    for (const auto& c : host.controls) if (c.request == 0x0B) no_set_interface = false;
    CHECK(no_set_interface);   // setting 0 needs no SET_INTERFACE
  }
  {
    FakeUsbHost host(true, false);   // bidirectional in setting 0
    host.device_id = "";
    FakeUsbBus bus(&host);
    SystemPrinters printers(a);
    CHECK(printers.discover_usb(bus) == 1 && printers.count() == 1 && strcmp(printers.default_printer()->name(), "USB printer") == 0);
    printers.shutdown();
  }
  // Damaged configuration descriptors never crash discovery.
  FakeUsbHost host;
  FakeUsbBus bus(&host);
  const std::vector<uint8_t> good = host.printer_config;
  uint32_t seed = 9;
  for (int round = 0; round < 400; ++round) {
    host.printer_config = good;
    host.printer_config.resize(round % (good.size() + 1));
    for (int k = 0; k < round % 5; ++k) { seed = seed * 1664525 + 1013904223; if (!host.printer_config.empty()) host.printer_config[(seed >> 8) % host.printer_config.size()] = static_cast<uint8_t>(seed >> 16); }
    List<UniquePointer<IPrinter>> found(a);
    UsbPrinter::discover(bus, a, found);
  }
  CHECK(g_live == 0);
}

int main() {
  test_packbits();
  test_pages_and_halftone();
  test_pcl_job();
  test_text();
  test_raw_delivery();
  test_lpd_delivery();
  test_failures_and_registry();
  test_device_id();
  test_usb_printer();
  test_usb_variants();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("printer tests passed\n");
  return 0;
}
