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
#include "Hardware/Printer/PrinterMonitor.hpp"
#include "Network/Snmp/SnmpUsm.hpp"
#include <algorithm>
#include <map>
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


// The printer end of an IEEE 1284.4 session, as a state machine over the byte streams of the two bulk pipes. It checks what the host
// does: data only with credit, packets no bigger than agreed, replies to its own commands.
struct FakeDot4Peripheral {
  std::string from_host, to_host;
  std::string printed;                 // payload of data packets on the print channel
  uint8_t socket = 0x01;
  bool inited = false, opened = false, exited = false;
  int credit_for_host = 2;             // credit the host holds now
  int grant_open = 2, grant_on_request = 2;
  int refuse_requests = 0;             // CreditRequests to answer with 0 credit (printer busy)
  uint16_t max_p2s = 1024, max_s2p = 512;
  int violations = 0, credit_requests = 0, credits_from_host = 0, replies_to_us = 0;
  bool send_credit_command = false;    // after the first data packet, grant credit unprompted
  std::string pending_reply;           // data waiting for host credit
  uint32_t host_credit_for_us = 0;
  size_t in_chunk = 0;                 // 0: serve whatever is queued; else at most this many bytes per transfer

  static void be16(std::string& s, uint16_t v) { s.push_back(static_cast<char>(v >> 8)); s.push_back(static_cast<char>(v)); }
  void packet(uint8_t ps, uint8_t ss, uint8_t credit, const std::string& payload) {
    to_host.push_back(static_cast<char>(ps)); to_host.push_back(static_cast<char>(ss));
    be16(to_host, static_cast<uint16_t>(6 + payload.size()));
    to_host.push_back(static_cast<char>(credit)); to_host.push_back(0);
    to_host += payload;
  }
  void reply(uint8_t cmd, const std::string& body) { packet(0, 0, 1, std::string(1, static_cast<char>(cmd | 0x80)) + body); }
  void flush_pending() {
    if (!pending_reply.empty() && host_credit_for_us > 0) { --host_credit_for_us; packet(socket, socket, 0, pending_reply); pending_reply.clear(); }
  }
  void feed(const uint8_t* d, size_t n) {
    from_host.append(reinterpret_cast<const char*>(d), n);
    while (from_host.size() >= 6) {
      const uint8_t* h = reinterpret_cast<const uint8_t*>(from_host.data());
      const size_t length = (h[2] << 8) | h[3];
      if (length < 6) { ++violations; from_host.clear(); return; }
      if (from_host.size() < length) return;
      const std::string payload = from_host.substr(6, length - 6);
      const uint8_t ps = h[0], ss = h[1];
      from_host.erase(0, length);
      if (ps == 0 && ss == 0) command(payload);
      else if (ps == socket && ss == socket && opened) {
        if (credit_for_host <= 0) ++violations; else --credit_for_host;
        if (length > max_p2s) ++violations;
        printed += payload;
        if (payload.find("@PJL INFO STATUS") != std::string::npos) pending_reply = "@PJL INFO STATUS\r\nCODE=10001\r\nONLINE=TRUE\r\n";
        flush_pending();
        if (send_credit_command) { send_credit_command = false; ++credit_for_host; packet(0, 0, 1, std::string("\x03", 1) + char(socket) + char(socket) + char(0) + char(1)); }
      } else {
        ++violations;
      }
    }
  }
  void command(const std::string& c) {
    if (c.empty()) { ++violations; return; }
    const uint8_t cmd = static_cast<uint8_t>(c[0]);
    if (cmd & 0x80) { if (cmd == 0x83) ++replies_to_us; return; }
    if (cmd == 0) { inited = true; reply(0, std::string(1, 0) + char(0x20)); }
    else if (cmd == 9) {
      if (!inited) ++violations;
      const std::string name = c.substr(1);
      reply(9, name == "PRINT" ? std::string(1, 0) + char(socket) + name : std::string(1, 0x0A) + char(0));
    } else if (cmd == 1 && c.size() >= 9) {
      opened = true;
      std::string body(1, 0);
      body += c[1]; body += c[2];
      be16(body, std::min<uint16_t>(max_p2s, static_cast<uint16_t>(((uint8_t)c[3] << 8) | (uint8_t)c[4])));
      be16(body, std::min<uint16_t>(max_s2p, static_cast<uint16_t>(((uint8_t)c[5] << 8) | (uint8_t)c[6])));
      be16(body, 0xFFFF);
      be16(body, static_cast<uint16_t>(grant_open));
      credit_for_host = grant_open;
      reply(1, body);
    } else if (cmd == 3 && c.size() >= 5) {
      ++credits_from_host;
      host_credit_for_us += static_cast<uint8_t>(c[3]) << 8 | static_cast<uint8_t>(c[4]);
      reply(3, std::string(1, 0) + c[1] + c[2]);
      flush_pending();
    } else if (cmd == 4 && c.size() >= 5) {
      ++credit_requests;
      int grant = 0;
      if (refuse_requests > 0) --refuse_requests; else grant = grant_on_request;
      credit_for_host += grant;
      std::string body(1, 0);
      body += c[1]; body += c[2];
      be16(body, static_cast<uint16_t>(grant));
      reply(4, body);
    } else if (cmd == 2) {
      opened = false;
      reply(2, std::string(1, 0) + c[1] + c[2]);
    } else if (cmd == 8) {
      exited = true;
      reply(8, std::string(1, 0));
    } else {
      ++violations;
    }
  }
  size_t serve(uint8_t* out, size_t capacity) {
    size_t n = std::min(capacity, to_host.size());
    if (in_chunk && n > in_chunk) n = in_chunk;
    memcpy(out, to_host.data(), n);
    to_host.erase(0, n);
    return n;
  }
};

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
  FakeDot4Peripheral* dot4 = nullptr;   // when set, the pipes carry IEEE 1284.4

  explicit FakeUsbHost(bool bidirectional = true, bool with_alternates = true, bool dot4_interface = false) {
    auto put = [](std::vector<uint8_t>& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b.begin(), b.end()); };
    std::vector<uint8_t>& c = printer_config;
    put(c, {9, 2, 0, 0, 1, 1, 0, 0x80, 50});
    if (dot4_interface) {
      put(c, {9, 4, 0, 0, 1, 7, 1, 1, 0});   // setting 0: unidirectional
      put(c, {7, 5, 0x01, 2, 0x00, 0x02, 0});
      put(c, {9, 4, 0, 1, 2, 7, 1, 3, 0});   // setting 1: IEEE 1284.4
      put(c, {7, 5, 0x01, 2, 0x00, 0x02, 0});
      put(c, {7, 5, 0x82, 2, 0x00, 0x02, 0});
    } else if (bidirectional && with_alternates) {
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
    if (dot4) {
      if (endpoint & 0x80) { *transferred = static_cast<uint32_t>(dot4->serve(data.data(), data.size())); return true; }
      if (fail_out > 0) { --fail_out; return false; }
      dot4->feed(data.data(), data.size());
      *transferred = static_cast<uint32_t>(data.size());
      return true;
    }
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
  host.port_status = 0x08 | 0x20;   // paper out, still selected? (bit 4 clear = off line), no error
  CHECK(p.report() == nullptr && p.request_report() && p.report() && p.report()->has_error(kErrorNoPaper) && p.report()->has_error(kErrorOffline) && !p.report()->has_error(kErrorServiceRequested));
  CHECK(strcmp(p.report()->description, "Brother HL-2070N series") == 0 && p.report()->supply_count == 0 && !p.report()->has_page_count);
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


// ---- SNMP ----

static SnmpValue vstr(const char* text) {
  SnmpValue v;
  v.type = SnmpType::OctetString;
  v.length = static_cast<uint8_t>(strlen(text));
  memcpy(v.bytes, text, v.length);
  return v;
}
static SnmpValue vint(int64_t n, SnmpType t = SnmpType::Integer) { SnmpValue v; v.type = t; v.integer = n; return v; }
static Oid O(const char* text) { Oid o; assert(Oid::parse(text, &o)); return o; }

static void test_snmp_messages() {
  TestAllocator a;
  // The classic GET of sysDescr.0 (version 1, community "public", request id 1), byte for byte.
  SnmpMessage m;
  m.version = 0;
  m.request_id = 1;
  m.count = 1;
  m.binds[0].oid = O("1.3.6.1.2.1.1.1.0");
  ByteBuffer out(a);
  CHECK(encode_snmp(m, a, &out));
  const uint8_t want[] = {0x30, 0x26, 0x02, 0x01, 0x00, 0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c', 0xA0, 0x19, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
                          0x30, 0x0E, 0x30, 0x0C, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00, 0x05, 0x00};
  CHECK(out.size() == sizeof want && memcmp(out.view().data(), want, sizeof want) == 0);
  SnmpMessage back;
  CHECK(decode_snmp(out.view(), &back) && back.version == 0 && strcmp(back.community, "public") == 0 && back.pdu == SnmpPdu::Get && back.request_id == 1 &&
        back.count == 1 && back.binds[0].oid == m.binds[0].oid && back.binds[0].value.type == SnmpType::Null);
  // Every value type, including big and negative numbers and arcs that need several bytes.
  SnmpMessage r;
  r.pdu = SnmpPdu::Response;
  r.request_id = 0x12345678;
  r.count = 9;
  const char* oids[9] = {"1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.10.0", "2.100.3", "1.3.6.1.2.1.43.10.2.1.4.1.1", "0.0", "1.3.6.1.2", "1.3.6.1.3", "1.3.6.1.4", "1.3.6.1.5", "1.3.6.1.6"};
  r.binds[0].value = vint(-129);
  r.binds[1].value = vstr("Brother HL-2070N");
  r.binds[2].value = vint(4000000000ll, SnmpType::Counter32);
  r.binds[3].value = vint(0x7fffffff, SnmpType::Gauge32);
  r.binds[4].value.type = SnmpType::ObjectId;
  r.binds[4].value.oid = O("1.3.6.1.4.1.2435");
  r.binds[5].value = vint(123456, SnmpType::TimeTicks);
  r.binds[6].value.type = SnmpType::NoSuchObject;
  r.binds[7].value.type = SnmpType::EndOfMibView;
  r.binds[8].value = vint(1ll << 40, SnmpType::Counter64);
  for (int i = 0; i < 9; ++i) r.binds[i].oid = O(oids[i]);
  CHECK(encode_snmp(r, a, &out) && decode_snmp(out.view(), &back) && back.count == 9 && back.request_id == 0x12345678);
  CHECK(back.binds[0].value.integer == -129 && back.binds[1].value.length == 16 && memcmp(back.binds[1].value.bytes, "Brother HL-2070N", 16) == 0);
  CHECK(back.binds[2].value.integer == 4000000000ll && back.binds[3].value.integer == 0x7fffffff && back.binds[4].value.oid == O("1.3.6.1.4.1.2435"));
  CHECK(back.binds[5].value.type == SnmpType::TimeTicks && back.binds[6].value.type == SnmpType::NoSuchObject && back.binds[7].value.type == SnmpType::EndOfMibView);
  CHECK(back.binds[8].value.integer == (1ll << 40) && back.binds[1].oid == O("2.100.3") && back.binds[0].oid == O(oids[0]));
  // Parsing OIDs.
  Oid o;
  CHECK(Oid::parse(".1.3.6.1", &o) && o.length == 4 && !Oid::parse("1", &o) && !Oid::parse("1.3.x", &o) && !Oid::parse("1.3.", &o) && !Oid::parse("", &o) && !Oid::parse("1.3.99999999999", &o));
  CHECK(O("1.3.6.1.2.1.1.1.0").starts_with(O("1.3.6.1.2")) && !O("1.3.6").starts_with(O("1.3.6.1")));
  // Damaged messages never crash, and most are refused.
  uint32_t seed = 3;
  int accepted = 0;
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> bytes(out.view().data(), out.view().data() + out.size());
    seed = seed * 1664525 + 1013904223;
    if (round % 3 == 0) bytes.resize((seed >> 8) % bytes.size());
    else for (int k = 0; k < 1 + round % 4; ++k) { seed = seed * 1664525 + 1013904223; bytes[(seed >> 8) % bytes.size()] = static_cast<uint8_t>(seed >> 20); }
    SnmpMessage x;
    if (decode_snmp(ConstByteView(bytes.data(), bytes.size()), &x)) ++accepted;
  }
  CHECK(accepted < 1500);
  // Refused outright.
  const uint8_t junk[] = {0x30, 0x03, 0x02, 0x01};
  CHECK(!decode_snmp(ConstByteView(junk, sizeof junk), &back) && !decode_snmp(ConstByteView(), &back));
  SnmpMessage too_many;
  too_many.count = 13;
  CHECK(!encode_snmp(too_many, a, &out));
}

