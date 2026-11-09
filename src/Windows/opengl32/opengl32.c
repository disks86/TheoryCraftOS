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

/* opengl32.dll: OpenGL for Windows programs over the kernel's OpenGL (src/OpenGl). Every GL command is encoded into the calling thread's command buffer by the generated entry
 * points (src/OpenGl/generated/GlEntries.inc) and runs in the kernel (NtTcosOpenGl, src/Process/WindowsOpenGl.hpp); this file adds WGL: contexts (HGLRC = the kernel's context
 * number), making them current on a window's device context, swapping and presenting, pixel formats (gdi32's), wglGetProcAddress over every command the kernel has, the WGL
 * extensions programs look for (ARB_create_context, ARB_pixel_format, EXT_swap_control ...), and font display lists.
 *
 * A frame reaches the window through the kernel when the window is a desktop frame (win32k redirection); otherwise the library reads it back and draws it with SetDIBitsToDevice. */
#include <windows.h>
#include <winternl.h>
#pragma GCC diagnostic ignored "-Wattributes"      /* (this library defines the wgl* functions wingdi.h declares as imports) */
#define GLAPI
#define APIENTRY WINAPI
#include "../../OpenGl/GlClient.h"
#define GL_COMPILE 0x1300
#define GL_UNPACK_ALIGNMENT 0x0CF5

__declspec(dllimport) NTSTATUS NTAPI NtTcosOpenGl(void *request);

typedef struct TcosGlRequest {
  ULONG size, op, context, share;
  ULONGLONG window, data;
  ULONG length, width, height, flags;
  ULONGLONG result;
  ULONG major, minor;
} TcosGlRequest;
typedef char TcosGlRequestSize[sizeof(TcosGlRequest) == 64 ? 1 : -1];
enum { OP_CREATE = 1, OP_DELETE, OP_MAKE_CURRENT, OP_EXECUTE, OP_SWAP, OP_READ_FRONT, OP_SHARE_LISTS, OP_FLUSHED };

static NTSTATUS gl_call(TcosGlRequest *r) {
  r->size = sizeof(*r);
  return NtTcosOpenGl(r);
}

/* ---- Per thread: the command buffer, the current context and its device context. */
typedef struct ThreadGl {
  GlStream stream;
  ULONG context;
  HDC dc, read_dc;
  ULONGLONG drawable;
} ThreadGl;

static DWORD g_tls = TLS_OUT_OF_INDEXES;
static ThreadGl g_fallback;      /* when a thread's own cannot be made: commands then go nowhere */

static ThreadGl *thread_gl(void) {
  if (g_tls == TLS_OUT_OF_INDEXES) return &g_fallback;
  ThreadGl *t = (ThreadGl *)TlsGetValue(g_tls);
  if (!t) {
    t = (ThreadGl *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadGl));
    if (!t) return &g_fallback;
    TlsSetValue(g_tls, t);
  }
  return t;
}

GlStream *gl_stream_current(void) { return &thread_gl()->stream; }

uint64_t gl_stream_submit(const uint32_t *words, uint32_t count) {
  ThreadGl *t = thread_gl();
  if (!t->context || t == &g_fallback || count == 0) return 0;
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_EXECUTE;
  r.data = (ULONGLONG)(ULONG_PTR)words;
  r.length = count * 4;
  if (gl_call(&r) < 0) return 0;
  return r.result;
}

#include "../../OpenGl/generated/GlEntries.inc"

/* ---- What is drawn to: a window's client area, or the bitmap selected into a memory device context. The kernel knows a drawable by a number: the HWND, or the HDC with
 * the top bit set for a bitmap. */
#define BITMAP_DRAWABLE 0x8000000000000000ull

static ULONGLONG dc_drawable(HDC dc) {
  if (!dc) return 0;
  HWND window = WindowFromDC(dc);
  if (window) return (ULONGLONG)(ULONG_PTR)window;
  return GetObjectType(dc) == OBJ_MEMDC ? BITMAP_DRAWABLE | (ULONGLONG)(ULONG_PTR)dc : 0;
}

