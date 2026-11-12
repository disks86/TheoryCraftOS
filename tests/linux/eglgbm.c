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

/* GBM and EGL images as KMS-style programs and video players use them: glibc, libgbm.so.1 (src/Linux/libgbm over /dev/dxg) and libEGL.so.1 / libGLESv2.so.2
 * (src/Linux/libegl). It checks a GBM device on /dev/dxg, buffer objects (formats, strides, modifiers, mapping for writing and reading, gbm_bo_write, user
 * data, dmabuf descriptors and importing them back with their checks), an EGL display of the GBM platform, a window surface on a gbm_surface whose swaps
 * reach its buffers (the frame's colour read through gbm_bo_map; a swap with every buffer held fails and works again once one is released), and
 * EGL_EXT_image_dma_buf_import: a dmabuf written by the CPU, and one the GPU drew into, made EGL images and then textures (read back through a framebuffer
 * object), with the attribute checks. Prints "egl-gbm: N checks, M failed". tests/linux_egl_wayland_emulated.sh runs it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte;
typedef float GLfloat;
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_EXTENSIONS 0x1F03
#define GL_NO_ERROR 0
#define GL_INVALID_OPERATION 0x0502
#define DRM_FORMAT_MOD_LINEAR 0ull
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, void *image);      /* (GL_OES_EGL_image; the development machine may have no GLES2 headers) */
extern void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void glClear(GLbitfield);
extern void glViewport(GLint, GLint, GLsizei, GLsizei);
extern void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
extern const GLubyte *glGetString(GLenum);
extern GLenum glGetError(void);
extern void glGenTextures(GLsizei, GLuint *);
extern void glBindTexture(GLenum, GLuint);
extern void glGenFramebuffers(GLsizei, GLuint *);
extern void glBindFramebuffer(GLenum, GLuint);
extern void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum glCheckFramebufferStatus(GLenum);
extern void glFinish(void);