// A scripted SNMP agent on stack B (UDP 161): a sorted MIB, GET and GETNEXT, with switches for misbehaviour.
struct FakeAgent {
  Lan& lan;
  SharedPointer<IUdpSocket> udp;
  std::map<std::vector<uint32_t>, SnmpValue> mib;
  int drop_first = 0;       // requests to ignore
  bool reject_all = false;  // answer everything with error-status noSuchName
  bool silent = false;
  int requests = 0;
  explicit FakeAgent(Lan& l) : lan(l) { udp = lan.b.udp_open(161); }
  static std::vector<uint32_t> key(const Oid& o) { return std::vector<uint32_t>(o.arc, o.arc + o.length); }
  void put(const char* oid, const SnmpValue& v) { mib[key(O(oid))] = v; }
  void fill(const SnmpMessage& req, SnmpMessage* out) {
    SnmpMessage& rsp = *out;
    if (reject_all) {
      rsp.error_status = 2;
      rsp.error_index = 1;
    } else {
      for (uint32_t i = 0; i < req.count; ++i) {
        if (req.pdu == SnmpPdu::Get) {
          auto it = mib.find(key(req.binds[i].oid));
          if (it != mib.end()) rsp.binds[i].value = it->second;
          else rsp.binds[i].value.type = SnmpType::NoSuchObject;
        } else {
          auto it = mib.upper_bound(key(req.binds[i].oid));
          if (it != mib.end()) {
            Oid o;
            o.length = static_cast<uint8_t>(it->first.size());
            for (size_t k = 0; k < it->first.size(); ++k) o.arc[k] = it->first[k];
            rsp.binds[i].oid = o;
            rsp.binds[i].value = it->second;
          } else {
            rsp.binds[i].value.type = SnmpType::EndOfMibView;
          }
        }
      }
    }
  }
  // --- SNMPv3 (user-based security), the agent side
  struct V3User { std::string name, auth_password, priv_password; UsmAuth auth; UsmPriv priv; UsmKeys keys; };
  std::vector<V3User> v3_users;
  uint8_t engine[12] = {0x80, 0, 0x0a, 0x13, 5, 'H', 'L', '2', '0', '7', '0', 'N'};
  uint32_t boots = 7;
  int64_t clock_offset_s = 0;     // the agent's clock runs this far from the stack's
  int not_in_window_once = 0;     // answer the next authenticated requests with notInTimeWindow
  int v3_requests = 0, discoveries = 0, resyncs = 0;
  uint32_t engine_time() const { return static_cast<uint32_t>(lan.clock.us / 1000000 + 5000 + clock_offset_s); }
  void add_user(const char* name, UsmAuth a, const char* ap, UsmPriv p, const char* pp) {
    V3User u{name, ap, pp, a, p, UsmKeys()};
    usm_make_keys(a, ap, p, pp, engine, sizeof engine, &u.keys);
    v3_users.push_back(u);
  }
  void send_v3(const SnmpMessage& m, const UsmKeys* keys, Ipv4Address to, uint16_t port) {
    ByteBuffer out(lan.alloc);
    static uint64_t salt = 1;
    if (encode_snmp_v3(m, keys, salt++, lan.alloc, &out)) udp->send_to(to, port, out.view());
  }
  void report(const SnmpMessage& req, const char* stat_oid, Ipv4Address to, uint16_t port) {
    SnmpMessage r;
    r.version = 3;
    r.pdu = SnmpPdu::Report;
    r.request_id = req.request_id;
    r.v3.msg_id = req.v3.msg_id;
    r.v3.engine_id_length = sizeof engine;
    memcpy(r.v3.engine_id, engine, sizeof engine);
    memcpy(r.v3.context_engine_id, engine, sizeof engine);
    r.v3.context_engine_id_length = sizeof engine;
    r.v3.boots = boots;
    r.v3.time = engine_time();
    r.count = 1;
    r.binds[0].oid = O(stat_oid);
    r.binds[0].value = vint(1, SnmpType::Counter32);
    send_v3(r, nullptr, to, port);
  }
  bool handle_v3(const uint8_t* buf, uint32_t n, Ipv4Address from, uint16_t port) {
    SnmpMessage req;
    ConstByteView data(buf, n);
    ++v3_requests;
    if (decode_snmp_v3(data, nullptr, &req) == UsmStatus::Ok && req.v3.engine_id_length == 0) {   // engine discovery
      ++discoveries;
      report(req, "1.3.6.1.6.3.15.1.1.4.0", from, port);
      return true;
    }
    for (V3User& u : v3_users) {
      if (decode_snmp_v3(data, u.auth == UsmAuth::None ? nullptr : &u.keys, &req) != UsmStatus::Ok || u.name != req.v3.user) continue;
      if (req.v3.engine_id_length != sizeof engine || memcmp(req.v3.engine_id, engine, sizeof engine)) { report(req, "1.3.6.1.6.3.15.1.1.4.0", from, port); return true; }
      const int64_t skew = static_cast<int64_t>(req.v3.time) - static_cast<int64_t>(engine_time());
      if (u.auth != UsmAuth::None && (req.v3.boots != boots || skew > 150 || skew < -150 || not_in_window_once > 0)) {
        if (not_in_window_once > 0) --not_in_window_once;
        ++resyncs;
        report(req, "1.3.6.1.6.3.15.1.1.2.0", from, port);
        return true;
      }
      SnmpMessage rsp = req;
      rsp.pdu = SnmpPdu::Response;
      fill(req, &rsp);
      rsp.v3.flags = 0;
      rsp.v3.boots = boots;
      rsp.v3.time = engine_time();
      send_v3(rsp, u.auth == UsmAuth::None ? nullptr : &u.keys, from, port);
      return true;
    }
    report(req, "1.3.6.1.6.3.15.1.1.5.0", from, port);   // wrongDigests / unknown user
    return true;
  }
  void step() {
    uint8_t buf[1500];
    Ipv4Address from;
    uint16_t port;
    uint32_t n;
    while (udp->receive_from(ByteView(buf), &from, &port, &n)) {
      SnmpMessage req, rsp;
      ++requests;
      if (silent) continue;
      {   // version 3?  30 <length> 02 01 03
        size_t at = 1 + 1 + (n > 1 && (buf[1] & 0x80) ? (buf[1] & 0x7F) : 0);
        if (n > at + 2 && buf[0] == 0x30 && buf[at] == 0x02 && buf[at + 1] == 0x01 && buf[at + 2] == 0x03 && handle_v3(buf, n, from, port)) continue;
      }
      if (!decode_snmp(ConstByteView(buf, n), &req)) continue;
      if (drop_first > 0) { --drop_first; continue; }
      rsp = req;
      rsp.pdu = SnmpPdu::Response;
      fill(req, &rsp);
      ByteBuffer out(lan.alloc);
      if (encode_snmp(rsp, lan.alloc, &out)) udp->send_to(from, port, out.view());
    }
  }
};

