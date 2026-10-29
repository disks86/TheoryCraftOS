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


// DpLinkTrainer against a simulated sink and source that log everything in order: the sequence of writes the DisplayPort specification asks for (power, downspread, lane count and rate,
// the patterns, the drive per lane, patterns off), the clock recovery and channel equalization loops with a sink that asks for levels (reaching them, never reaching them, asking
// for the maximum, equalizing only after a few rounds), the delays by the sink's read interval, every write and read failing in turn, and the link configuration choice.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
// The sink's DPCD as a state machine, and the source: both append to one log so that the order of events across them is visible.
struct Rig final : IDpcdAux, IDpSource {
  std::vector<uint8_t> dpcd = std::vector<uint8_t>(0x800, 0);
  std::vector<std::string> log;
  uint32_t lanes = 4;
  // what each lane needs: voltage swing level for the clock recovery, voltage and pre-emphasis levels for the equalization
  uint8_t need_voltage[4] = {0, 0, 0, 0}, need_pre[4] = {0, 0, 0, 0};
  uint32_t eq_after_reads = 0;       // the channel equalization succeeds only from this many reads of the status in pattern 2/3 (0: as soon as the drive is right)
  bool never_recover = false;        // the clock is never recovered
  bool never_equalize = false;
  int ask_voltage = -1;              // the sink always asks for this voltage level whatever it needs (-1: what it needs)
  std::vector<int> ask_sequence;     // or, if given, the level it asks for at the nth status read (the last one repeats)
  // failures
  uint32_t fail_write_address = 0xFFFFFFFF, fail_write_nth = 0;      // the nth write (1-based) to this address fails
  uint32_t write_count_at_address = 0;
  bool fail_status_read = false;
  uint32_t fail_status_after = 0, status_reads = 0;
  bool fail_start = false, fail_pattern = false, fail_drive = false, fail_complete = false;
  uint32_t eq_reads = 0;
  std::vector<uint32_t> delays;

  static std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "%02X", v);
    return b;
  }

  void model_status() {
    const uint8_t pattern = dpcd[0x102] & 3;
    bool all_eq = true;
    for (uint32_t l = 0; l < 4; ++l) {
      uint8_t nibble = 0;
      if (l < lanes) {
        const uint8_t set = dpcd[0x103 + l];
        const bool cr = !never_recover && pattern != 0 && (set & 3) >= need_voltage[l];
        const bool eq = cr && !never_equalize && pattern >= 2 && (set & 3) >= need_voltage[l] && ((set >> 3) & 3) >= need_pre[l] && eq_reads >= eq_after_reads;
        nibble = static_cast<uint8_t>((cr ? 1 : 0) | (eq ? 2 : 0) | (eq ? 4 : 0));
        if (!eq) all_eq = false;
      }
      const uint32_t at = 0x202 + l / 2, shift = l % 2 * 4;
      dpcd[at] = static_cast<uint8_t>((dpcd[at] & ~(0xF << shift)) | (nibble << shift));
      // the adjust request: voltage in bits 1:0, pre-emphasis in 3:2 of the lane's nibble
      uint8_t v = ask_voltage >= 0 ? static_cast<uint8_t>(ask_voltage) : need_voltage[l];
      if (!ask_sequence.empty()) v = static_cast<uint8_t>(ask_sequence[status_reads - 1 < ask_sequence.size() ? status_reads - 1 : ask_sequence.size() - 1]);
      const uint8_t request = static_cast<uint8_t>((l < lanes ? v : 0) | ((l < lanes ? need_pre[l] : 0) << 2));
      const uint32_t ra = 0x206 + l / 2;
      dpcd[ra] = static_cast<uint8_t>((dpcd[ra] & ~(0xF << shift)) | (request << shift));
    }
    dpcd[0x204] = all_eq ? 0x01 : 0x00;
  }

  bool dpcd_read(uint32_t address, ByteView out) override {
    if (address == 0x200) {
      ++status_reads;
      if (fail_status_read || (fail_status_after && status_reads > fail_status_after)) return false;
      if ((dpcd[0x102] & 3) >= 2) ++eq_reads;
      model_status();
      log.push_back("status");
    }
    for (size_t i = 0; i < out.size(); ++i) out[i] = dpcd[address + i];
    return true;
  }
  bool dpcd_write(uint32_t address, ConstByteView data) override {
    if (address == fail_write_address && ++write_count_at_address == fail_write_nth) return false;
    std::string entry = "w " + hex(address >> 8) + hex(address & 0xFF) + "=";
    for (size_t i = 0; i < data.size(); ++i) {
      dpcd[address + i] = data[i];
      entry += hex(data[i]);
    }
    log.push_back(entry);
    return true;
  }
  bool start_training() override { log.push_back("start"); return !fail_start; }
  bool set_pattern(uint8_t pattern) override { log.push_back("pattern " + hex(pattern)); return !fail_pattern; }
  bool complete_training() override { log.push_back("complete"); return !fail_complete; }
  bool set_drive(uint8_t lane_set) override { log.push_back("drive " + hex(lane_set)); return !fail_drive; }
  void delay_us(uint32_t us) override { delays.push_back(us); }
};