static int checks, failures;
#define CHECK(c)                                                    \
  do {                                                              \
    ++checks;                                                       \
    if (!(c)) {                                                     \
      ++failures;                                                   \
      printf("FAILED line %d: %s\n", __LINE__, #c);                 \
    }                                                               \
  } while (0)

static int has_word(const char *list, const char *word) {
  if (!list) return 0;
  const size_t n = strlen(word);
  for (const char *p = list; (p = strstr(p, word)) != NULL; p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
  return 0;
}

static int user_data_freed;
static void free_user_data(struct gbm_bo *bo, void *data) { user_data_freed += data == (void *)&user_data_freed; }

/* The pixel at (x, y) of a buffer object, read through gbm_bo_map. */
static uint32_t bo_pixel(struct gbm_bo *bo, uint32_t x, uint32_t y) {
  uint32_t stride = 0;
  void *data = NULL;
  uint8_t *p = (uint8_t *)gbm_bo_map(bo, 0, 0, gbm_bo_get_width(bo), gbm_bo_get_height(bo), GBM_BO_TRANSFER_READ, &stride, &data);
  if (!p) return 0xDEADBEEF;
  uint32_t v;
  memcpy(&v, p + (size_t)y * stride + (size_t)x * 4, 4);
  gbm_bo_unmap(bo, data);
  return v;
}

/* The texture of an EGL image of `fd`, read back RGBA through a framebuffer object (the current context): the pixel at (x, y). */
static uint32_t image_pixel(EGLDisplay egl, PFNEGLCREATEIMAGEKHRPROC create, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target, int fd, uint32_t w, uint32_t h,
                            uint32_t stride, uint32_t format, uint32_t x, uint32_t y) {
  const EGLint attribs[] = {EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format, EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
  EGLImageKHR image = create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
  if (image == EGL_NO_IMAGE_KHR) return 0xDEADBEEF;
  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  target(GL_TEXTURE_2D, image);
  eglDestroyImage(egl, image);      /* (the texture keeps its pixels) */
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  uint8_t px[4] = {0, 0, 0, 0};
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) glReadPixels((GLint)x, (GLint)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return (uint32_t)px[0] << 24 | (uint32_t)px[1] << 16 | (uint32_t)px[2] << 8 | px[3];      /* 0xRRGGBBAA */
}

int main(void) {
  /* ---- the device */
  const int fd = open("/dev/dxg", O_RDWR | O_CLOEXEC);
  CHECK(fd >= 0);
  CHECK(gbm_create_device(-1) == NULL);
  const int not_dxg = memfd_create("not-dxg", MFD_CLOEXEC);
  CHECK(not_dxg >= 0 && gbm_create_device(not_dxg) == NULL);      /* (only /dev/dxg) */
  struct gbm_device *gbm = gbm_create_device(fd);
  CHECK(gbm != NULL);
  if (!gbm) {
    printf("egl-gbm: %d checks, %d failed\n", checks, failures);
    return 1;
  }
  CHECK(gbm_device_get_fd(gbm) == fd);
  CHECK(strcmp(gbm_device_get_backend_name(gbm), "tcos") == 0);
  CHECK(gbm_device_is_format_supported(gbm, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING));
  CHECK(gbm_device_is_format_supported(gbm, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING));
  CHECK(!gbm_device_is_format_supported(gbm, GBM_FORMAT_NV12, GBM_BO_USE_RENDERING));
  CHECK(!gbm_device_is_format_supported(gbm, GBM_FORMAT_ARGB8888, GBM_BO_USE_CURSOR));
  CHECK(gbm_device_get_format_modifier_plane_count(gbm, GBM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR) == 1);
  struct gbm_format_name_desc name;
  CHECK(strcmp(gbm_format_get_name(GBM_FORMAT_XRGB8888, &name), "XR24") == 0);

  /* ---- buffer objects */
  CHECK(gbm_bo_create(gbm, 0, 16, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING) == NULL);
  CHECK(gbm_bo_create(gbm, 16, 16, GBM_FORMAT_NV12, GBM_BO_USE_RENDERING) == NULL);
  CHECK(gbm_bo_create(gbm, 100000, 16, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING) == NULL);
  const uint64_t tiled = 0x0100000000000001ull;      /* (I915_FORMAT_MOD_X_TILED: no tiled layouts) */
  CHECK(gbm_bo_create_with_modifiers(gbm, 16, 16, GBM_FORMAT_ARGB8888, &tiled, 1) == NULL);
  const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
  struct gbm_bo *lin = gbm_bo_create_with_modifiers(gbm, 16, 16, GBM_FORMAT_ARGB8888, &linear, 1);
  CHECK(lin && gbm_bo_get_modifier(lin) == DRM_FORMAT_MOD_LINEAR && gbm_bo_get_plane_count(lin) == 1);
  if (lin) gbm_bo_destroy(lin);

  struct gbm_bo *bo = gbm_bo_create(gbm, 40, 30, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
  CHECK(bo != NULL);
  if (bo) {
    CHECK(gbm_bo_get_width(bo) == 40 && gbm_bo_get_height(bo) == 30 && gbm_bo_get_format(bo) == GBM_FORMAT_ARGB8888 && gbm_bo_get_bpp(bo) == 32);
    const uint32_t stride = gbm_bo_get_stride(bo);
    CHECK(stride >= 40 * 4 && gbm_bo_get_stride_for_plane(bo, 0) == stride && gbm_bo_get_offset(bo, 0) == 0);
    CHECK(gbm_bo_get_handle(bo).u32 != 0 && gbm_bo_get_device(bo) == gbm);
    /* written through a mapping, read back through another */
    uint32_t map_stride = 0;
    void *data = NULL;
    CHECK(gbm_bo_map(bo, 0, 0, 41, 30, GBM_BO_TRANSFER_WRITE, &map_stride, &data) == NULL);      /* (outside the buffer) */
    uint8_t *p = (uint8_t *)gbm_bo_map(bo, 0, 0, 40, 30, GBM_BO_TRANSFER_WRITE, &map_stride, &data);
    CHECK(p != NULL && map_stride == stride);
    if (p) {
      for (uint32_t y = 0; y < 30; ++y)
        for (uint32_t x = 0; x < 40; ++x) {
          const uint32_t v = 0xFF000000u | (x << 16) | (y << 8) | 0x55;
          memcpy(p + (size_t)y * map_stride + x * 4, &v, 4);
        }
      gbm_bo_unmap(bo, data);
    }
    CHECK(bo_pixel(bo, 7, 9) == (0xFF000000u | (7u << 16) | (9u << 8) | 0x55));
    /* a mapping of a part starts at that part */
    p = (uint8_t *)gbm_bo_map(bo, 3, 2, 4, 4, GBM_BO_TRANSFER_READ, &map_stride, &data);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4), gbm_bo_unmap(bo, data);
    CHECK(v == (0xFF000000u | (3u << 16) | (2u << 8) | 0x55));
    /* gbm_bo_write */
    uint8_t *rows = calloc(stride, 30);
    if (rows) {
      memset(rows, 0x11, (size_t)stride * 30);
      CHECK(gbm_bo_write(bo, rows, (size_t)stride * 30) == 0);
      CHECK(gbm_bo_write(bo, rows, (size_t)stride * 31) != 0);
      free(rows);
    }
    CHECK(bo_pixel(bo, 39, 29) == 0x11111111u);
    /* the dmabuf: another descriptor of the same image, imported back with the right description only */
    const int dmabuf = gbm_bo_get_fd(bo);
    CHECK(dmabuf >= 0 && dmabuf != fd);
    if (dmabuf >= 0) {
      CHECK(lseek(dmabuf, 0, SEEK_END) >= (off_t)stride * 30);
      struct gbm_import_fd_data good = {dmabuf, 40, 30, stride, GBM_FORMAT_ARGB8888};
      struct gbm_bo *in = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &good, GBM_BO_USE_RENDERING);
      CHECK(in && gbm_bo_get_stride(in) == stride && gbm_bo_get_width(in) == 40);
      if (in) {
        CHECK(bo_pixel(in, 0, 0) == 0x11111111u);      /* (the same picture) */
        gbm_bo_destroy(in);
      }
      struct gbm_import_fd_data wrong_size = {dmabuf, 41, 30, stride, GBM_FORMAT_ARGB8888};
      CHECK(gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &wrong_size, 0) == NULL);
      struct gbm_import_fd_data wrong_format = {dmabuf, 40, 30, stride, GBM_FORMAT_ABGR8888};
      CHECK(gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &wrong_format, 0) == NULL);
      struct gbm_import_fd_data xrgb = {dmabuf, 40, 30, stride, GBM_FORMAT_XRGB8888};      /* (the alpha channel ignored: the same picture) */
      struct gbm_bo *x = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &xrgb, 0);
      CHECK(x != NULL);
      if (x) gbm_bo_destroy(x);
      struct gbm_import_fd_modifier_data mod;
      memset(&mod, 0, sizeof mod);
      mod.width = 40, mod.height = 30, mod.format = GBM_FORMAT_ARGB8888, mod.num_fds = 1, mod.fds[0] = dmabuf, mod.strides[0] = (int)stride, mod.modifier = DRM_FORMAT_MOD_LINEAR;
      x = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &mod, 0);
      CHECK(x != NULL);
      if (x) gbm_bo_destroy(x);
      mod.offsets[0] = 64;
      CHECK(gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &mod, 0) == NULL);
      struct gbm_import_fd_data memfd = {not_dxg, 40, 30, stride, GBM_FORMAT_ARGB8888};
      CHECK(gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &memfd, 0) == NULL);      /* (not an image) */
      close(dmabuf);
    }
    gbm_bo_set_user_data(bo, &user_data_freed, free_user_data);
    CHECK(gbm_bo_get_user_data(bo) == &user_data_freed);
    gbm_bo_destroy(bo);
    CHECK(user_data_freed == 1);
  }

  /* ---- EGL on the GBM platform */
  const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  CHECK(has_word(client, "EGL_KHR_platform_gbm") && has_word(client, "EGL_MESA_platform_gbm"));
  EGLDisplay egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
  CHECK(egl != EGL_NO_DISPLAY && eglGetDisplay((EGLNativeDisplayType)gbm) == egl);
  CHECK(eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, &checks, NULL) == EGL_NO_DISPLAY);      /* (not a gbm_device) */
  EGLint major = 0, minor = 0;
  CHECK(eglInitialize(egl, &major, &minor) && major == 1 && minor == 5);
  const char *ext = eglQueryString(egl, EGL_EXTENSIONS);
  CHECK(has_word(ext, "EGL_KHR_image_base") && has_word(ext, "EGL_EXT_image_dma_buf_import") && has_word(ext, "EGL_EXT_image_dma_buf_import_modifiers"));
  /* the config whose visual is the gbm_surface's format, as KMS programs find it */
  const EGLint want[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig configs[16];
  EGLint count = 0;
  CHECK(eglChooseConfig(egl, want, configs, 16, &count) && count > 0);
  EGLConfig config = NULL;
  for (EGLint i = 0; i < count && !config; ++i) {
    EGLint visual = 0;
    if (eglGetConfigAttrib(egl, configs[i], EGL_NATIVE_VISUAL_ID, &visual) && (uint32_t)visual == GBM_FORMAT_XRGB8888) config = configs[i];
  }
  CHECK(config != NULL);
  struct gbm_surface *gs = gbm_surface_create(gbm, 64, 48, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  CHECK(gs != NULL && gbm_surface_has_free_buffers(gs));
  CHECK(gbm_surface_create(gbm, 64, 48, GBM_FORMAT_NV12, GBM_BO_USE_RENDERING) == NULL);
  CHECK(gbm_surface_lock_front_buffer(gs) == NULL);      /* (no frame yet) */
  EGLSurface surface = gs && config ? eglCreateWindowSurface(egl, config, (EGLNativeWindowType)gs, NULL) : EGL_NO_SURFACE;
  CHECK(surface != EGL_NO_SURFACE);
  CHECK(gs && config && eglCreateWindowSurface(egl, config, (EGLNativeWindowType)gs, NULL) == EGL_NO_SURFACE && eglGetError() == EGL_BAD_ALLOC);      /* (one per gbm_surface) */
  CHECK(config && eglCreateWindowSurface(egl, config, (EGLNativeWindowType)&checks, NULL) == EGL_NO_SURFACE && eglGetError() == EGL_BAD_NATIVE_WINDOW);
  const EGLint es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(egl, config, EGL_NO_CONTEXT, es2);
  CHECK(ctx != EGL_NO_CONTEXT);
  CHECK(eglMakeCurrent(egl, surface, surface, ctx));
  EGLint w = 0, h = 0;
  CHECK(eglQuerySurface(egl, surface, EGL_WIDTH, &w) && eglQuerySurface(egl, surface, EGL_HEIGHT, &h) && w == 64 && h == 48);
  CHECK(has_word((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image"));
  glViewport(0, 0, 64, 48);
  glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(eglSwapBuffers(egl, surface));
  struct gbm_bo *front = gbm_surface_lock_front_buffer(gs);
  CHECK(front != NULL);
  if (front) {
    CHECK(gbm_bo_get_width(front) == 64 && gbm_bo_get_height(front) == 48 && gbm_bo_get_format(front) == GBM_FORMAT_XRGB8888);
    const uint32_t px = bo_pixel(front, 10, 10) & 0x00FFFFFFu;      /* (R ff, G 7f or 80, B 3f or 40: within a step of rounding) */
    CHECK((px >> 16) == 0xFF && ((px >> 8) & 0xFF) >= 0x7F && ((px >> 8) & 0xFF) <= 0x80 && (px & 0xFF) >= 0x3F && (px & 0xFF) <= 0x40);
  }
  /* two more frames are queued; with all three buffers held a swap has nowhere to go */
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(eglSwapBuffers(egl, surface));
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(eglSwapBuffers(egl, surface));
  CHECK(!gbm_surface_has_free_buffers(gs));
  CHECK(!eglSwapBuffers(egl, surface) && eglGetError() == EGL_BAD_ALLOC);
  struct gbm_bo *newest = gbm_surface_lock_front_buffer(gs);      /* (the blue one; the green one, never locked, is free again) */
  CHECK(newest != NULL && newest != front);
  if (newest) CHECK((bo_pixel(newest, 63, 47) & 0x00FFFFFFu) == 0x0000FFu);
  CHECK(gbm_surface_has_free_buffers(gs));
  if (front) gbm_surface_release_buffer(gs, front);
  if (newest) gbm_surface_release_buffer(gs, newest);
  CHECK(eglSwapBuffers(egl, surface));
  struct gbm_bo *again = gbm_surface_lock_front_buffer(gs);
  CHECK(again != NULL);
  if (again) gbm_surface_release_buffer(gs, again);

  /* ---- EGL images of dmabufs */
  PFNEGLCREATEIMAGEKHRPROC create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  PFNEGLQUERYDMABUFFORMATSEXTPROC formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
  PFNEGLQUERYDMABUFMODIFIERSEXTPROC modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
  CHECK(create && target && formats && modifiers);
  if (create && target && formats && modifiers) {
    EGLint n = 0;
    CHECK(formats(egl, 0, NULL, &n) && n == 4);
    EGLint list[8];
    CHECK(formats(egl, 8, list, &n) && n == 4 && list[0] == (EGLint)GBM_FORMAT_ARGB8888);
    EGLuint64KHR mods[2] = {99, 99};
    CHECK(modifiers(egl, GBM_FORMAT_XRGB8888, 2, mods, NULL, &n) && n == 1 && mods[0] == DRM_FORMAT_MOD_LINEAR);
    /* a picture the CPU wrote: ABGR8888 (R, G, B, A bytes) and ARGB8888 (B, G, R, A) */
    struct gbm_bo *pic = gbm_bo_create(gbm, 8, 8, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING);
    CHECK(pic != NULL);
    if (pic) {
      const uint32_t stride = gbm_bo_get_stride(pic);
      uint8_t *rows = calloc(stride, 8);
      if (rows) {
        for (uint32_t y = 0; y < 8; ++y)
          for (uint32_t x = 0; x < 8; ++x) {
            uint8_t *q = rows + (size_t)y * stride + x * 4;
            q[0] = (uint8_t)(x * 30), q[1] = (uint8_t)(y * 30), q[2] = 0x77, q[3] = 0xFF;
          }
        gbm_bo_write(pic, rows, (size_t)stride * 8);
        free(rows);
      }
      const int pfd = gbm_bo_get_fd(pic);
      CHECK(image_pixel(egl, create, target, pfd, 8, 8, stride, GBM_FORMAT_ABGR8888, 3, 5) == ((3u * 30) << 24 | (5u * 30) << 16 | 0x77u << 8 | 0xFFu));
      CHECK(glGetError() == GL_NO_ERROR);
      /* the attribute checks */
      const EGLint no_fd[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_ABGR8888, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
      CHECK(create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, no_fd) == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_PARAMETER);
      const EGLint nv12[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_NV12, EGL_DMA_BUF_PLANE0_FD_EXT, pfd,
                             EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
      CHECK(create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, nv12) == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_MATCH);
      const EGLint short_pitch[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_ABGR8888, EGL_DMA_BUF_PLANE0_FD_EXT, pfd,
                                    EGL_DMA_BUF_PLANE0_PITCH_EXT, 16, EGL_NONE};
      CHECK(create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, short_pitch) == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_ACCESS);
      const EGLint too_tall[] = {EGL_WIDTH, 8, EGL_HEIGHT, 4000, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_ABGR8888, EGL_DMA_BUF_PLANE0_FD_EXT, pfd,
                                 EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
      CHECK(create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, too_tall) == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_ACCESS);      /* (past the descriptor's end) */
      const EGLint tiled_mod[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_ABGR8888, EGL_DMA_BUF_PLANE0_FD_EXT, pfd,
                                  EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 1, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0x01000000,
                                  EGL_NONE};
      CHECK(create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, tiled_mod) == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_MATCH);
      CHECK(create(egl, ctx, EGL_LINUX_DMA_BUF_EXT, NULL, no_fd) == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_PARAMETER);      /* (no context for dmabufs) */
      target(GL_TEXTURE_2D, (EGLImageKHR)&checks);      /* (not an image) */
      CHECK(glGetError() == GL_INVALID_OPERATION);
      close(pfd);
      gbm_bo_destroy(pic);
    }
    /* a picture the GPU drew: a frame of the gbm_surface, made a texture of another context */
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK(eglSwapBuffers(egl, surface));
    struct gbm_bo *frame = gbm_surface_lock_front_buffer(gs);
    CHECK(frame != NULL);
    if (frame) {
      EGLContext other = eglCreateContext(egl, config, EGL_NO_CONTEXT, es2);
      CHECK(other != EGL_NO_CONTEXT && eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, other));
      const int ffd = gbm_bo_get_fd(frame);
      CHECK(image_pixel(egl, create, target, ffd, 64, 48, gbm_bo_get_stride(frame), GBM_FORMAT_XRGB8888, 20, 20) == 0x0000FFFFu);
      close(ffd);
      eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(egl, other);
      gbm_surface_release_buffer(gs, frame);
    }
  }

  eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  CHECK(eglDestroySurface(egl, surface));
  CHECK(eglDestroyContext(egl, ctx));
  CHECK(eglTerminate(egl));
  if (gs) gbm_surface_destroy(gs);
  gbm_device_destroy(gbm);
  close(not_dxg);
  close(fd);
  printf("egl-gbm: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
