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

/* What Google Chrome's GPU process does with ANGLE (the libEGL.dll and libGLESv2.dll Chrome ships) on Windows: an EGL display on Direct3D 11 (the WARP device,
 * as Chrome asks for when it has no GPU it knows), a context and a pbuffer, a clear, a shader-drawn triangle, and the pixels read back. EGL_KHR_debug messages
 * are printed, so a refusal says why.
 * Build: x86_64-w64-mingw32-gcc -O1 -o angletest.exe angletest.c
 * Run:   angletest.exe [warp|hardware] [ANGLE folder]   (default: warp, and the newest version folder of an installed Chrome - which has no separate ANGLE
 *        DLLs since Chrome links ANGLE in; give the folder of a build that has libEGL.dll and libGLESv2.dll). Writes C:\angletest.txt too.
 * Prints "angletest: N checks, M failed". */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef intptr_t EGLAttrib;
typedef void (*Proc)(void);
typedef void (WINAPI *DebugProc)(EGLenum error, const char *command, EGLint type, void *thread_label, void *object_label, const char *message);

#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x322A
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE 0x320B
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_DEBUG_MSG_CRITICAL_KHR 0x33B9
#define EGL_DEBUG_MSG_ERROR_KHR 0x33BA
#define EGL_DEBUG_MSG_WARN_KHR 0x33BB
#define EGL_DEBUG_MSG_INFO_KHR 0x33BC
#define EGL_TRUE 1

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static FILE *out;
static int checks, failed;
static void say(const char *format, ...) {
  va_list a;
  va_start(a, format);
  vprintf(format, a);
  va_end(a);
  if (out) { va_start(a, format); vfprintf(out, format, a); va_end(a); fflush(out); }
  fflush(stdout);
}
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; say("angletest: line %d failed: %s\n", __LINE__, #c); } } while (0)

static void WINAPI on_debug(EGLenum error, const char *command, EGLint type, void *thread_label, void *object_label, const char *message) {
  (void)thread_label; (void)object_label;
  const char *kind = type == EGL_DEBUG_MSG_CRITICAL_KHR ? "critical" : type == EGL_DEBUG_MSG_ERROR_KHR ? "error" : type == EGL_DEBUG_MSG_WARN_KHR ? "warning" : "info";
  say("angletest: egl %s %s (error %#x): %s\n", kind, command ? command : "?", error, message ? message : "");
}

static BOOL newest_angle_folder(char *folder, size_t capacity) {
  const char *base = "C:\\Program Files\\Google\\Chrome\\Application\\";
  char pattern[MAX_PATH];
  snprintf(pattern, sizeof pattern, "%s*", base);
  WIN32_FIND_DATAA f;
  HANDLE h = FindFirstFileA(pattern, &f);
  if (h == INVALID_HANDLE_VALUE) return FALSE;
  char best[64] = "";      /* (a version: 158.0.8127.37) */
  do {
    if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && f.cFileName[0] >= '0' && f.cFileName[0] <= '9' && strcmp(f.cFileName, best) > 0) {
      strncpy(best, f.cFileName, sizeof best - 1);
      best[sizeof best - 1] = 0;
    }
  } while (FindNextFileA(h, &f));
  FindClose(h);
  if (!best[0]) return FALSE;
  snprintf(folder, capacity, "%s%s", base, best);
  return TRUE;
}

int main(int argc, char **argv) {
  out = fopen("C:\\angletest.txt", "w");
  const BOOL warp = argc < 2 || strcmp(argv[1], "hardware") != 0;
  char folder[MAX_PATH];
  if (argc >= 3) snprintf(folder, sizeof folder, "%s", argv[2]);
  else if (!newest_angle_folder(folder, sizeof folder)) { say("angletest: no Chrome folder\n"); return 1; }
  say("angletest: ANGLE from %s, %s device\n", folder, warp ? "WARP" : "hardware");
  SetDllDirectoryA(folder);      /* (libGLESv2.dll and d3dcompiler_47.dll are found next to libEGL.dll) */
  char path[MAX_PATH];
  snprintf(path, sizeof path, "%s\\libEGL.dll", folder);
  HMODULE egl = LoadLibraryA(path);
  snprintf(path, sizeof path, "%s\\libGLESv2.dll", folder);
  HMODULE gles = LoadLibraryA(path);
  if (!egl || !gles) {      /* (Chrome links ANGLE into chrome.dll: give the folder of an ANGLE build with the two DLLs, such as Electron's or a Chromium one) */
    say("angletest: no libEGL.dll and libGLESv2.dll in %s: pass the folder of an ANGLE build\n", folder);
    CHECK(egl != NULL && gles != NULL);
    goto done;
  }

#define EGLF(name, ret, args) typedef ret (*name##_fn) args; name##_fn name = (name##_fn)(void (*)(void))GetProcAddress(egl, #name)
  EGLF(eglGetProcAddress, Proc, (const char *));
  EGLF(eglInitialize, EGLBoolean, (EGLDisplay, EGLint *, EGLint *));
  EGLF(eglGetError, EGLint, (void));
  EGLF(eglQueryString, const char *, (EGLDisplay, EGLint));
  EGLF(eglChooseConfig, EGLBoolean, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *));
  EGLF(eglCreatePbufferSurface, EGLSurface, (EGLDisplay, EGLConfig, const EGLint *));
  EGLF(eglCreateContext, EGLContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
  EGLF(eglMakeCurrent, EGLBoolean, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
  EGLF(eglTerminate, EGLBoolean, (EGLDisplay));
  CHECK(eglGetProcAddress && eglInitialize && eglGetError && eglChooseConfig && eglCreatePbufferSurface && eglCreateContext && eglMakeCurrent);
  if (!eglGetProcAddress || !eglInitialize) goto done;
  EGLint (*debug_control)(DebugProc, const EGLAttrib *) = (EGLint (*)(DebugProc, const EGLAttrib *))eglGetProcAddress("eglDebugMessageControlKHR");
  if (debug_control) {
    const EGLAttrib all[] = {EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE, EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE, EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    debug_control(on_debug, all);
  }
  EGLDisplay (*get_platform_display)(EGLenum, void *, const EGLint *) = (EGLDisplay (*)(EGLenum, void *, const EGLint *))eglGetProcAddress("eglGetPlatformDisplayEXT");
  CHECK(get_platform_display != NULL);
  if (!get_platform_display) goto done;
  const EGLint display_attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
                                       warp ? EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE : EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE, EGL_NONE};
  EGLDisplay display = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE, NULL, display_attributes);
  CHECK(display != NULL);
  EGLint major = 0, minor = 0;
  const EGLBoolean initialized = display && eglInitialize(display, &major, &minor);
  CHECK(initialized);
  if (!initialized) { say("angletest: eglInitialize error %#x\n", eglGetError()); goto done; }
  say("angletest: EGL %d.%d %s %s\n", major, minor, eglQueryString(display, EGL_VENDOR), eglQueryString(display, EGL_VERSION));

  const EGLint config_attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig config = NULL;
  EGLint configs = 0;
  CHECK(eglChooseConfig(display, config_attributes, &config, 1, &configs) && configs == 1);
  const EGLint surface_attributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
  CHECK(surface != NULL);
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, NULL, context_attributes);
  CHECK(context != NULL);
  const EGLBoolean current = surface && context && eglMakeCurrent(display, surface, surface, context);
  CHECK(current);
  if (!current) { say("angletest: eglMakeCurrent/create error %#x\n", eglGetError()); goto done; }

