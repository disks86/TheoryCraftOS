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

// Host-side tests for sample rate and format conversion (Resampler, FormatConverter) and panning.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Audio/Pan.hpp"
#include "Audio/Resampler.hpp"

using namespace audio;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

static void test_copy_and_invalid() {
  Resampler r;
  CHECK(!r.configure(100, 48000) && !r.configure(48000, 400000));
  CHECK(r.configure(48000, 48000));
  int16_t in[20], out[20]; uint32_t used;
  for (int i = 0; i < 20; ++i) in[i] = static_cast<int16_t>(i * 100);
  CHECK(r.process(in, 10, out, 6, &used) == 6 && used == 6 && memcmp(in, out, 12 * 2) == 0);
}

static void test_up() {
  Resampler r;
  CHECK(r.configure(24000, 48000));
  int16_t in[200], out[600]; uint32_t used;
  for (int i = 0; i < 100; ++i) in[2 * i] = in[2 * i + 1] = 1000;  // DC
  uint32_t made = r.process(in, 100, out, 600, &used);
  CHECK(used == 100 && made >= 196 && made <= 200);
  for (uint32_t i = 0; i < made * 2; ++i) CHECK(out[i] == 1000);

  // A ramp comes out as a ramp, twice as dense, strictly non-decreasing and never past the input.
  r.reset();
  for (int i = 0; i < 100; ++i) in[2 * i] = in[2 * i + 1] = static_cast<int16_t>(i * 100);
  made = r.process(in, 100, out, 600, &used);
  bool monotonic = true;
  for (uint32_t i = 1; i < made; ++i) monotonic &= out[2 * i] >= out[2 * (i - 1)];
  CHECK(monotonic && made > 190 && out[2 * 10] >= 400 && out[2 * 10] <= 600);  // frame 10 of the output ~ input 5 (= 500)

  // Fed in small pieces gives exactly the same output as in one go.
  int16_t whole[600], piecewise[600];
  r.reset();
  uint32_t whole_made = r.process(in, 100, whole, 600, &used);
  r.reset();
  uint32_t total = 0, pos = 0;
  while (pos < 100) {
    const uint32_t chunk = pos + 7 < 100 ? 7 : 100 - pos;
    total += r.process(in + 2 * pos, chunk, piecewise + 2 * total, 600 - total, &used);
    CHECK(used == chunk);
    pos += used;
  }
  CHECK(total == whole_made && memcmp(whole, piecewise, total * 4) == 0);

  // A small output buffer stops it without losing input: keep calling.
  r.reset();
  total = 0; pos = 0;
  while (pos < 100 || total < whole_made) {
    uint32_t n = r.process(in + 2 * pos, 100 - pos, piecewise + 2 * total, 3, &used);
    pos += used; total += n;
    if (n == 0 && used == 0) break;
  }
  CHECK(total == whole_made && memcmp(whole, piecewise, total * 4) == 0);
}

static void test_down() {
  Resampler r;
  CHECK(r.configure(48000, 8000));
  int16_t in[1200], out[300]; uint32_t used;
  for (int i = 0; i < 600; ++i) in[2 * i] = in[2 * i + 1] = 1000;
  uint32_t made = r.process(in, 600, out, 300, &used);
  CHECK(used == 600 && made == 100);
  for (uint32_t i = 0; i < made * 2; ++i) CHECK(out[i] == 1000);

  // A tone far above the new Nyquist rate (12 kHz at 48k, going down to 8k) is strongly reduced.
  for (int i = 0; i < 600; ++i) in[2 * i] = in[2 * i + 1] = (i / 2) % 2 ? 8000 : -8000;  // period 4 frames = 12 kHz
  r.reset();
  made = r.process(in, 600, out, 300, &used);
  int peak = 0;
  for (uint32_t i = 2; i < made * 2; ++i) peak = out[i] > peak ? out[i] : peak < -out[i] ? -out[i] : peak;
  CHECK(peak < 3000);  // a box filter is no brick wall, but 8000 comes out well under half

  // Non-integer ratio: 44100 -> 48000 keeps the length (within a frame), 48000 -> 44100 too.
  Resampler up, down;
  CHECK(up.configure(44100, 48000) && down.configure(48000, 44100));
  static int16_t big[2 * 4410], mid[2 * 5000], back[2 * 5000];
  for (int i = 0; i < 4410; ++i) big[2 * i] = big[2 * i + 1] = static_cast<int16_t>(i % 100 * 50);
  uint32_t made_up = up.process(big, 4410, mid, 5000, &used);
  CHECK(used == 4410 && made_up >= 4797 && made_up <= 4800);
  uint32_t made_down = down.process(mid, made_up, back, 5000, &used);
  CHECK(used == made_up && made_down >= 4407 && made_down <= 4410);
}

