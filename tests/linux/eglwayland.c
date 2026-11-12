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

/* An OpenGL ES program as GL toolkits write one: glibc, libwayland-client, libwayland-egl, an xdg-shell toplevel, and EGL with OpenGL ES 2.0 through
 * libEGL.so.1 / libGLESv2.so.2 (src/Linux/libegl over the kernel's OpenGL). It checks the client and display strings, config choice and its order, context
 * versions (ES 2.0 and 3.0 yes, 1.x and 3.1 no), a window surface on a wl_egl_window (one per window), a GLSL ES 1.00 program drawn and read back, swaps with
 * a swap interval of 1 and of 0 and with damage, a resize through wl_egl_window_resize, a fence, a pbuffer with a shared ES 3.0 context and a #version 300 es
 * program, a context with no surface, a desktop OpenGL 3.3 core context, and eglGetProcAddress. The last frame is purple (0xFF8040C0).
 * Prints "egl-wayland: N checks, M failed". tests/linux_egl_wayland_emulated.sh runs it; with the argument "hold" the window stays four seconds after the
 * last frame (for a screenshot in QEMU). */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#define WL_EGL_PLATFORM 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* OpenGL ES 2.0: what this program uses (the development machine may have no GLES2 headers) */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte, GLboolean;
typedef float GLfloat;
typedef char GLchar;
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_NO_ERROR 0
#define GL_VIEWPORT 0x0BA2
#define GL_MAJOR_VERSION 0x821B
extern void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void glClear(GLbitfield);
extern void glViewport(GLint, GLint, GLsizei, GLsizei);
extern void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
extern const GLubyte *glGetString(GLenum);
extern GLenum glGetError(void);
extern void glGetIntegerv(GLenum, GLint *);
extern GLuint glCreateShader(GLenum);
extern void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
extern void glCompileShader(GLuint);
extern void glGetShaderiv(GLuint, GLenum, GLint *);
extern GLuint glCreateProgram(void);
extern void glAttachShader(GLuint, GLuint);
extern void glBindAttribLocation(GLuint, GLuint, const GLchar *);
extern void glLinkProgram(GLuint);
extern void glGetProgramiv(GLuint, GLenum, GLint *);
extern void glUseProgram(GLuint);
extern GLint glGetUniformLocation(GLuint, const GLchar *);
extern void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
extern void glEnableVertexAttribArray(GLuint);
extern void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
extern void glDrawArrays(GLenum, GLint, GLsizei);
extern void glDeleteProgram(GLuint);
extern void glDeleteShader(GLuint);
extern void glFinish(void);