static void drawable_size(HDC dc, ULONGLONG drawable, ULONG *w, ULONG *h) {
  *w = 1;
  *h = 1;
  if (drawable & BITMAP_DRAWABLE) {
    BITMAP bm;
    HGDIOBJ bitmap = GetCurrentObject(dc, OBJ_BITMAP);
    if (bitmap && GetObjectA(bitmap, sizeof(bm), &bm)) {
      if (bm.bmWidth > 0) *w = (ULONG)bm.bmWidth;
      if (bm.bmHeight > 0) *h = (ULONG)bm.bmHeight;
    }
    return;
  }
  RECT rc;
  if (drawable && GetClientRect((HWND)(ULONG_PTR)drawable, &rc)) {
    if (rc.right > rc.left) *w = (ULONG)(rc.right - rc.left);
    if (rc.bottom > rc.top) *h = (ULONG)(rc.bottom - rc.top);
  }
}

/* The surface flags of the DC's pixel format (1 double buffered, 2 depth, 4 stencil, 8 accumulation, 16 alpha). */
static ULONG format_flags(HDC dc) {
  PIXELFORMATDESCRIPTOR pfd;
  int format = GetPixelFormat(dc);
  if (format <= 0 || !DescribePixelFormat(dc, format, sizeof(pfd), &pfd)) return 1 | 2 | 4 | 16;
  ULONG f = 0;
  if (pfd.dwFlags & PFD_DOUBLEBUFFER) f |= 1;
  if (pfd.cDepthBits) f |= 2;
  if (pfd.cStencilBits) f |= 4;
  if (pfd.cAccumBits) f |= 8;
  if (pfd.cAlphaBits) f |= 16;
  /* the accumulation buffer is always offered: programs ask for it in a format that has none */
  return f | 8;
}

/* ---- Contexts */
static ULONG create_context(HDC dc, ULONG share, ULONG major, ULONG minor, ULONG flags) {
  if (dc && GetPixelFormat(dc) <= 0) {
    SetLastError(ERROR_INVALID_PIXEL_FORMAT);
    return 0;
  }
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_CREATE;
  r.share = share;
  r.major = major;
  r.minor = minor;
  r.flags = flags;
  if (gl_call(&r) < 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return (ULONG)r.result;
}

HGLRC WINAPI wglCreateContext(HDC dc) { return (HGLRC)(ULONG_PTR)create_context(dc, 0, 3, 3, 0); }
HGLRC WINAPI wglCreateLayerContext(HDC dc, int layer) { return layer == 0 ? wglCreateContext(dc) : NULL; }

static BOOL make_current(HDC dc, HDC read_dc, HGLRC rc) {
  ThreadGl *t = thread_gl();
  if (t == &g_fallback) return FALSE;
  gl_stream_flush();
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_MAKE_CURRENT;
  r.context = (ULONG)(ULONG_PTR)rc;
  if (!rc && !t->context) {      /* (releasing when nothing is current: Windows fails that) */
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (rc) {
    const ULONGLONG drawable = dc_drawable(dc);
    if (!drawable || GetPixelFormat(dc) <= 0) {
      SetLastError(drawable ? ERROR_INVALID_PIXEL_FORMAT : ERROR_INVALID_HANDLE);
      return FALSE;
    }
    r.window = drawable;
    drawable_size(dc, drawable, &r.width, &r.height);
    r.flags = format_flags(dc);
  }
  const NTSTATUS st = gl_call(&r);
  if (st < 0) {
    SetLastError(st == (NTSTATUS)0xC0000708 ? ERROR_BUSY : ERROR_INVALID_HANDLE);
    return FALSE;
  }
  t->context = (ULONG)(ULONG_PTR)rc;
  t->dc = rc ? dc : NULL;
  t->read_dc = rc ? read_dc : NULL;
  t->drawable = rc ? dc_drawable(dc) : 0;
  return TRUE;
}

BOOL WINAPI wglMakeCurrent(HDC dc, HGLRC rc) {
  if (!rc && !dc && !thread_gl()->context) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!rc && !thread_gl()->context) return TRUE;      /* (with a DC and nothing current: nothing to do) */
  return make_current(dc, dc, rc);
}
static BOOL WINAPI wglMakeContextCurrentARB(HDC draw, HDC read, HGLRC rc) { return make_current(draw, read, rc); }

BOOL WINAPI wglDeleteContext(HGLRC rc) {
  if (!rc) return FALSE;
  ThreadGl *t = thread_gl();
  if (t->context == (ULONG)(ULONG_PTR)rc) make_current(NULL, NULL, NULL);
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_DELETE;
  r.context = (ULONG)(ULONG_PTR)rc;
  if (gl_call(&r) < 0) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  return TRUE;
}

HGLRC WINAPI wglGetCurrentContext(void) { return (HGLRC)(ULONG_PTR)thread_gl()->context; }
HDC WINAPI wglGetCurrentDC(void) { return thread_gl()->dc; }
static HDC WINAPI wglGetCurrentReadDCARB(void) { return thread_gl()->read_dc; }

BOOL WINAPI wglShareLists(HGLRC source, HGLRC target) {
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_SHARE_LISTS;
  r.share = (ULONG)(ULONG_PTR)source;
  r.context = (ULONG)(ULONG_PTR)target;
  return gl_call(&r) >= 0;
}

BOOL WINAPI wglCopyContext(HGLRC source, HGLRC target, UINT mask) {
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return FALSE;
}

/* ---- Presenting */
static void blit_front(HDC dc, ULONGLONG drawable) {
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_READ_FRONT;
  r.window = drawable;
  if (gl_call(&r) < 0 || !r.width || !r.height) return;
  const SIZE_T bytes = (SIZE_T)r.width * r.height * 4;
  void *pixels = HeapAlloc(GetProcessHeap(), 0, bytes);
  if (!pixels) return;
  r.data = (ULONGLONG)(ULONG_PTR)pixels;
  r.length = (ULONG)bytes;
  if (gl_call(&r) >= 0) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)r.width;
    bi.bmiHeader.biHeight = -(LONG)r.height;      /* top row first */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, r.width, r.height, 0, 0, 0, r.height, pixels, &bi, DIB_RGB_COLORS);
  }
  HeapFree(GetProcessHeap(), 0, pixels);
}

