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

/* winmm.dll's wave output over the kernel's PipeWire graph: device and caps, format queries, a 400 Hz tone (44.1 kHz, 16-bit stereo) in four buffers with
 * an event callback and one with a function callback, position, pause and restart, reset, volume, PlaySound from memory, close. Run by
 * tests/pipewire_emulated.sh on the CPU emulator, at the same time as a Linux program and a native stream. Prints "winmmtest: N checks, M failed". */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

/* (a sine of its own: the test is about audio, not the C runtime's maths) */
static double tsin(double x) {
  const double pi = 3.14159265358979323846;
  while (x > pi) x -= 2 * pi;
  while (x < -pi) x += 2 * pi;
  double term = x, sum = x;
  for (int n = 1; n < 10; ++n) {
    term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)

#define RATE 44100
#define FRAMES (RATE / 4)      /* 250 ms a buffer */
static short buffers[4][FRAMES * 2];
static volatile LONG function_done;

static void CALLBACK on_wave(HWAVEOUT h, UINT message, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR p2) {
  (void)h; (void)p1; (void)p2;
  if (message == WOM_DONE && instance == 0x1234) InterlockedIncrement(&function_done);
}

static void fill(short *b, int frames, int start) {
  for (int i = 0; i < frames; ++i) {
    const short v = (short)(8000.0 * tsin(2.0 * 3.14159265358979 * 400.0 * (start + i) / RATE));
    b[2 * i] = b[2 * i + 1] = v;
  }
}

int main(void) {
  CHECK(waveOutGetNumDevs() == 1);
  WAVEOUTCAPSW caps;
  CHECK(waveOutGetDevCapsW(0, &caps, sizeof caps) == MMSYSERR_NOERROR && caps.wChannels == 2 && (caps.dwFormats & WAVE_FORMAT_4S16));
  CHECK(waveOutGetDevCapsW(WAVE_MAPPER, &caps, sizeof caps) == MMSYSERR_NOERROR);
  CHECK(waveOutGetDevCapsW(5, &caps, sizeof caps) == MMSYSERR_BADDEVICEID);

  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
  CHECK(waveOutOpen(NULL, WAVE_MAPPER, &f, 0, 0, WAVE_FORMAT_QUERY) == MMSYSERR_NOERROR);
  WAVEFORMATEX fl = {3 /* IEEE float */, 2, 48000, 48000 * 8, 8, 32, 0};
  CHECK(waveOutOpen(NULL, WAVE_MAPPER, &fl, 0, 0, WAVE_FORMAT_QUERY) == MMSYSERR_NOERROR);
  WAVEFORMATEX bad = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 12, 0};
  CHECK(waveOutOpen(NULL, WAVE_MAPPER, &bad, 0, 0, WAVE_FORMAT_QUERY) == WAVERR_BADFORMAT);

  HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
  HWAVEOUT h = NULL;
  CHECK(waveOutOpen(&h, WAVE_MAPPER, &f, (DWORD_PTR)event, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR && h);
  WaitForSingleObject(event, 1000);      /* (WOM_OPEN sets it) */
  WAVEHDR headers[4];
  memset(headers, 0, sizeof headers);
  for (int i = 0; i < 4; ++i) {
    fill(buffers[i], FRAMES, i * FRAMES);
    headers[i].lpData = (LPSTR)buffers[i];
    headers[i].dwBufferLength = sizeof buffers[i];
    CHECK(waveOutWrite(h, &headers[i], sizeof headers[i]) == WAVERR_UNPREPARED || i > 0);
    CHECK(waveOutPrepareHeader(h, &headers[i], sizeof headers[i]) == MMSYSERR_NOERROR);
  }
  const DWORD start = GetTickCount();
  for (int i = 0; i < 4; ++i) CHECK(waveOutWrite(h, &headers[i], sizeof headers[i]) == MMSYSERR_NOERROR);
  CHECK(waveOutWrite(h, &headers[0], sizeof headers[0]) == WAVERR_STILLPLAYING);
  CHECK(waveOutUnprepareHeader(h, &headers[3], sizeof headers[3]) == WAVERR_STILLPLAYING);
  CHECK(waveOutClose(h) == WAVERR_STILLPLAYING);
  /* the position moves while it plays */
  Sleep(300);
  MMTIME t;
  memset(&t, 0, sizeof t);
  t.wType = TIME_SAMPLES;
  CHECK(waveOutGetPosition(h, &t, sizeof t) == MMSYSERR_NOERROR && t.wType == TIME_SAMPLES && t.u.sample > 1000 && t.u.sample < 4 * FRAMES);
  /* pause holds it, restart goes on */
  CHECK(waveOutPause(h) == MMSYSERR_NOERROR);
  Sleep(50);
  MMTIME a, b;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  a.wType = b.wType = TIME_BYTES;
  waveOutGetPosition(h, &a, sizeof a);
  Sleep(100);
  waveOutGetPosition(h, &b, sizeof b);
  CHECK(a.u.cb == b.u.cb);
  CHECK(waveOutRestart(h) == MMSYSERR_NOERROR);
  for (DWORD waited = 0; !(headers[3].dwFlags & WHDR_DONE) && waited < 10000; waited += 10) WaitForSingleObject(event, 10);
  const DWORD took = GetTickCount() - start;
  for (int i = 0; i < 4; ++i) CHECK((headers[i].dwFlags & WHDR_DONE) && !(headers[i].dwFlags & WHDR_INQUEUE));
  CHECK(took >= 900 && took < 5000);      /* 1 s of sound (and the pause) took about that long */
  MMTIME end;
  memset(&end, 0, sizeof end);
  end.wType = TIME_MS;
  CHECK(waveOutGetPosition(h, &end, sizeof end) == MMSYSERR_NOERROR && end.u.ms >= 990 && end.u.ms <= 1000);
  /* volume */
  DWORD v = 0;
  CHECK(waveOutSetVolume(h, 0x80008000) == MMSYSERR_NOERROR && waveOutGetVolume(h, &v) == MMSYSERR_NOERROR && (v & 0xFFFF) >= 0x7FF0 && (v & 0xFFFF) <= 0x8010);
  CHECK(waveOutSetVolume(h, 0xFFFFFFFF) == MMSYSERR_NOERROR);
  /* reset returns what is queued at once */
  for (int i = 0; i < 2; ++i) CHECK(waveOutWrite(h, &headers[i], sizeof headers[i]) == MMSYSERR_NOERROR);
  CHECK(waveOutReset(h) == MMSYSERR_NOERROR && (headers[0].dwFlags & WHDR_DONE) && (headers[1].dwFlags & WHDR_DONE));
  t.wType = TIME_BYTES;
  CHECK(waveOutGetPosition(h, &t, sizeof t) == MMSYSERR_NOERROR && t.u.cb == 0);
  for (int i = 0; i < 4; ++i) CHECK(waveOutUnprepareHeader(h, &headers[i], sizeof headers[i]) == MMSYSERR_NOERROR);
  CHECK(waveOutClose(h) == MMSYSERR_NOERROR);
  CHECK(waveOutClose(h) == MMSYSERR_INVALHANDLE);

  /* a function callback, 16-bit mono at 22.05 kHz */
  WAVEFORMATEX m = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
  CHECK(waveOutOpen(&h, 0, &m, (DWORD_PTR)on_wave, 0x1234, CALLBACK_FUNCTION) == MMSYSERR_NOERROR);
  static short mono[2205];
  for (int i = 0; i < 2205; ++i) mono[i] = (short)(4000.0 * tsin(2.0 * 3.14159265358979 * 400.0 * i / 22050));
  WAVEHDR one;
  memset(&one, 0, sizeof one);
  one.lpData = (LPSTR)mono;
  one.dwBufferLength = sizeof mono;
  CHECK(waveOutPrepareHeader(h, &one, sizeof one) == MMSYSERR_NOERROR && waveOutWrite(h, &one, sizeof one) == MMSYSERR_NOERROR);
  for (int waited = 0; !function_done && waited < 5000; waited += 10) Sleep(10);
  CHECK(function_done == 1);
  waveOutUnprepareHeader(h, &one, sizeof one);
  CHECK(waveOutClose(h) == MMSYSERR_NOERROR);

  /* PlaySound of a WAVE image in memory (synchronous: returns once it has played) */
  static unsigned char wav[44 + 4410 * 2];
  const DWORD data = 4410 * 2;
  memcpy(wav, "RIFF", 4); *(DWORD *)(wav + 4) = 36 + data; memcpy(wav + 8, "WAVEfmt ", 8); *(DWORD *)(wav + 16) = 16;
  WAVEFORMATEX pf = {WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0};
  memcpy(wav + 20, &pf, 16); memcpy(wav + 36, "data", 4); *(DWORD *)(wav + 40) = data;
  for (int i = 0; i < 4410; ++i) ((short *)(wav + 44))[i] = (short)(3000.0 * tsin(2.0 * 3.14159265358979 * 400.0 * i / 44100));
  const DWORD before = GetTickCount();
  CHECK(PlaySoundA((LPCSTR)wav, NULL, SND_MEMORY | SND_SYNC));
  CHECK(GetTickCount() - before >= 80);
  CHECK(!PlaySoundA("C:\\no such file.wav", NULL, SND_FILENAME | SND_NODEFAULT));
  CHECK(PlaySoundA(NULL, NULL, 0));
  char text[128];
  CHECK(waveOutGetErrorTextA(WAVERR_STILLPLAYING, text, sizeof text) == MMSYSERR_NOERROR && text[0]);
  printf("winmmtest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
