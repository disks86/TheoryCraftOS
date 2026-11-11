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

/* An X11 session in the raw protocol (little-endian), with no library: what tests/linux/x11test.c (a freestanding Linux program) and
 * tests/windows/x11wintest.c (a Windows program over ws2_32's AF_UNIX) do once they have a connection to the kernel's X server. The program gives
 * it a way to send and to receive bytes, and a way to report; it opens a window titled `title`, draws into it, reads the pixels back, and checks the
 * replies. Needs no C library. */
#ifndef TCOS_TESTS_X11SESSION_H
#define TCOS_TESTS_X11SESSION_H

typedef struct X11Io {
  void* context;
  int (*send)(void* context, const unsigned char* data, unsigned long size);      /* 1: all of it went */
  int (*recv)(void* context, unsigned char* data, unsigned long size);            /* 1: exactly `size` bytes came */
  void (*fail)(void* context, const char* what, int line);
} X11Io;

typedef struct X11Session {
  X11Io io;
  unsigned base, root, checks, failed;
  unsigned char reply[4096];
  unsigned long reply_size;
} X11Session;

static void x11_check(X11Session* s, int ok, const char* what, int line) {
  ++s->checks;
  if (ok) return;
  ++s->failed;
  s->io.fail(s->io.context, what, line);
}
#define X11_CHECK(s, ok) x11_check((s), (ok) ? 1 : 0, #ok, __LINE__)