static BOOL present(HDC dc, ULONG op) {
  gl_stream_flush();
  const ULONGLONG drawable = dc_drawable(dc);
  if (!drawable) return FALSE;
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = op;
  r.window = drawable;
  drawable_size(dc, drawable, &r.width, &r.height);
  if (gl_call(&r) < 0) return FALSE;
  if (r.result == 0) blit_front(dc, drawable);      /* (a bitmap, or a window the kernel does not draw in) */
  return TRUE;
}

BOOL WINAPI wglSwapBuffers(HDC dc) { return present(dc, OP_SWAP); }
BOOL WINAPI wglSwapLayerBuffers(HDC dc, UINT planes) { return (planes & WGL_SWAP_MAIN_PLANE) ? wglSwapBuffers(dc) : TRUE; }

DWORD WINAPI wglSwapMultipleBuffers(UINT count, const WGLSWAP *swaps) {
  DWORD done = 0;
  for (UINT i = 0; i < count && i < 32; ++i)
    if (wglSwapLayerBuffers(swaps[i].hdc, swaps[i].uiFlags)) done |= 1u << i;
  return done;
}

/* Without a current context GL commands do nothing, and glGetError says so (as Windows' opengl32 does). */
GLAPI GLenum APIENTRY glGetError(void) {
  if (!thread_gl()->context) return 0x0502;      /* GL_INVALID_OPERATION */
  return tcos_glGetError();
}

GLAPI void APIENTRY glFlush(void) {
  tcos_glFlush();
  ThreadGl *t = thread_gl();
  if (t->context && t->dc) present(t->dc, OP_FLUSHED);
}
GLAPI void APIENTRY glFinish(void) {
  tcos_glFinish();
  ThreadGl *t = thread_gl();
  if (t->context && t->dc) present(t->dc, OP_FLUSHED);
}

static int swap_interval = 1;
static BOOL WINAPI wglSwapIntervalEXT(int interval) {
  if (interval < 0) {
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
  }
  swap_interval = interval;      /* (frames are shown as they are swapped: there is no wait for the display) */
  return TRUE;
}
static int WINAPI wglGetSwapIntervalEXT(void) { return swap_interval; }