#define GLF(name, ret, args) typedef ret (*name##_fn) args; name##_fn name = (name##_fn)eglGetProcAddress(#name)
  GLF(glGetString, const char *, (unsigned));
  GLF(glClearColor, void, (float, float, float, float));
  GLF(glClear, void, (unsigned));
  GLF(glReadPixels, void, (int, int, int, int, unsigned, unsigned, void *));
  GLF(glGetError, unsigned, (void));
  GLF(glCreateShader, unsigned, (unsigned));
  GLF(glShaderSource, void, (unsigned, int, const char *const *, const int *));
  GLF(glCompileShader, void, (unsigned));
  GLF(glGetShaderiv, void, (unsigned, unsigned, int *));
  GLF(glGetShaderInfoLog, void, (unsigned, int, int *, char *));
  GLF(glCreateProgram, unsigned, (void));
  GLF(glAttachShader, void, (unsigned, unsigned));
  GLF(glBindAttribLocation, void, (unsigned, unsigned, const char *));
  GLF(glLinkProgram, void, (unsigned));
  GLF(glGetProgramiv, void, (unsigned, unsigned, int *));
  GLF(glGetProgramInfoLog, void, (unsigned, int, int *, char *));
  GLF(glUseProgram, void, (unsigned));
  GLF(glVertexAttribPointer, void, (unsigned, int, unsigned, unsigned char, int, const void *));
  GLF(glEnableVertexAttribArray, void, (unsigned));
  GLF(glDrawArrays, void, (unsigned, int, int));
  GLF(glViewport, void, (int, int, int, int));
  say("angletest: GL %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

  unsigned char pixel[4] = {0, 0, 0, 0};
  glViewport(0, 0, 64, 64);
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  say("angletest: clear read %d %d %d %d\n", pixel[0], pixel[1], pixel[2], pixel[3]);
  CHECK(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);

  /* a triangle over the whole viewport, drawn green by a shader (HLSL through d3dcompiler_47.dll) */
  const char *vs_text = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
  const char *fs_text = "precision mediump float; uniform float u; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }";
  unsigned shaders[2] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
  const char *texts[2] = {vs_text, fs_text};
  for (int i = 0; i < 2; ++i) {
    int ok = 0;
    glShaderSource(shaders[i], 1, &texts[i], NULL);
    glCompileShader(shaders[i]);
    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512] = ""; glGetShaderInfoLog(shaders[i], sizeof log, NULL, log); say("angletest: shader %d: %s\n", i, log); }
    CHECK(ok);
  }
  unsigned program = glCreateProgram();
  glAttachShader(program, shaders[0]);
  glAttachShader(program, shaders[1]);
  glBindAttribLocation(program, 0, "p");
  glLinkProgram(program);
  int linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) { char log[512] = ""; glGetProgramInfoLog(program, sizeof log, NULL, log); say("angletest: link: %s\n", log); }
  CHECK(linked);
  static const float triangle[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  glUseProgram(program);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, triangle);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  memset(pixel, 0, sizeof pixel);
  glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  say("angletest: draw read %d %d %d %d, gl error %#x\n", pixel[0], pixel[1], pixel[2], pixel[3], glGetError());
  CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255);
  eglMakeCurrent(display, NULL, NULL, NULL);
  eglTerminate(display);

done:
  say("angletest: %d checks, %d failed\n", checks, failed);
  if (out) fclose(out);
  return failed;
}