static void populate_hl2070n(FakeAgent& agent) {
  agent.put("1.3.6.1.2.1.1.1.0", vstr("Brother NC-6200h, Firmware Ver.1.12  (06.03.31),MID 8C5-129,FID 2"));
  agent.put("1.3.6.1.2.1.25.3.5.1.1.1", vint(3));
  const char err[2] = {0x20, 0x00};   // low toner (bit 2)
  SnmpValue e = vstr("");
  e.length = 2; e.bytes[0] = static_cast<uint8_t>(err[0]); e.bytes[1] = static_cast<uint8_t>(err[1]);
  agent.put("1.3.6.1.2.1.25.3.5.1.2.1", e);
  agent.put("1.3.6.1.2.1.43.10.2.1.4.1.1", vint(12345, SnmpType::Counter32));
  // (no serial number: noSuchObject)
  agent.put("1.3.6.1.2.1.43.11.1.1.4.1.1", vint(3));
  agent.put("1.3.6.1.2.1.43.11.1.1.5.1.1", vint(21));
  agent.put("1.3.6.1.2.1.43.11.1.1.6.1.1", vstr("Black Toner Cartridge"));
  agent.put("1.3.6.1.2.1.43.11.1.1.8.1.1", vint(100));
  agent.put("1.3.6.1.2.1.43.11.1.1.9.1.1", vint(7));
  agent.put("1.3.6.1.2.1.43.11.1.1.4.1.2", vint(3));
  agent.put("1.3.6.1.2.1.43.11.1.1.5.1.2", vint(9));
  agent.put("1.3.6.1.2.1.43.11.1.1.6.1.2", vstr("Drum Unit"));
  agent.put("1.3.6.1.2.1.43.11.1.1.8.1.2", vint(12000));
  agent.put("1.3.6.1.2.1.43.11.1.1.9.1.2", vint(9000));
  agent.put("1.3.6.1.2.1.43.11.1.1.4.1.3", vint(4));
  agent.put("1.3.6.1.2.1.43.11.1.1.5.1.3", vint(4));
  agent.put("1.3.6.1.2.1.43.11.1.1.6.1.3", vstr("Waste Toner Box"));
  agent.put("1.3.6.1.2.1.43.11.1.1.8.1.3", vint(-2));
  agent.put("1.3.6.1.2.1.43.11.1.1.9.1.3", vint(-3));
  agent.put("1.3.6.1.2.1.43.16.5.1.2.1.1", vstr("TONER LOW"));
  agent.put("1.3.6.1.2.1.43.18.1.1.2.1.1", vint(4));
  agent.put("1.3.6.1.2.1.43.18.1.1.7.1.1", vint(12));
  agent.put("1.3.6.1.2.1.43.18.1.1.8.1.1", vstr("Toner low"));
  agent.put("1.3.6.1.2.1.43.18.1.1.2.1.2", vint(3));
  agent.put("1.3.6.1.2.1.43.18.1.1.7.1.2", vint(501));
  agent.put("1.3.6.1.2.1.43.18.1.1.8.1.2", vstr("Cover is open"));
  agent.put("1.3.6.1.2.1.44.1.1.0", vint(1));   // (beyond everything the monitor walks)
}