Dpcd::Capabilities sink_caps() {
  Dpcd::Capabilities c;
  c.valid = true;
  c.revision = 0x12;
  c.max_link_rate_code = 0x14;
  c.max_lanes = 4;
  c.tps3 = true;
  c.enhanced_framing = true;
  c.max_downspread = true;
  return c;
}

size_t index_of(const std::vector<std::string>& log, const std::string& entry, size_t from = 0) {
  for (size_t i = from; i < log.size(); ++i)
    if (log[i] == entry) return i;
  return log.size();
}
}  // namespace

static void test_clean_training() {
  Rig r;
  r.lanes = 4;
  const Dpcd::Capabilities caps = sink_caps();
  const DpLinkTrainer::Outcome o = DpLinkTrainer::train(r, r, caps, 0x0A, 4);
  CHECK(o.trained() && o.lanes == 4 && o.bandwidth_code == 0x0A && o.clock_recovery_reads == 1 && o.channel_eq_reads == 1);
  const std::vector<std::string> want = {
      "w 0600=01",           // sink power D0
      "w 0107=10",           // downspread 0.5 % (the sink can)
      "w 0101=84",           // four lanes with enhanced framing
      "w 0100=0A",           // 2.7 Gbit/s
      "start",
      "w 0102=00",           // the pattern off on the sink
      "pattern 01",          // clock recovery: pattern 1 on the source, the sink, then the drive (source, then sink's four lanes)
      "w 0102=01",
      "drive 00",
      "w 0103=00000000",
      "status",              // (clock recovered at once)
      "pattern 03",          // equalization with pattern 3 (the sink has it)
      "w 0102=03",
      "status",
      "w 0102=00",           // done: pattern off, the encoder told
      "complete",
  };
  CHECK(r.log == want);
  if (r.log != want) {
    for (const std::string& s : r.log) printf("  %s\\n", s.c_str());
  }
  // the delays: 1 ms for the sink to wake, 400 us before clock recovery, then the default intervals (100 us, 400 us)
  CHECK(r.delays == std::vector<uint32_t>({1000, 400, 100, 400}));
  CHECK(o.train_set[0] == 0 && o.train_set[3] == 0);
}