static void test_format_converter() {
  FormatConverter c;
  StreamFormat mono8{8000, 1, SampleType::U8}, native;
  CHECK(!c.configure(StreamFormat{1000, 1, SampleType::S16}, native));
  CHECK(c.configure(mono8, native));
  uint8_t in[16];
  for (int i = 0; i < 16; ++i) in[i] = (i % 2) ? 255 : 128;
  uint8_t out[4096]; uint32_t used;
  uint32_t made = c.convert(ConstByteView(in, 16), ByteView(out, 4096), &used);
  CHECK(used == 16 && made >= 15 * 6 * 4 - 12 && made <= 16 * 6 * 4);  // 8 kHz -> 48 kHz: six times the frames, 4 bytes each
  int16_t* samples = reinterpret_cast<int16_t*>(out);
  CHECK(samples[0] == samples[1]);                        // mono became stereo
  CHECK(samples[0] == 0);                                 // 128 is silence

  // Out buffer limits: nothing is dropped, the input not used is reported.
  FormatConverter limited;
  CHECK(limited.configure(StreamFormat{48000, 1, SampleType::U8}, native));
  made = limited.convert(ConstByteView(in, 16), ByteView(out, 20), &used);
  CHECK(made == 20 && used == 5);

  // Stereo S16 -> mono U8 at the same rate: average, then 8 bits.
  FormatConverter down;
  CHECK(down.configure(native, mono8));
  int16_t loud[2 * 600];
  for (int i = 0; i < 600; ++i) { loud[2 * i] = 16000; loud[2 * i + 1] = 8000; }
  made = down.convert(ConstByteView(reinterpret_cast<uint8_t*>(loud), sizeof loud), ByteView(out, 200), &used);
  CHECK(made == 100);
  CHECK(out[50] == 128 + 12000 / 256 || out[50] == 128 + 12000 / 256 + 1);
  CHECK(down.input_bytes_for(100) == 600 * 4 && down.output_bytes_for(2400) == 100);
}

static void test_pan() {
  int32_t l, r, sl, sr;
  pan_gains(kPanLeft, true, &l, &r);
  CHECK(l == 32768 && r == 0);
  pan_gains(kPanRight, true, &l, &r);
  CHECK(l == 0 && r == 32768);
  pan_gains(kPanCenter, true, &l, &r);
  CHECK(l == r && l > 23000 && l < 23300);  // 0.707
  // Equal power at every position.
  for (int p = -100; p <= 100; ++p) {
    pan_gains(p, true, &l, &r);
    const double power = (double(l) * l + double(r) * r) / (32768.0 * 32768.0);
    CHECK(power > 0.995 && power < 1.005);
  }
  // Stereo: the centre is untouched; off-centre only the far channel goes down.
  pan_gains(kPanCenter, false, &sl, &sr);
  CHECK(sl == 32768 && sr == 32768);
  pan_gains(50, false, &sl, &sr);
  CHECK(sl < 32768 && sl > 0 && sr == 32768);
  pan_gains(-50, false, &sl, &sr);
  CHECK(sr < 32768 && sr > 0 && sl == 32768);
  pan_gains(1000, false, &sl, &sr);  // clamped
  CHECK(sl == 0 && sr == 32768);
}

int main() {
  test_copy_and_invalid();
  test_up();
  test_down();
  test_format_converter();
  test_pan();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("audio conversion tests passed\n");
  return 0;
}