static bool pump_monitor(Lan& lan, FakeAgent& agent, PrinterMonitor& mon, int max_ms = 30000) {
  for (int i = 0; i < max_ms; ++i) {
    if (!mon.busy()) return true;
    lan.step();
    agent.step();
    mon.poll();
  }
  return !mon.busy();
}

static void test_printer_monitor() {
  {
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    PrinterMonitor mon(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(!mon.succeeded() && mon.start() && mon.busy() && !mon.start());
    CHECK(pump_monitor(lan, agent, mon) && mon.succeeded());
    const PrinterReport& r = mon.report();
    CHECK(r.valid && r.engine == PrinterEngineState::Idle && r.has_page_count && r.page_count == 12345);
    CHECK(strncmp(r.description, "Brother NC-6200h", 16) == 0 && r.serial[0] == 0 && strcmp(r.display, "TONER LOW") == 0);
    CHECK(r.has_error(kErrorLowToner) && !r.has_error(kErrorNoToner) && !r.has_error(kErrorJammed) && r.toner_low());
    CHECK(r.supply_count == 3);
    if (r.supply_count == 3) {
      CHECK(r.supplies[0].kind == SupplyKind::Toner && strcmp(r.supplies[0].description, "Black Toner Cartridge") == 0 && r.supplies[0].level_percent == 7 && r.supplies[0].low() && !r.supplies[0].empty());
      CHECK(r.supplies[1].kind == SupplyKind::Drum && r.supplies[1].level_percent == 75 && !r.supplies[1].low());
      CHECK(r.supplies[2].kind == SupplyKind::WasteToner && !r.supplies[2].consumed && r.supplies[2].level_percent == SupplyInfo::kLevelSomeRemaining && !r.supplies[2].low());
      CHECK(r.supply(SupplyKind::Toner) == &r.supplies[0] && r.supply(SupplyKind::Fuser) == nullptr);
    }
    CHECK(r.alert_count == 2);
    if (r.alert_count == 2) {
      CHECK(r.alerts[0].severity == AlertSeverity::Warning && r.alerts[0].code == 12 && strcmp(r.alerts[0].description, "Toner low") == 0);
      CHECK(r.alerts[1].severity == AlertSeverity::Critical && r.alerts[1].code == 501 && strcmp(alert_code_name(r.alerts[1].code), "door open") == 0);
    }
    // A second refresh sees the changes: the toner is gone and the printer is printing.
    agent.put("1.3.6.1.2.1.43.11.1.1.9.1.1", vint(0));
    agent.put("1.3.6.1.2.1.25.3.5.1.1.1", vint(4));
    agent.put("1.3.6.1.2.1.25.3.5.1.2.1", [] { SnmpValue e = vstr(""); e.length = 1; e.bytes[0] = 0x10 | 0x04; return e; }());
    CHECK(mon.start() && pump_monitor(lan, agent, mon) && mon.succeeded());
    CHECK(mon.report().engine == PrinterEngineState::Printing && mon.report().supplies[0].empty() && mon.report().has_error(kErrorNoToner) && mon.report().has_error(kErrorJammed) && !mon.report().has_error(kErrorLowToner));
  }
  {   // a lost request is sent again
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    agent.drop_first = 2;
    PrinterMonitor mon(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(mon.start() && pump_monitor(lan, agent, mon) && mon.succeeded() && mon.report().page_count == 12345 && agent.requests > 3);
  }
  {   // nobody answers: it fails after the retries, and the failure is clean
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    agent.silent = true;
    PrinterMonitor mon(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(mon.start() && pump_monitor(lan, agent, mon) && mon.failed() && !mon.report().valid && agent.requests == 3);
    CHECK(mon.start());   // and can be tried again
  }
  {   // an agent that refuses everything (noSuchName): the refresh completes with an empty report
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    agent.reject_all = true;
    PrinterMonitor mon(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(mon.start() && pump_monitor(lan, agent, mon) && mon.succeeded() && mon.report().supply_count == 0 && mon.report().alert_count == 0 && !mon.report().has_page_count);
  }
  {   // through the printer object
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    BrotherHl2070n printer(lan.a, Ipv4Address::make(10, 0, 0, 2), lan.alloc);
    CHECK(printer.report() == nullptr && !printer.report_pending() && printer.request_report() && printer.report_pending() && !printer.request_report());
    for (int i = 0; i < 30000 && printer.report_pending(); ++i) { lan.step(); agent.step(); printer.poll(); }
    CHECK(printer.report() != nullptr && printer.report()->page_count == 12345 && printer.report()->toner_low());
  }
}


static void test_dot4() {
  TestAllocator a;
  FakeUsbBus* none = nullptr;
  (void)none;
  {
    FakeUsbHost host(true, true, true);
    FakeDot4Peripheral peri;
    host.dot4 = &peri;
    FakeUsbBus bus(&host);
    List<UniquePointer<IPrinter>> found(a);
    UsbPrinter::discover(bus, a, found);
    CHECK(found.count() == 1);
    if (found.count() != 1) return;
    UsbPrinter& usb = static_cast<UsbPrinter&>(*found[0]);
    CHECK(usb.multiplexed() && usb.bidirectional() && peri.inited && peri.opened && usb.dot4() && usb.dot4()->socket() == 1);
    CHECK(usb.dot4()->max_packet() == 1024 && usb.dot4()->printer_max_packet() == 512);
    bool set_alt = false;
    for (const auto& c : host.controls) if (c.request == 0x0B && c.value == 1) set_alt = true;
    CHECK(set_alt && strcmp(usb.name(), "Brother HL-2070N series") == 0);   // the device ID still comes by class request

    // A 40 KB job: 1018 bytes a packet, two credits at a time.
    std::string job(40000, 'x');
    for (size_t i = 0; i < job.size(); ++i) job[i] = static_cast<char>(i * 13 + 1);
    peri.send_credit_command = true;   // the printer also grants credit by itself once
    CHECK(usb.submit(ConstByteView(reinterpret_cast<const uint8_t*>(job.data()), job.size()), "dot4 job"));
    for (int i = 0; i < 1000 && usb.pending_jobs(); ++i) usb.poll();
    CHECK(usb.pending_jobs() == 0 && peri.printed == job && peri.violations == 0 && usb.statistics().jobs_sent == 1);
    CHECK(peri.credit_requests > 5 && usb.dot4()->packets_sent() == (job.size() + 1017) / 1018);   // (full packets: the job goes out in whole multiples of the packet size)
    CHECK(peri.replies_to_us == 1);   // the unsolicited Credit command was answered

    // A printer with no credit to give right now: nothing is lost, nothing is counted as a failure.
    peri.printed.clear();
    peri.refuse_requests = 3;
    const std::string wait_job(6000, 'w');   // more than the credit it holds
    CHECK(usb.submit(ConstByteView(reinterpret_cast<const uint8_t*>(wait_job.data()), wait_job.size()), "wait"));
    int polls = 0;
    while (usb.pending_jobs() && polls < 50) { usb.poll(); ++polls; }
    CHECK(polls >= 4 && peri.printed == wait_job && peri.violations == 0 && usb.statistics().jobs_failed == 0 && usb.state() == PrinterState::Ready);

    // The way back: a PJL query, answered on the channel. The printer needs credit for it (the host grants some).
    peri.in_chunk = 7;   // the reply trickles in a few bytes per transfer
    const char query[] = "\x1b%-12345X@PJL INFO STATUS\r\n\x1b%-12345X";
    CHECK(usb.submit(ConstByteView(reinterpret_cast<const uint8_t*>(query), sizeof query - 1), "query"));
    for (int i = 0; i < 20 && usb.pending_jobs(); ++i) usb.poll();
    uint8_t back[256];
    uint32_t n = 0;
    CHECK(usb.read_back(ByteView(back), &n) && n > 20 && memcmp(back, "@PJL INFO STATUS", 16) == 0 && peri.credits_from_host >= 1 && peri.violations == 0);

    // A failing pipe: the session is closed and opened again, and the job completes.
    peri.printed.clear();
    host.fail_out = 1;
    const char small[] = "after a failure";
    CHECK(usb.submit(ConstByteView(reinterpret_cast<const uint8_t*>(small), sizeof small - 1), "retry"));
    for (int i = 0; i < 20 && usb.pending_jobs(); ++i) usb.poll();
    CHECK(usb.statistics().jobs_failed == 0 && peri.printed == "after a failure" && usb.multiplexed());
    usb.shutdown();
    CHECK(peri.exited && !peri.opened);
  }
  {   // a printer that does not know the service, or that never answers: the printer is left out
    FakeUsbHost host(true, true, true);
    FakeDot4Peripheral peri;
    peri.socket = 0;   // GetSocketID answers with an error
    host.dot4 = &peri;
    FakeUsbBus bus(&host);
    List<UniquePointer<IPrinter>> found(a);
    UsbPrinter::discover(bus, a, found);
    CHECK(found.count() == 0);
  }
  {   // a printer that sends garbage instead of packets
    FakeUsbHost host(true, true, true);
    FakeDot4Peripheral peri;
    peri.to_host = std::string("\xff\xff\xff\xff\xff\xff\xff\xff", 8);
    host.dot4 = &peri;
    FakeUsbBus bus(&host);
    List<UniquePointer<IPrinter>> found(a);
    UsbPrinter::discover(bus, a, found);
    CHECK(found.count() == 0);
  }
  {   // plain bidirectional is preferred over 1284.4 when the printer offers both
    FakeUsbHost host;
    FakeUsbBus bus(&host);
    List<UniquePointer<IPrinter>> found(a);
    UsbPrinter::discover(bus, a, found);
    CHECK(found.count() == 1 && !static_cast<UsbPrinter&>(*found[0]).multiplexed());
  }
  // The session directly: random traffic from the printer never crashes the host side.
  {
    uint32_t seed = 11;
    for (int round = 0; round < 300; ++round) {
      FakeUsbHost host(true, true, true);
      FakeDot4Peripheral peri;
      host.dot4 = &peri;
      for (int k = 0, n = 1 + round % 40; k < n; ++k) { seed = seed * 1664525 + 1013904223; peri.to_host.push_back(static_cast<char>(seed >> 16)); }
      if (round % 2) { peri.to_host.insert(0, std::string("\x00\x00\x00\x09\x01\x00\x80\x00\x20", 9)); }
      FakeUsbBus bus(&host);
      List<UniquePointer<IPrinter>> found(a);
      UsbPrinter::discover(bus, a, found);
    }
  }
  CHECK(g_live == 0);
}


static void test_snmp_v3_monitor() {
  const UsmAuth auths[] = {UsmAuth::None, UsmAuth::HmacMd5, UsmAuth::HmacSha1, UsmAuth::HmacSha256, UsmAuth::HmacSha1, UsmAuth::HmacMd5};
  const UsmPriv privs[] = {UsmPriv::None, UsmPriv::None, UsmPriv::Aes128, UsmPriv::Aes128, UsmPriv::Des, UsmPriv::Des};
  for (int k = 0; k < 6; ++k) {
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    agent.add_user("monitor", auths[k], "authpassword", privs[k], "privpassword");
    PrinterMonitor mon(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(mon.client().set_v3_user("monitor", auths[k], "authpassword", privs[k], "privpassword"));
    CHECK(mon.start() && pump_monitor(lan, agent, mon, 60000) && mon.succeeded());
    CHECK(mon.report().page_count == 12345 && mon.report().supply_count == 3 && mon.report().alert_count == 2 && mon.report().toner_low());
    CHECK(agent.discoveries == 1 && mon.client().engine_known() && mon.client().engine_boots() == 7);
    // A second refresh reuses the engine: no new discovery.
    CHECK(mon.start() && pump_monitor(lan, agent, mon, 60000) && mon.succeeded() && agent.discoveries == 1);
  }
  {   // the agent's clock is far off (or it rebooted): its notInTimeWindow report makes the client adopt the clock and send again
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    agent.add_user("monitor", UsmAuth::HmacSha1, "authpassword", UsmPriv::Aes128, "privpassword");
    PrinterMonitor mon(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(mon.client().set_v3_user("monitor", UsmAuth::HmacSha1, "authpassword", UsmPriv::Aes128, "privpassword"));
    CHECK(mon.start() && pump_monitor(lan, agent, mon, 60000) && mon.succeeded());
    agent.not_in_window_once = 1;
    CHECK(mon.start() && pump_monitor(lan, agent, mon, 60000) && mon.succeeded() && agent.resyncs == 1);
    agent.boots = 8;   // the printer was switched off and on: new boot count
    CHECK(mon.start() && pump_monitor(lan, agent, mon, 60000) && mon.succeeded() && mon.client().engine_boots() == 8);
  }
  {   // wrong password, unknown user, a user that must authenticate when the agent knows only a plain one: a clean failure, not a hang
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    agent.add_user("monitor", UsmAuth::HmacSha1, "authpassword", UsmPriv::Aes128, "privpassword");
    PrinterMonitor bad(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(bad.client().set_v3_user("monitor", UsmAuth::HmacSha1, "wrong-password", UsmPriv::Aes128, "privpassword"));
    CHECK(bad.start() && pump_monitor(lan, agent, bad) && bad.failed() && bad.client().error() == SnmpError::Security);
    PrinterMonitor nobody(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 2));
    CHECK(nobody.client().set_v3_user("nobody", UsmAuth::HmacSha1, "authpassword", UsmPriv::None, "") && nobody.start() && pump_monitor(lan, agent, nobody) && nobody.failed());
    CHECK(!nobody.client().set_v3_user("x", UsmAuth::None, "", UsmPriv::Aes128, "privpassword") && !nobody.client().set_v3_user("x", UsmAuth::HmacSha1, "short", UsmPriv::None, ""));
  }
  {   // through the printer object
    Lan lan;
    lan.configure_static();
    FakeAgent agent(lan);
    populate_hl2070n(agent);
    agent.add_user("monitor", UsmAuth::HmacSha1, "authpassword", UsmPriv::Aes128, "privpassword");
    BrotherHl2070n printer(lan.a, Ipv4Address::make(10, 0, 0, 2), lan.alloc);
    CHECK(printer.set_snmp_v3_user("monitor", UsmAuth::HmacSha1, "authpassword", UsmPriv::Aes128, "privpassword") && printer.request_report());
    for (int i = 0; i < 60000 && printer.report_pending(); ++i) { lan.step(); agent.step(); printer.poll(); }
    CHECK(printer.report() && printer.report()->page_count == 12345 && agent.v3_requests >= 2);
  }
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
  test_dot4();
  test_snmp_messages();
  test_printer_monitor();
  test_snmp_v3_monitor();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("printer tests passed\n");
  return 0;
}