static void test_options() {
  // a sink of DPCD 1.0 has no power state; without downspread or enhanced framing or pattern 3
  Rig r;
  Dpcd::Capabilities caps = sink_caps();
  caps.revision = 0x10;
  caps.max_downspread = false;
  caps.enhanced_framing = false;
  caps.tps3 = false;
  r.lanes = 2;
  const DpLinkTrainer::Outcome o = DpLinkTrainer::train(r, r, caps, 0x06, 2);
  CHECK(o.trained());
  CHECK(index_of(r.log, "w 0600=01") == r.log.size() && index_of(r.log, "w 0107=00") < r.log.size() && index_of(r.log, "w 0101=02") < r.log.size() && index_of(r.log, "w 0100=06") < r.log.size());
  CHECK(index_of(r.log, "pattern 02") < r.log.size() && index_of(r.log, "pattern 03") == r.log.size() && index_of(r.log, "w 0103=0000") < r.log.size());
  CHECK(r.delays == std::vector<uint32_t>({400, 100, 400}));       // (no 1 ms after a power state write)
  // the read interval: 3 means 12 ms between the steps, both loops
  Rig s;
  caps = sink_caps();
  caps.training_read_interval = 3;
  CHECK(DpLinkTrainer::train(s, s, caps, 0x0A, 4).trained());
  CHECK(s.delays == std::vector<uint32_t>({1000, 400, 12000, 12000}));
  // one lane
  // exactly DPCD 1.1 has the power state
  Rig v11;
  caps = sink_caps();
  caps.revision = 0x11;
  CHECK(DpLinkTrainer::train(v11, v11, caps, 0x0A, 4).trained() && v11.log[0] == "w 0600=01" && v11.delays[0] == 1000);
  Rig one;
  one.lanes = 1;
  CHECK(DpLinkTrainer::train(one, one, sink_caps(), 0x0A, 1).trained() && index_of(one.log, "w 0101=81") < one.log.size() && index_of(one.log, "w 0103=00") < one.log.size());
  // lane counts that are none: refused before anything happens
  Rig bad;
  for (uint32_t n : {0u, 3u, 5u, 8u}) CHECK(DpLinkTrainer::train(bad, bad, sink_caps(), 0x0A, n).result == DpLinkTrainer::Result::SinkSetup && bad.log.empty());
}

static void test_adjusting() {
  // the sink needs voltage 1 and pre-emphasis 1: asked for after the first status read; the drive 0x09 (voltage 1 | pre-emphasis 1 << 3) then recovers the clock and equalizes
  Rig r;
  r.lanes = 4;
  for (int l = 0; l < 4; ++l) r.need_voltage[l] = 1, r.need_pre[l] = 1;
  DpLinkTrainer::Outcome o = DpLinkTrainer::train(r, r, sink_caps(), 0x0A, 4);
  CHECK(o.trained() && o.clock_recovery_reads == 2 && o.channel_eq_reads == 1);
  CHECK(index_of(r.log, "drive 09") < r.log.size() && index_of(r.log, "w 0103=09090909") < r.log.size() && o.train_set[0] == 0x09 && o.train_set[3] == 0x09);
  // the drive is the highest any lane asks for, set on every lane; the flags appear once the top level is asked
  Rig m;
  m.lanes = 4;
  m.need_voltage[0] = 0; m.need_voltage[1] = 2; m.need_voltage[2] = 1; m.need_voltage[3] = 0;
  m.need_pre[0] = 0; m.need_pre[1] = 1; m.need_pre[2] = 2; m.need_pre[3] = 0;
  o = DpLinkTrainer::train(m, m, sink_caps(), 0x0A, 4);
  CHECK(o.trained() && o.train_set[0] == (2 | (2 << 3)) && o.train_set[1] == o.train_set[0] && index_of(m.log, "w 0103=12121212") < m.log.size());
  Rig top;
  top.lanes = 4;
  top.need_voltage[2] = 3;
  top.need_pre[1] = 3;
  o = DpLinkTrainer::train(top, top, sink_caps(), 0x0A, 4);
  // (voltage 3 | max swing 4, pre-emphasis 3 << 3 | max pre-emphasis 0x20 = 0x7 | 0x38 = 0x3F)
  CHECK(o.trained() && o.train_set[0] == 0x3F && index_of(top.log, "drive 3F") < top.log.size());
  // fewer lanes than four only look at their own
  Rig two;
  two.lanes = 2;
  two.need_voltage[0] = 1;
  two.need_voltage[2] = 3;        // (a lane that is not in use)
  o = DpLinkTrainer::train(two, two, sink_caps(), 0x0A, 2);
  CHECK(o.trained() && o.train_set[0] == 0x01 && index_of(two.log, "w 0103=0101") < two.log.size());
}