/* ---- Pixel formats: gdi32's (the display's formats). */
int WINAPI wglChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) { return ChoosePixelFormat(dc, pfd); }
int WINAPI wglDescribePixelFormat(HDC dc, int format, UINT size, PIXELFORMATDESCRIPTOR *pfd) { return DescribePixelFormat(dc, format, size, pfd); }
int WINAPI wglGetPixelFormat(HDC dc) { return GetPixelFormat(dc); }
BOOL WINAPI wglSetPixelFormat(HDC dc, int format, const PIXELFORMATDESCRIPTOR *pfd) { return SetPixelFormat(dc, format, pfd); }

/* No overlay or underlay planes. */
BOOL WINAPI wglDescribeLayerPlane(HDC dc, int format, int plane, UINT size, LAYERPLANEDESCRIPTOR *lpd) { return FALSE; }
int WINAPI wglGetLayerPaletteEntries(HDC dc, int plane, int start, int count, COLORREF *colors) { return 0; }
int WINAPI wglSetLayerPaletteEntries(HDC dc, int plane, int start, int count, const COLORREF *colors) { return 0; }
BOOL WINAPI wglRealizeLayerPalette(HDC dc, int plane, BOOL realize) { return FALSE; }

/* ---- WGL_ARB_create_context, WGL_ARB_pixel_format, WGL_ARB_extensions_string ... */
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_LAYER_PLANE_ARB 0x2093
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x1
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x2
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x1
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x2
#define ERROR_INVALID_VERSION_ARB 0x2095
#define ERROR_INVALID_PROFILE_ARB 0x2096

static HGLRC WINAPI wglCreateContextAttribsARB(HDC dc, HGLRC share, const int *attribs) {
  ULONG major = 3, minor = 3, flags = 0;
  int profile = WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, ctx_flags = 0;
  for (const int *a = attribs; a && a[0]; a += 2) {
    switch (a[0]) {
      case WGL_CONTEXT_MAJOR_VERSION_ARB: major = (ULONG)a[1]; break;
      case WGL_CONTEXT_MINOR_VERSION_ARB: minor = (ULONG)a[1]; break;
      case WGL_CONTEXT_FLAGS_ARB: ctx_flags = a[1]; break;
      case WGL_CONTEXT_PROFILE_MASK_ARB: profile = a[1]; break;
      case WGL_CONTEXT_LAYER_PLANE_ARB: if (a[1] != 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; } break;
      default: break;
    }
  }
  /* What the kernel's GL offers: up to 3.3 (the shading language it compiles); a request for more is refused as Windows' drivers do. */
  if (major > 3 || (major == 3 && minor > 3) || major == 0) {
    SetLastError(ERROR_INVALID_VERSION_ARB);
    return NULL;
  }
  if (profile & ~(WGL_CONTEXT_CORE_PROFILE_BIT_ARB | WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB)) {
    SetLastError(ERROR_INVALID_PROFILE_ARB);
    return NULL;
  }
  const BOOL core = (major > 3 || (major == 3 && minor >= 2)) && (profile & WGL_CONTEXT_CORE_PROFILE_BIT_ARB) && !(profile & WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB);
  if (core) flags |= 1;
  if (ctx_flags & WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB) flags |= 2;
  if (ctx_flags & WGL_CONTEXT_DEBUG_BIT_ARB) flags |= 4;
  return (HGLRC)(ULONG_PTR)create_context(dc, (ULONG)(ULONG_PTR)share, major, minor, flags);
}

static const char g_wgl_extensions[] =
    "WGL_ARB_extensions_string WGL_ARB_create_context WGL_ARB_create_context_profile WGL_ARB_pixel_format WGL_ARB_make_current_read WGL_EXT_extensions_string WGL_EXT_swap_control";
static const char *WINAPI wglGetExtensionsStringARB(HDC dc) { return g_wgl_extensions; }
static const char *WINAPI wglGetExtensionsStringEXT(void) { return g_wgl_extensions; }