static int checks, failed;
#define CHECK(c)                                                    \
  do {                                                              \
    ++checks;                                                       \
    if (!(c)) {                                                     \
      ++failed;                                                     \
      printf("FAILED line %d: %s\n", __LINE__, #c);                 \
    }                                                               \
  } while (0)

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm;
static struct zxdg_decoration_manager_v1 *decorations;
static int configured, frames_done;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  (void)data;
  if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
  else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, "zxdg_decoration_manager_v1")) decorations = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data, (void)r, (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void ping(void *data, struct xdg_wm_base *b, uint32_t serial) { (void)data, xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};
static void surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(s, serial);
  configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {surface_configure};
static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
  (void)data, (void)t, (void)w, (void)h, (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) { (void)data, (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};
static void own_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  (void)data, (void)time;
  ++frames_done;
  wl_callback_destroy(cb);
}
static const struct wl_callback_listener own_frame_listener = {own_frame_done};

static int has_word(const char *list, const char *word) {
  if (!list) return 0;
  const size_t n = strlen(word);
  for (const char *p = list; (p = strstr(p, word)) != NULL; p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
  return 0;
}

static GLuint program_of(const char *vs, const char *fs) {
  const GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(v, 1, &vs, NULL);
  glShaderSource(f, 1, &fs, NULL);
  glCompileShader(v);
  glCompileShader(f);
  GLint ok_v = 0, ok_f = 0, linked = 0;
  glGetShaderiv(v, GL_COMPILE_STATUS, &ok_v);
  glGetShaderiv(f, GL_COMPILE_STATUS, &ok_f);
  const GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "pos");
  glLinkProgram(p);
  glGetProgramiv(p, GL_LINK_STATUS, &linked);
  glDeleteShader(v);
  glDeleteShader(f);
  if (!ok_v || !ok_f || !linked) {
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static unsigned pixel(int x, int y) {
  GLubyte px[4] = {0, 0, 0, 0};
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  return (unsigned)px[0] << 24 | (unsigned)px[1] << 16 | (unsigned)px[2] << 8 | px[3];
}
static int near_rgba(unsigned got, unsigned want) {
  for (int s = 0; s < 32; s += 8) {
    const int a = (int)(got >> s & 0xFF), b = (int)(want >> s & 0xFF);
    if (a - b > 2 || b - a > 2) return 0;
  }
  return 1;
}

/* A second thread: the context current on the main thread is refused to it; one of its own draws on a pbuffer of its own at the same time. */
typedef struct ThreadTest {
  EGLDisplay egl;
  EGLConfig config;
  EGLContext busy;
  int refused, drew, released;
} ThreadTest;
static void *thread_main(void *arg) {
  ThreadTest *t = (ThreadTest *)arg;
  t->refused = !eglMakeCurrent(t->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, t->busy) && eglGetError() == EGL_BAD_ACCESS;
  const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLContext own = eglCreateContext(t->egl, t->config, EGL_NO_CONTEXT, attribs);
  EGLSurface pbuffer = eglCreatePbufferSurface(t->egl, t->config, size);
  if (own && pbuffer && eglMakeCurrent(t->egl, pbuffer, pbuffer, own)) {
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    t->drew = near_rgba(pixel(8, 8), 0xFF00FFFF);
  }
  t->released = eglMakeCurrent(t->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) && eglDestroySurface(t->egl, pbuffer) && eglDestroyContext(t->egl, own);
  eglReleaseThread();
  return NULL;
}

int main(int argc, char **argv) {
  struct wl_display *display = wl_display_connect(NULL);
  CHECK(display != NULL);
  if (!display) return 1;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  CHECK(compositor && wm);
  if (!compositor || !wm) return 1;
  xdg_wm_base_add_listener(wm, &wm_listener, NULL);

  /* client extensions and the display */
  const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  CHECK(has_word(client, "EGL_KHR_platform_wayland") && has_word(client, "EGL_EXT_platform_base") && has_word(client, "EGL_MESA_platform_surfaceless"));
  EGLDisplay egl = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
  CHECK(egl != EGL_NO_DISPLAY);
  CHECK(eglGetDisplay((EGLNativeDisplayType)display) == egl);      /* (the same native display: the same EGLDisplay) */
  CHECK(eglQueryString(egl, EGL_VENDOR) == NULL && eglGetError() == EGL_NOT_INITIALIZED);
  EGLint major = 0, minor = 0;
  CHECK(eglInitialize(egl, &major, &minor) && major == 1 && minor == 5);
  CHECK(has_word(eglQueryString(egl, EGL_CLIENT_APIS), "OpenGL_ES") && has_word(eglQueryString(egl, EGL_CLIENT_APIS), "OpenGL"));
  const char *ext = eglQueryString(egl, EGL_EXTENSIONS);
  CHECK(has_word(ext, "EGL_KHR_surfaceless_context") && has_word(ext, "EGL_KHR_swap_buffers_with_damage") && has_word(ext, "EGL_KHR_create_context"));

  /* configs: what was asked for, in EGL's order */
  const EGLint want[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig configs[16];
  EGLint count = 0;
  CHECK(eglChooseConfig(egl, want, configs, 16, &count) && count >= 1);
  EGLConfig config = configs[0];
  EGLint alpha = -1, depth = -1, visual = 0;
  eglGetConfigAttrib(egl, config, EGL_ALPHA_SIZE, &alpha);
  eglGetConfigAttrib(egl, config, EGL_DEPTH_SIZE, &depth);
  eglGetConfigAttrib(egl, config, EGL_NATIVE_VISUAL_ID, &visual);
  CHECK(alpha == 8 && depth >= 24 && visual == 0x34325241);      /* 'AR24' */
  const EGLint plain[] = {EGL_NONE};
  EGLConfig first = NULL;
  CHECK(eglChooseConfig(egl, plain, &first, 1, &count) && count == 1);
  EGLint first_alpha = -1, first_depth = -1;
  eglGetConfigAttrib(egl, first, EGL_ALPHA_SIZE, &first_alpha);
  eglGetConfigAttrib(egl, first, EGL_DEPTH_SIZE, &first_depth);
  CHECK(first_alpha == 0 && first_depth == 0);      /* (nothing asked for: the smallest buffer first) */
  const EGLint bad[] = {0x7777, 1, EGL_NONE};
  CHECK(!eglChooseConfig(egl, bad, configs, 16, &count) && eglGetError() == EGL_BAD_ATTRIBUTE);
  const EGLint too_deep[] = {EGL_DEPTH_SIZE, 32, EGL_NONE};
  CHECK(eglChooseConfig(egl, too_deep, configs, 16, &count) && count == 0);
  const EGLint es1_configs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_NONE};
  CHECK(eglChooseConfig(egl, es1_configs, configs, 16, &count) && count == 0);      /* (no OpenGL ES 1.x) */

  /* contexts */
  CHECK(eglBindAPI(EGL_OPENGL_ES_API));
  const EGLint es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE}, es1[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  const EGLint es31[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE}, es3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  EGLContext context = eglCreateContext(egl, config, EGL_NO_CONTEXT, es2);
  CHECK(context != EGL_NO_CONTEXT);
  CHECK(eglCreateContext(egl, config, EGL_NO_CONTEXT, es1) == EGL_NO_CONTEXT && eglGetError() == EGL_BAD_MATCH);
  CHECK(eglCreateContext(egl, config, EGL_NO_CONTEXT, es31) == EGL_NO_CONTEXT && eglGetError() == EGL_BAD_MATCH);

  /* the window */
  struct wl_surface *surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surface);
  xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
  struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
  xdg_toplevel_set_title(top, "Linux EGL window");
  struct zxdg_toplevel_decoration_v1 *decoration = NULL;
  if (decorations) {      /* (framed by the desktop, as a window of its own) */
    decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(decorations, top);
    zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }
  wl_surface_commit(surface);
  while (!configured && wl_display_dispatch(display) >= 0) {
  }
  CHECK(configured);
  struct wl_egl_window *window = wl_egl_window_create(surface, 320, 200);
  CHECK(window != NULL);
  EGLSurface ws = eglCreateWindowSurface(egl, config, (EGLNativeWindowType)window, NULL);
  CHECK(ws != EGL_NO_SURFACE);
  CHECK(eglCreateWindowSurface(egl, config, (EGLNativeWindowType)window, NULL) == EGL_NO_SURFACE && eglGetError() == EGL_BAD_ALLOC);      /* (one per window) */
  CHECK(eglMakeCurrent(egl, ws, ws, context) && eglGetCurrentContext() == context && eglGetCurrentSurface(EGL_DRAW) == ws);
  const char *version = (const char *)glGetString(GL_VERSION);
  CHECK(version && strncmp(version, "OpenGL ES 3.0", 13) == 0);
  const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
  CHECK(glsl && strncmp(glsl, "OpenGL ES GLSL ES", 17) == 0);
  CHECK(has_word((const char *)glGetString(GL_EXTENSIONS), "GL_OES_element_index_uint"));
  EGLint w = 0, h = 0;
  CHECK(eglQuerySurface(egl, ws, EGL_WIDTH, &w) && eglQuerySurface(egl, ws, EGL_HEIGHT, &h) && w == 320 && h == 200);
  GLint viewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, viewport);
  CHECK(viewport[2] == 320 && viewport[3] == 200);

  /* a GLSL ES 1.00 program: a triangle over the lower left half */
  const GLuint program = program_of("attribute vec2 pos;\nvoid main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n",
                                    "precision mediump float;\nuniform vec4 color;\nvoid main(){ gl_FragColor = color; }\n");
  CHECK(program != 0);
  glUseProgram(program);
  glUniform4f(glGetUniformLocation(program, "color"), 1, 1, 0, 1);
  const GLfloat tri[] = {-1, -1, 1, -1, -1, 1};
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, tri);
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  CHECK(near_rgba(pixel(20, 20), 0xFFFF00FF));      /* yellow under the triangle */
  CHECK(near_rgba(pixel(300, 190), 0x0000FFFF));    /* blue beside it */
  CHECK(glGetError() == GL_NO_ERROR);

  /* another thread, while this one has the context current */
  ThreadTest tt = {egl, config, context, 0, 0, 0};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, thread_main, &tt) == 0 && pthread_join(thread, NULL) == 0);
  CHECK(tt.refused && tt.drew && tt.released);
  CHECK(eglGetCurrentContext() == context && near_rgba(pixel(20, 20), 0xFFFF00FF));      /* (this thread's context and picture are as they were) */

  /* what libepoxy does: the GL library opened by name, its commands looked up in it */
  void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
  CHECK(gles != NULL && dlsym(gles, "glDrawArrays") != NULL && dlsym(gles, "glClear") != NULL);
  void *gl = dlopen("libGL.so.1", RTLD_NOW);
  CHECK(gl != NULL && dlsym(gl, "glBegin") != NULL);

  /* swaps with the swap interval of 1: each waits for the frame before it to be shown */
  CHECK(eglSwapInterval(egl, 1));
  int swapped = 1;
  for (int i = 0; i < 4; ++i) {
    glClearColor(0, (float)i / 4, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    swapped &= eglSwapBuffers(egl, ws) == EGL_TRUE;
  }
  CHECK(swapped);
  /* the program's own queue still works beside the library's */
  struct wl_callback *own = wl_surface_frame(surface);
  wl_callback_add_listener(own, &own_frame_listener, NULL);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(eglSwapBuffers(egl, ws));
  while (!frames_done && wl_display_dispatch(display) >= 0) {
  }
  CHECK(frames_done == 1);
  CHECK(wl_display_roundtrip(display) >= 0);

  /* a resize: the next frame has the new size */
  wl_egl_window_resize(window, 256, 160, 0, 0);
  CHECK(eglQuerySurface(egl, ws, EGL_WIDTH, &w) && eglQuerySurface(egl, ws, EGL_HEIGHT, &h) && w == 256 && h == 160);
  glViewport(0, 0, 256, 160);
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(near_rgba(pixel(250, 150), 0xFF0000FF));
  int aw = 0, ah = 0;
  CHECK(eglSwapBuffers(egl, ws));
  wl_egl_window_get_attached_size(window, &aw, &ah);
  CHECK(aw == 256 && ah == 160);
  /* damage, and no waiting (swap interval 0) */
  CHECK(eglSwapInterval(egl, 0));
  const EGLint damage[] = {0, 0, 64, 32, 100, 100, 20, 20};
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC with_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)eglGetProcAddress("eglSwapBuffersWithDamageKHR");
  CHECK(with_damage != NULL);
  int quick = 1;
  for (int i = 0; i < 6 && with_damage; ++i) {
    glClearColor(0, 0, (float)i / 6, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    quick &= with_damage(egl, ws, damage, 2) == EGL_TRUE;
  }
  CHECK(quick);

  /* a fence: every command before it has run */
  PFNEGLCREATESYNCKHRPROC create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
  PFNEGLCLIENTWAITSYNCKHRPROC wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
  PFNEGLDESTROYSYNCKHRPROC destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
  CHECK(create_sync && wait_sync && destroy_sync);
  if (create_sync && wait_sync && destroy_sync) {
    EGLSyncKHR sync = create_sync(egl, EGL_SYNC_FENCE_KHR, NULL);
    CHECK(sync != EGL_NO_SYNC_KHR);
    CHECK(wait_sync(egl, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR) == EGL_CONDITION_SATISFIED_KHR);
    CHECK(destroy_sync(egl, sync) && !destroy_sync(egl, sync) && eglGetError() == EGL_BAD_PARAMETER);
  }

  /* a pbuffer with an ES 3.0 context that shares the first's objects */
  const EGLint pbuffer_attribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
  const EGLint pbuffer_want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  EGLConfig pconfig = NULL;
  CHECK(eglChooseConfig(egl, pbuffer_want, &pconfig, 1, &count) && count == 1);
  EGLSurface pbuffer = eglCreatePbufferSurface(egl, pconfig, pbuffer_attribs);
  CHECK(pbuffer != EGL_NO_SURFACE);
  EGLContext shared = eglCreateContext(egl, pconfig, context, es3);
  CHECK(shared != EGL_NO_CONTEXT);
  CHECK(eglMakeCurrent(egl, pbuffer, pbuffer, shared));
  CHECK(eglQuerySurface(egl, pbuffer, EGL_WIDTH, &w) && w == 64);
  GLint gl_major = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &gl_major);
  CHECK(gl_major == 3);
  const GLuint p3 = program_of("#version 300 es\nin vec2 pos;\nvoid main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n",
                               "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }\n");
  CHECK(p3 != 0);
  glUseProgram(p3);
  const GLfloat whole[] = {-1, -1, 3, -1, -1, 3};
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, whole);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  CHECK(near_rgba(pixel(60, 40), 0x00FF00FF));
  CHECK(eglSwapBuffers(egl, pbuffer));      /* (a pbuffer's swap does nothing) */
  /* no surface at all, and the window surface is not current any more */
  CHECK(eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, shared));      /* EGL_KHR_surfaceless_context */
  CHECK(eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE);
  CHECK(!eglSwapBuffers(egl, ws) && eglGetError() == EGL_BAD_SURFACE);      /* (not current) */
  CHECK(eglDestroySurface(egl, pbuffer) && !eglDestroySurface(egl, pbuffer) && eglGetError() == EGL_BAD_SURFACE);

  /* desktop OpenGL 3.3, core */
  CHECK(eglBindAPI(EGL_OPENGL_API) && eglQueryAPI() == EGL_OPENGL_API);
  const EGLint core[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  const EGLint gl46[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_NONE};
  EGLContext desktop = eglCreateContext(egl, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, core);
  CHECK(desktop != EGL_NO_CONTEXT);
  CHECK(eglCreateContext(egl, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, gl46) == EGL_NO_CONTEXT && eglGetError() == EGL_BAD_MATCH);
  CHECK(eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, desktop));
  version = (const char *)glGetString(GL_VERSION);
  CHECK(version && strncmp(version, "3.3", 3) == 0);
  CHECK(eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) && eglGetCurrentContext() == EGL_NO_CONTEXT);
  CHECK(eglDestroyContext(egl, desktop) && eglDestroyContext(egl, shared));
  CHECK(eglBindAPI(EGL_OPENGL_ES_API));

  /* the last frame: purple */
  CHECK(eglMakeCurrent(egl, ws, ws, context));
  CHECK(eglSwapInterval(egl, 1));
  glClearColor(128.0f / 255, 64.0f / 255, 192.0f / 255, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(near_rgba(pixel(10, 10), 0x8040C0FF));
  CHECK(eglSwapBuffers(egl, ws));
  /* the desktop draws it: the frame callback of a frame of the program's own */
  frames_done = 0;
  own = wl_surface_frame(surface);
  wl_callback_add_listener(own, &own_frame_listener, NULL);
  wl_surface_commit(surface);
  while (!frames_done && wl_display_dispatch(display) >= 0) {
  }
  CHECK(frames_done == 1);
  if (argc > 1 && !strcmp(argv[1], "hold")) {      /* (QEMU: the window stays a few seconds for a screenshot) */
    struct timespec pause = {4, 0};
    nanosleep(&pause, NULL);
  }

  /* eglGetProcAddress knows GL and EGL, not other names */
  CHECK(eglGetProcAddress("glDrawArrays") != NULL && eglGetProcAddress("eglSwapBuffers") != NULL && eglGetProcAddress("glNoSuchCommand") == NULL);

  glUseProgram(0);
  glDeleteProgram(program);
  CHECK(eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
  CHECK(eglDestroySurface(egl, ws) && eglDestroyContext(egl, context));
  wl_egl_window_destroy(window);
  CHECK(eglTerminate(egl) && eglReleaseThread());
  CHECK(wl_display_roundtrip(display) >= 0 && wl_display_get_error(display) == 0);
  if (decoration) zxdg_toplevel_decoration_v1_destroy(decoration);
  xdg_toplevel_destroy(top);
  xdg_surface_destroy(xs);
  wl_surface_destroy(surface);
  wl_display_disconnect(display);
  printf("egl-wayland: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