static void test_clock_recovery_failures() {
  // the clock is never recovered and the sink keeps asking for voltage 2: the first read sets 2, the next five repeat it (the second read sees the new voltage for the first time)
  Rig r;
  r.never_recover = true;
  r.ask_voltage = 2;
  DpLinkTrainer::Outcome o = DpLinkTrainer::train(r, r, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::ClockRecoveryTries && o.clock_recovery_reads == 7);
  // still cleaned up: the pattern off on the sink and the source told it is over; and no equalization
  CHECK(r.log.back() == "complete" && r.log[r.log.size() - 2] == "w 0102=00" && index_of(r.log, "pattern 03") == r.log.size());
  // the tries are of the same voltage in a row: a sink that asks 1, 1, 1, 1 and then 2 five more times is not given up on at the fifth repeat overall, but when the voltage 2 has been asked six
  // times (the first is a change): reads 1 to 4 set 1, the fifth sets 2, and the fifth repeat of 2 is the eleventh read
  Rig seq;
  seq.never_recover = true;
  seq.ask_sequence = {1, 1, 1, 1, 2};
  o = DpLinkTrainer::train(seq, seq, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::ClockRecoveryTries && o.clock_recovery_reads == 11);
  // asking for the most swing: every lane at the maximum and still no clock: gives up at the second read
  Rig m;
  m.never_recover = true;
  m.ask_voltage = 3;
  o = DpLinkTrainer::train(m, m, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::ClockRecoveryMaxSwing && o.clock_recovery_reads == 2 && o.train_set[0] == 0x07);
  // asking for a level that comes up as different each time is not "the same voltage": the tries start over (a sink asking 1 then 2 then 3 within the budget)
  Rig step;
  step.lanes = 4;
  step.need_voltage[0] = step.need_voltage[1] = step.need_voltage[2] = step.need_voltage[3] = 3;
  o = DpLinkTrainer::train(step, step, sink_caps(), 0x0A, 4);
  CHECK(o.trained() && o.clock_recovery_reads == 2 && o.train_set[0] == 0x07);
}

static void test_equalization() {
  // equalizes on the Nth read in pattern 3: the loop adjusts and reads again, up to six adjustments (seven reads)
  for (uint32_t after : {1u, 2u, 5u, 7u}) {
    Rig r;
    r.eq_after_reads = after;
    const DpLinkTrainer::Outcome o = DpLinkTrainer::train(r, r, sink_caps(), 0x0A, 4);
    CHECK(o.trained() && o.channel_eq_reads == after);
  }
  Rig late;
  late.eq_after_reads = 8;
  DpLinkTrainer::Outcome o = DpLinkTrainer::train(late, late, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::ChannelEqualization && o.channel_eq_reads == 7);
  CHECK(late.log.back() == "complete" && late.log[late.log.size() - 2] == "w 0102=00");
  Rig never;
  never.never_equalize = true;
  o = DpLinkTrainer::train(never, never, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::ChannelEqualization && o.channel_eq_reads == 7);
  // the equalization needs the alignment too: lanes in sync but not aligned is not done (the model sets the alignment with the equalization, so break it)
  struct NoAlign final : IDpcdAux {
    Rig& rig;
    explicit NoAlign(Rig& r) : rig(r) {}
    bool dpcd_read(uint32_t a, ByteView out) override {
      const bool ok = rig.dpcd_read(a, out);
      if (a == 0x200 && out.size() > 4) out[4] = 0;      // (the alignment status byte)
      return ok;
    }
    bool dpcd_write(uint32_t a, ConstByteView d) override { return rig.dpcd_write(a, d); }
  };
  Rig base;
  NoAlign na(base);
  o = DpLinkTrainer::train(na, base, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::ChannelEqualization);
}