#define WGL_NUMBER_PIXEL_FORMATS_ARB 0x2000
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_DRAW_TO_BITMAP_ARB 0x2002
#define WGL_ACCELERATION_ARB 0x2003
#define WGL_SUPPORT_GDI_ARB 0x200F
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#define WGL_STEREO_ARB 0x2012
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_COLOR_BITS_ARB 0x2014
#define WGL_RED_BITS_ARB 0x2015
#define WGL_GREEN_BITS_ARB 0x2017
#define WGL_BLUE_BITS_ARB 0x2019
#define WGL_ALPHA_BITS_ARB 0x201B
#define WGL_ACCUM_BITS_ARB 0x201D
#define WGL_DEPTH_BITS_ARB 0x2022
#define WGL_STENCIL_BITS_ARB 0x2023
#define WGL_AUX_BUFFERS_ARB 0x2024
#define WGL_SWAP_METHOD_ARB 0x2007
#define WGL_SWAP_EXCHANGE_ARB 0x2028
#define WGL_SWAP_COPY_ARB 0x2029
#define WGL_SWAP_UNDEFINED_ARB 0x202A
#define WGL_SAMPLE_BUFFERS_ARB 0x2041
#define WGL_SAMPLES_ARB 0x2042
#define WGL_FULL_ACCELERATION_ARB 0x2027
#define WGL_TYPE_RGBA_ARB 0x202B

static int attribute_of(const PIXELFORMATDESCRIPTOR *p, int count, int attribute, int *out) {
  switch (attribute) {
    case WGL_NUMBER_PIXEL_FORMATS_ARB: *out = count; return 1;
    case WGL_DRAW_TO_WINDOW_ARB: *out = (p->dwFlags & PFD_DRAW_TO_WINDOW) != 0; return 1;
    case WGL_DRAW_TO_BITMAP_ARB: *out = (p->dwFlags & PFD_DRAW_TO_BITMAP) != 0; return 1;
    case WGL_ACCELERATION_ARB: *out = WGL_FULL_ACCELERATION_ARB; return 1;
    case WGL_SUPPORT_GDI_ARB: *out = (p->dwFlags & PFD_SUPPORT_GDI) != 0; return 1;
    case WGL_SUPPORT_OPENGL_ARB: *out = (p->dwFlags & PFD_SUPPORT_OPENGL) != 0; return 1;
    case WGL_DOUBLE_BUFFER_ARB: *out = (p->dwFlags & PFD_DOUBLEBUFFER) != 0; return 1;
    case WGL_STEREO_ARB: *out = 0; return 1;
    case WGL_PIXEL_TYPE_ARB: *out = WGL_TYPE_RGBA_ARB; return 1;
    case WGL_COLOR_BITS_ARB: *out = p->cColorBits; return 1;
    case WGL_RED_BITS_ARB: *out = p->cRedBits; return 1;
    case WGL_GREEN_BITS_ARB: *out = p->cGreenBits; return 1;
    case WGL_BLUE_BITS_ARB: *out = p->cBlueBits; return 1;
    case WGL_ALPHA_BITS_ARB: *out = p->cAlphaBits; return 1;
    case WGL_ACCUM_BITS_ARB: *out = p->cAccumBits; return 1;
    case WGL_DEPTH_BITS_ARB: *out = p->cDepthBits; return 1;
    case WGL_STENCIL_BITS_ARB: *out = p->cStencilBits; return 1;
    case WGL_AUX_BUFFERS_ARB: *out = 0; return 1;
    case WGL_SWAP_METHOD_ARB: *out = (p->dwFlags & PFD_DOUBLEBUFFER) ? WGL_SWAP_EXCHANGE_ARB : WGL_SWAP_UNDEFINED_ARB; return 1;      /* (a swap exchanges the buffers) */
    case WGL_SAMPLE_BUFFERS_ARB: case WGL_SAMPLES_ARB: *out = 0; return 1;
    default: *out = 0; return 0;
  }
}

static BOOL WINAPI wglGetPixelFormatAttribivARB(HDC dc, int format, int layer, UINT n, const int *attributes, int *values) {
  PIXELFORMATDESCRIPTOR p;
  memset(&p, 0, sizeof(p));
  const int count = DescribePixelFormat(dc, 1, sizeof(p), NULL);
  if (n && attributes[0] != WGL_NUMBER_PIXEL_FORMATS_ARB && !DescribePixelFormat(dc, format, sizeof(p), &p)) return FALSE;
  for (UINT i = 0; i < n; ++i) {
    if (attributes[i] != WGL_NUMBER_PIXEL_FORMATS_ARB && format > 0) DescribePixelFormat(dc, format, sizeof(p), &p);
    if (!attribute_of(&p, count, attributes[i], &values[i])) {
      SetLastError(ERROR_INVALID_PARAMETER);
      return FALSE;
    }
  }
  return TRUE;
}

