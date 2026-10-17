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

int main() {
  test_packbits();
  test_pages_and_halftone();
  test_pcl_job();
  test_text();
  test_raw_delivery();
  test_lpd_delivery();
  test_failures_and_registry();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("printer tests passed\n");
  return 0;
}