static void test_patterns() {
  // pattern 3 for a sink that has it, pattern 2 otherwise
  Rig a;
  Dpcd::Capabilities c = sink_caps();
  CHECK(DpLinkTrainer::train(a, a, c, 0x0A, 4).trained() && index_of(a.log, "pattern 03") < a.log.size() && index_of(a.log, "pattern 02") == a.log.size());
  Rig b;
  c.tps3 = false;
  CHECK(DpLinkTrainer::train(b, b, c, 0x0A, 4).trained() && index_of(b.log, "pattern 02") < b.log.size() && index_of(b.log, "pattern 03") == b.log.size());
  // pattern 1 first, with the source ahead of the sink; the pattern is on before the first drive of the clock recovery
  CHECK(index_of(b.log, "pattern 01") + 1 == index_of(b.log, "w 0102=01") && index_of(b.log, "w 0102=01") < index_of(b.log, "drive 00"));
}

static void test_failures() {
  const DpLinkTrainer::Result R = DpLinkTrainer::Result::Trained;
  (void)R;
  struct Fail {
    uint32_t address, nth;
    DpLinkTrainer::Result result;
  };
  const Fail writes[] = {{0x600, 1, DpLinkTrainer::Result::SinkPower}, {0x107, 1, DpLinkTrainer::Result::SinkSetup}, {0x101, 1, DpLinkTrainer::Result::SinkSetup},
                         {0x100, 1, DpLinkTrainer::Result::SinkSetup}, {0x102, 1, DpLinkTrainer::Result::PatternWrite},       // the first pattern write: off before the training
                         {0x102, 2, DpLinkTrainer::Result::PatternWrite},        // pattern 1 on the sink
                         {0x103, 1, DpLinkTrainer::Result::PatternWrite},        // the first drive write
                         {0x102, 3, DpLinkTrainer::Result::PatternWrite},        // pattern 3 on the sink
                         {0x102, 4, DpLinkTrainer::Result::PatternWrite}};       // the pattern off at the end
  for (const Fail& f : writes) {
    Rig r;
    r.fail_write_address = f.address;
    r.fail_write_nth = f.nth;
    const DpLinkTrainer::Outcome o = DpLinkTrainer::train(r, r, sink_caps(), 0x0A, 4);
    CHECK(o.result == f.result);
  }
  // a drive write failing in the middle of the adjusting
  Rig d;
  d.need_voltage[0] = 1;
  d.fail_write_address = 0x103;
  d.fail_write_nth = 2;
  CHECK(DpLinkTrainer::train(d, d, sink_caps(), 0x0A, 4).result == DpLinkTrainer::Result::DriveWrite);
  Rig s;
  s.fail_status_read = true;
  CHECK(DpLinkTrainer::train(s, s, sink_caps(), 0x0A, 4).result == DpLinkTrainer::Result::StatusRead);
  Rig s2;
  s2.fail_status_after = 1;      // the clock recovery's status comes, the equalization's does not
  DpLinkTrainer::Outcome o = DpLinkTrainer::train(s2, s2, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::StatusRead && o.clock_recovery_reads == 1 && o.channel_eq_reads == 1 && s2.log.back() == "complete");
  Rig a;
  a.fail_start = true;
  o = DpLinkTrainer::train(a, a, sink_caps(), 0x0A, 4);
  CHECK(o.result == DpLinkTrainer::Result::SourceStart && index_of(a.log, "pattern 01") == a.log.size());
  Rig p;
  p.fail_pattern = true;
  CHECK(DpLinkTrainer::train(p, p, sink_caps(), 0x0A, 4).result == DpLinkTrainer::Result::PatternWrite);
  Rig dr;
  dr.fail_drive = true;
  CHECK(DpLinkTrainer::train(dr, dr, sink_caps(), 0x0A, 4).result == DpLinkTrainer::Result::PatternWrite);      // (the first drive is part of setting the pattern up)
  Rig c;
  c.fail_complete = true;
  CHECK(DpLinkTrainer::train(c, c, sink_caps(), 0x0A, 4).result == DpLinkTrainer::Result::SourceComplete);
  // a failure of the training is not hidden by a failed clean up
  Rig f;
  f.never_equalize = true;
  f.fail_complete = true;
  CHECK(DpLinkTrainer::train(f, f, sink_caps(), 0x0A, 4).result == DpLinkTrainer::Result::ChannelEqualization);
}