static BOOL WINAPI wglGetPixelFormatAttribfvARB(HDC dc, int format, int layer, UINT n, const int *attributes, float *values) {
  for (UINT i = 0; i < n; ++i) {
    int v = 0;
    if (!wglGetPixelFormatAttribivARB(dc, format, layer, 1, &attributes[i], &v)) return FALSE;
    values[i] = (float)v;
  }
  return TRUE;
}

/* The formats that have every attribute asked for (exact for booleans, at least for sizes), best first: those with the smallest extra sizes. */
static BOOL WINAPI wglChoosePixelFormatARB(HDC dc, const int *int_attributes, const float *float_attributes, UINT max, int *formats, UINT *count) {
  PIXELFORMATDESCRIPTOR p;
  const int n = DescribePixelFormat(dc, 1, sizeof(p), NULL);
  int scores[64], found[64];
  UINT got = 0;
  for (int f = 1; f <= n && f <= 64; ++f) {
    if (!DescribePixelFormat(dc, f, sizeof(p), &p)) continue;
    int ok = 1, score = 0;
    for (const int *a = int_attributes; ok && a && a[0]; a += 2) {
      int v = 0;
      if (!attribute_of(&p, n, a[0], &v)) continue;
      switch (a[0]) {
        case WGL_COLOR_BITS_ARB: case WGL_RED_BITS_ARB: case WGL_GREEN_BITS_ARB: case WGL_BLUE_BITS_ARB: case WGL_ALPHA_BITS_ARB: case WGL_DEPTH_BITS_ARB: case WGL_STENCIL_BITS_ARB: case WGL_ACCUM_BITS_ARB:
        case WGL_AUX_BUFFERS_ARB: case WGL_SAMPLES_ARB: case WGL_SAMPLE_BUFFERS_ARB:
          if (a[0] == WGL_ACCUM_BITS_ARB) break;      /* (accumulation is always there) */
          if (v < a[1]) ok = 0; else score += v - a[1];
          break;
        default: if (v != a[1]) ok = 0; break;
      }
    }
    if (!ok) continue;
    UINT at = got++;
    while (at > 0 && scores[at - 1] > score) { scores[at] = scores[at - 1]; found[at] = found[at - 1]; --at; }
    scores[at] = score;
    found[at] = f;
  }
  for (UINT i = 0; i < got && i < max; ++i) formats[i] = found[i];
  *count = got < max ? got : max;
  return TRUE;
}

/* ---- wglGetProcAddress: every command of the kernel's GL and the WGL extensions, by name. */
typedef struct { const char *name; PROC proc; } ProcEntry;
static const ProcEntry g_procs[] = {
#define GL_PROC(name) {#name, (PROC)(void *)name},
#include "../../OpenGl/generated/GlProcs.inc"
#undef GL_PROC
};
static const ProcEntry g_wgl_procs[] = {
    {"wglChoosePixelFormatARB", (PROC)(void *)wglChoosePixelFormatARB}, {"wglCreateContextAttribsARB", (PROC)(void *)wglCreateContextAttribsARB},
    {"wglGetCurrentReadDCARB", (PROC)(void *)wglGetCurrentReadDCARB}, {"wglGetExtensionsStringARB", (PROC)(void *)wglGetExtensionsStringARB}, {"wglGetExtensionsStringEXT", (PROC)(void *)wglGetExtensionsStringEXT},
    {"wglGetPixelFormatAttribfvARB", (PROC)(void *)wglGetPixelFormatAttribfvARB}, {"wglGetPixelFormatAttribivARB", (PROC)(void *)wglGetPixelFormatAttribivARB},
    {"wglGetSwapIntervalEXT", (PROC)(void *)wglGetSwapIntervalEXT}, {"wglMakeContextCurrentARB", (PROC)(void *)wglMakeContextCurrentARB}, {"wglSwapIntervalEXT", (PROC)(void *)wglSwapIntervalEXT},
};