static unsigned x11_u16(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned x11_u32(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

/* A request being built: opcode, data byte, then the body; the length is filled in by x11_send. */
typedef struct X11Request {
  unsigned char b[512];
  unsigned n;
} X11Request;
static void x11_begin(X11Request* r, unsigned char opcode, unsigned char data) {
  r->b[0] = opcode;
  r->b[1] = data;
  r->b[2] = r->b[3] = 0;
  r->n = 4;
}
static void x11_8(X11Request* r, unsigned v) { r->b[r->n++] = (unsigned char)v; }
static void x11_16(X11Request* r, unsigned v) { x11_8(r, v); x11_8(r, v >> 8); }
static void x11_32(X11Request* r, unsigned v) { x11_16(r, v); x11_16(r, v >> 16); }
static void x11_str(X11Request* r, const char* text) { while (*text) x11_8(r, (unsigned char)*text++); }
static int x11_send(X11Session* s, X11Request* r) {
  while (r->n % 4) x11_8(r, 0);
  r->b[2] = (unsigned char)(r->n / 4);
  r->b[3] = (unsigned char)(r->n / 4 >> 8);
  return s->io.send(s->io.context, r->b, r->n);
}
static unsigned x11_len(const char* text) {
  unsigned n = 0;
  while (text[n]) ++n;
  return n;
}

/* The next reply (events are skipped). 1: a reply, 0: an error or the connection is gone. */
static int x11_reply(X11Session* s) {
  for (;;) {
    if (!s->io.recv(s->io.context, s->reply, 32)) return 0;
    s->reply_size = 32;
    if (s->reply[0] == 1) {
      const unsigned long extra = (unsigned long)x11_u32(s->reply + 4) * 4;
      if (extra > sizeof s->reply - 32) return 0;
      if (extra && !s->io.recv(s->io.context, s->reply + 32, extra)) return 0;
      s->reply_size += extra;
      return 1;
    }
    if (s->reply[0] == 0) return 0;      /* an error */
  }
}

/* Connection setup. 1 when the server accepted it. */
static int x11_setup(X11Session* s) {
  static const unsigned char hello[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  unsigned char head[8], rest[4096];
  if (!s->io.send(s->io.context, hello, 12) || !s->io.recv(s->io.context, head, 8) || head[0] != 1) return 0;
  const unsigned long size = (unsigned long)x11_u16(head + 6) * 4;
  if (size > sizeof rest || size < 32 || !s->io.recv(s->io.context, rest, size)) return 0;
  s->base = x11_u32(rest + 4);
  const unsigned vendor = x11_u16(rest + 16), formats = rest[21];
  const unsigned long screen = 32 + ((vendor + 3) & ~3u) + 8ul * formats;
  if (screen + 4 > size) return 0;
  s->root = x11_u32(rest + screen);
  return 1;
}

/* The session: a window of its own, drawn and read back, an atom, an extension. Returns the window (0 when it could not be made). */
static unsigned x11_run(X11Session* s, const char* title, unsigned colour) {
  X11Request r;
  X11_CHECK(s, x11_setup(s));
  if (s->failed) return 0;
  X11_CHECK(s, s->base != 0 && s->root != 0);
  const unsigned w = s->base | 1, gc = s->base | 2;
  x11_begin(&r, 1, 0);      /* CreateWindow: 160x100, white, Exposure | StructureNotify */
  x11_32(&r, w); x11_32(&r, s->root); x11_16(&r, 40); x11_16(&r, 40); x11_16(&r, 160); x11_16(&r, 100); x11_16(&r, 0); x11_16(&r, 1); x11_32(&r, 0);
  x11_32(&r, 0x2 | 0x800); x11_32(&r, 0xFFFFFF); x11_32(&r, (1u << 15) | (1u << 17));
  X11_CHECK(s, x11_send(s, &r));
  x11_begin(&r, 18, 0);     /* ChangeProperty WM_NAME STRING */
  x11_32(&r, w); x11_32(&r, 39); x11_32(&r, 31); x11_8(&r, 8); x11_8(&r, 0); x11_8(&r, 0); x11_8(&r, 0); x11_32(&r, x11_len(title)); x11_str(&r, title);
  X11_CHECK(s, x11_send(s, &r));
  x11_begin(&r, 8, 0);      /* MapWindow */
  x11_32(&r, w);
  X11_CHECK(s, x11_send(s, &r));
  x11_begin(&r, 55, 0);     /* CreateGC with the foreground */
  x11_32(&r, gc); x11_32(&r, w); x11_32(&r, 0x4); x11_32(&r, colour);
  X11_CHECK(s, x11_send(s, &r));
  x11_begin(&r, 70, 0);     /* PolyFillRectangle */
  x11_32(&r, w); x11_32(&r, gc); x11_16(&r, 0); x11_16(&r, 0); x11_16(&r, 40); x11_16(&r, 30);
  X11_CHECK(s, x11_send(s, &r));
  x11_begin(&r, 73, 2);     /* GetImage ZPixmap of two pixels: one drawn, one not */
  x11_32(&r, w); x11_16(&r, 39); x11_16(&r, 5); x11_16(&r, 2); x11_16(&r, 1); x11_32(&r, 0xFFFFFFFFu);
  X11_CHECK(s, x11_send(s, &r));
  X11_CHECK(s, x11_reply(s) && s->reply[1] == 24 && s->reply_size == 40);
  if (s->reply_size == 40) {
    X11_CHECK(s, (x11_u32(s->reply + 32) & 0xFFFFFF) == colour);
    X11_CHECK(s, (x11_u32(s->reply + 36) & 0xFFFFFF) == 0xFFFFFF);
  }
  x11_begin(&r, 16, 0);     /* InternAtom, then its name back */
  x11_16(&r, 13); x11_16(&r, 0); x11_str(&r, "TCOS_X11_TEST");
  X11_CHECK(s, x11_send(s, &r));
  X11_CHECK(s, x11_reply(s));
  const unsigned atom = x11_u32(s->reply + 8);
  X11_CHECK(s, atom > 68);
  x11_begin(&r, 17, 0);
  x11_32(&r, atom);
  X11_CHECK(s, x11_send(s, &r));
  X11_CHECK(s, x11_reply(s) && x11_u16(s->reply + 8) == 13 && s->reply[32] == 'T' && s->reply[44] == 'T');
  x11_begin(&r, 98, 0);     /* QueryExtension BIG-REQUESTS */
  x11_16(&r, 12); x11_16(&r, 0); x11_str(&r, "BIG-REQUESTS");
  X11_CHECK(s, x11_send(s, &r));
  X11_CHECK(s, x11_reply(s) && s->reply[8] == 1);
  x11_begin(&r, 20, 0);     /* GetProperty WM_NAME: the title as sent */
  x11_32(&r, w); x11_32(&r, 39); x11_32(&r, 0); x11_32(&r, 0); x11_32(&r, 64);
  X11_CHECK(s, x11_send(s, &r));
  X11_CHECK(s, x11_reply(s) && x11_u32(s->reply + 16) == x11_len(title) && s->reply[32] == (unsigned char)title[0]);
  return w;
}

#endif