static void test_link_config() {
  Dpcd::Capabilities c = sink_caps();
  // 1080p60 (148.5 MHz) at 24 bits: 2.7 Gbit/s on two lanes carries 270000 * 2 * 8 / 24 = 180000: the sink has four lanes, so two at 2.7
  DpLinkConfig k = DpLinkConfig::choose(c, 148500, 24);
  CHECK(k.lanes == 2 && k.rate_khz == 270000 && k.bandwidth_code() == 10);
  // 640x480 (25.175 MHz): one lane at 2.7 (90000 >= 25175)
  k = DpLinkConfig::choose(c, 25175, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 270000);
  // 4K60 (594 MHz): not at 2.7 on four lanes (360000): the first rate that does in the second loop: 5.4 on four lanes is 720000 >= 594000, at 5.4 on two lanes it is 360000: four
  k = DpLinkConfig::choose(c, 594000, 24);
  CHECK(k.lanes == 4 && k.rate_khz == 540000 && k.bandwidth_code() == 20);
  // 4K30 (297 MHz): 5.4 on two lanes is 360000 >= 297000, but 2.7 on four lanes is 360000 as well, and comes first
  k = DpLinkConfig::choose(c, 297000, 24);
  CHECK(k.lanes == 4 && k.rate_khz == 270000);
  // a sink of two lanes: the first loop looks at one lane only
  c.max_lanes = 2;
  k = DpLinkConfig::choose(c, 148500, 24);
  CHECK(k.lanes == 2 && k.rate_khz == 270000);
  k = DpLinkConfig::choose(c, 90000, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 270000);
  // a one lane sink does not try the first loop: the lowest rate that carries it
  c.max_lanes = 1;
  k = DpLinkConfig::choose(c, 25175, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 162000);
  k = DpLinkConfig::choose(c, 100000, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 540000);
  // the carrying capacity is enough when it equals the clock: 90000 on one lane at 2.7 (first loop), and 54000 on one lane at 1.62 (second loop)
  c = sink_caps();
  k = DpLinkConfig::choose(c, 90000, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 270000);
  c.max_lanes = 1;
  k = DpLinkConfig::choose(c, 54000, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 162000);
  k = DpLinkConfig::choose(c, 54001, 24);
  CHECK(k.lanes == 1 && k.rate_khz == 270000);
  // the sink's rate limits it: 1.62 Gbit/s on one lane carries 54000 (at 24 bits)
  c = sink_caps();
  c.max_link_rate_code = 0x06;
  k = DpLinkConfig::choose(c, 148500, 24);
  CHECK(k.lanes == 4 && k.rate_khz == 162000);       // (the sink cannot do 2.7, so the first loop is not tried: 4 * 54000 = 216000 carries it)
  // nothing carries it: four lanes at the lowest rate
  c = sink_caps();
  k = DpLinkConfig::choose(c, 2000000, 24);
  CHECK(k.lanes == 4 && k.rate_khz == 162000);
  // deeper colour needs more: 30 bits for 148.5 MHz is 270000 * 2 * 8 / 30 = 144000 < 148500: the first loop finds nothing, and in the second the lowest rate with four lanes is enough
  k = DpLinkConfig::choose(sink_caps(), 148500, 30);
  CHECK(k.lanes == 4 && k.rate_khz == 162000);
  // bits per pixel 0 is taken as 24
  CHECK(DpLinkConfig::choose(sink_caps(), 148500, 0).lanes == 2);
}

int main() {
  test_clean_training();
  test_options();
  test_adjusting();
  test_clock_recovery_failures();
  test_equalization();
  test_patterns();
  test_failures();
  test_link_config();
  printf(g_failed ? "DpLinkTrainTest: %d FAILED\n" : "DpLinkTrainTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