static int name_compare(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (unsigned char)*a - (unsigned char)*b;
}

PROC WINAPI wglGetProcAddress(LPCSTR name) {
  if (!name || (ULONG_PTR)name < 0x10000) return NULL;
  size_t lo = 0, hi = sizeof(g_procs) / sizeof(g_procs[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = name_compare(g_procs[mid].name, name);
    if (c == 0) return g_procs[mid].proc;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  for (size_t i = 0; i < sizeof(g_wgl_procs) / sizeof(g_wgl_procs[0]); ++i)
    if (name_compare(g_wgl_procs[i].name, name) == 0) return g_wgl_procs[i].proc;
  return NULL;
}
PROC WINAPI wglGetDefaultProcAddress(LPCSTR name) { return wglGetProcAddress(name); }

/* ---- Font display lists: one list per character, its glyph as a bitmap that moves the raster position by the advance. The glyph comes from GDI's outline service
 * (GGO_BITMAP); when that has nothing, the character is drawn with TextOut into a DIB section and its advance measured with GetTextExtentPoint32. */
typedef struct GlyphBits {
  UINT w, h, pitch;      /* rows of `pitch` bytes, bottom row first (GL's order), one bit a pixel, most significant first */
  int origin_x, origin_y;      /* the bitmap's lower left corner relative to the pen */
  int advance_x, advance_y;
  unsigned char *bits;      /* HeapAlloc'd (null when the glyph is empty) */
} GlyphBits;

static BOOL glyph_by_outline(HDC dc, UINT ch, BOOL wide, GlyphBits *g) {
  static const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
  GLYPHMETRICS gm;
  memset(&gm, 0, sizeof(gm));
  DWORD size = wide ? GetGlyphOutlineW(dc, ch, GGO_BITMAP, &gm, 0, NULL, &identity) : GetGlyphOutlineA(dc, ch, GGO_BITMAP, &gm, 0, NULL, &identity);
  if (size == GDI_ERROR || (size == 0 && gm.gmCellIncX == 0)) return FALSE;
  g->advance_x = gm.gmCellIncX;
  g->advance_y = gm.gmCellIncY;
  g->w = gm.gmBlackBoxX;
  g->h = gm.gmBlackBoxY;
  g->pitch = ((g->w + 31) / 32) * 4;
  g->origin_x = -gm.gmptGlyphOrigin.x;
  g->origin_y = (int)g->h - gm.gmptGlyphOrigin.y;
  g->bits = NULL;
  if (!size || !g->h || (SIZE_T)g->pitch * g->h > size) { g->w = g->h = 0; return TRUE; }
  unsigned char *top_first = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size);
  g->bits = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)g->pitch * g->h);
  if (!top_first || !g->bits) { HeapFree(GetProcessHeap(), 0, top_first); HeapFree(GetProcessHeap(), 0, g->bits); return FALSE; }
  if (wide) GetGlyphOutlineW(dc, ch, GGO_BITMAP, &gm, size, top_first, &identity);
  else GetGlyphOutlineA(dc, ch, GGO_BITMAP, &gm, size, top_first, &identity);
  for (UINT y = 0; y < g->h; ++y) memcpy(g->bits + (SIZE_T)y * g->pitch, top_first + (SIZE_T)(g->h - 1 - y) * g->pitch, g->pitch);
  HeapFree(GetProcessHeap(), 0, top_first);
  return TRUE;
}

static BOOL glyph_by_drawing(HDC dc, UINT ch, BOOL wide, GlyphBits *g) {
  SIZE extent = {0, 0};
  WCHAR wc = (WCHAR)ch;
  char ac = (char)ch;
  if (!(wide ? GetTextExtentPoint32W(dc, &wc, 1, &extent) : GetTextExtentPoint32A(dc, &ac, 1, &extent)) || extent.cx <= 0 || extent.cy <= 0) return FALSE;
  TEXTMETRICA tm;
  memset(&tm, 0, sizeof(tm));
  GetTextMetricsA(dc, &tm);
  g->advance_x = extent.cx;
  g->advance_y = 0;
  g->w = (UINT)extent.cx;
  g->h = (UINT)extent.cy;
  g->pitch = ((g->w + 31) / 32) * 4;
  g->origin_x = 0;
  g->origin_y = tm.tmDescent;      /* the bitmap's bottom is the cell's: the pen sits on the baseline */
  g->bits = NULL;
  BITMAPINFO bi;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = (LONG)g->w;
  bi.bmiHeader.biHeight = (LONG)g->h;      /* bottom row first */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *pixels = NULL;
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP dib = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &pixels, NULL, 0) : NULL;
  if (!dib || !pixels) { if (dib) DeleteObject(dib); if (mem) DeleteDC(mem); return FALSE; }
  HGDIOBJ old_bitmap = SelectObject(mem, dib);
  HGDIOBJ old_font = SelectObject(mem, GetCurrentObject(dc, OBJ_FONT));
  memset(pixels, 0, (SIZE_T)g->w * g->h * 4);
  SetBkMode(mem, TRANSPARENT);
  SetTextColor(mem, RGB(255, 255, 255));
  if (wide) TextOutW(mem, 0, 0, &wc, 1); else TextOutA(mem, 0, 0, &ac, 1);
  GdiFlush();
  g->bits = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)g->pitch * g->h);
  if (g->bits) {
    const DWORD *p = (const DWORD *)pixels;
    for (UINT y = 0; y < g->h; ++y)
      for (UINT x = 0; x < g->w; ++x)
        if ((p[(SIZE_T)y * g->w + x] & 0xFF00) >= 0x8000) g->bits[(SIZE_T)y * g->pitch + x / 8] |= (unsigned char)(0x80u >> (x % 8));
  }
  SelectObject(mem, old_font);
  SelectObject(mem, old_bitmap);
  DeleteObject(dib);
  DeleteDC(mem);
  return g->bits != NULL;
}

static BOOL use_font_bitmaps(HDC dc, DWORD first, DWORD count, DWORD base, BOOL wide) {
  for (DWORD i = 0; i < count; ++i) {
    GlyphBits g;
    memset(&g, 0, sizeof(g));
    const UINT ch = first + i;
    if (!glyph_by_outline(dc, ch, wide, &g) && !glyph_by_drawing(dc, ch, wide, &g)) memset(&g, 0, sizeof(g));
    glNewList(base + i, GL_COMPILE);
    if (g.bits && g.w && g.h) {
      GLint saved = 4;
      glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);      /* (rows padded to 4 bytes) */
      glBitmap((GLsizei)g.w, (GLsizei)g.h, (GLfloat)g.origin_x, (GLfloat)g.origin_y, (GLfloat)g.advance_x, (GLfloat)g.advance_y, g.bits);
      glPixelStorei(GL_UNPACK_ALIGNMENT, saved);
    } else {
      glBitmap(0, 0, 0, 0, (GLfloat)g.advance_x, (GLfloat)g.advance_y, NULL);
    }
    glEndList();
    if (g.bits) HeapFree(GetProcessHeap(), 0, g.bits);
  }
  return TRUE;
}
BOOL WINAPI wglUseFontBitmapsA(HDC dc, DWORD first, DWORD count, DWORD base) { return use_font_bitmaps(dc, first, count, base, FALSE); }
BOOL WINAPI wglUseFontBitmapsW(HDC dc, DWORD first, DWORD count, DWORD base) { return use_font_bitmaps(dc, first, count, base, TRUE); }
/* (outlines as polygons are not made: the programs that ask get FALSE, as from a driver without them) */
BOOL WINAPI wglUseFontOutlinesA(HDC dc, DWORD first, DWORD count, DWORD base, FLOAT deviation, FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT gm) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
BOOL WINAPI wglUseFontOutlinesW(HDC dc, DWORD first, DWORD count, DWORD base, FLOAT deviation, FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT gm) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_tls = TlsAlloc();
  } else if (reason == DLL_THREAD_DETACH || reason == DLL_PROCESS_DETACH) {
    if (g_tls != TLS_OUT_OF_INDEXES) {
      ThreadGl *t = (ThreadGl *)TlsGetValue(g_tls);
      if (t) {
        if (t->context) make_current(NULL, NULL, NULL);
        TlsSetValue(g_tls, NULL);
        HeapFree(GetProcessHeap(), 0, t);
      }
    }
  }
  return TRUE;
}
