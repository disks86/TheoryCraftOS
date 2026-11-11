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

/* opengl32.dll in the OS: a window with a pixel format, WGL contexts, fixed-function and shader drawing read back with glReadPixels, buffer objects through
 * wglGetProcAddress, display lists and font lists, shared objects, WGL_ARB_create_context, swaps. Prints "file:line: Test failed: ..." for each failed check (as Wine's tests do) and
 * a summary line "opengltest: N checks, M failed". Build: x86_64-w64-mingw32-gcc -O1 -o opengl32_own.exe tests/windows/opengltest.c -lopengl32 -lgdi32 -luser32 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("opengltest.c:%d: Test failed: %s\n", __LINE__, #cond); } } while (0)

typedef void (APIENTRY *GenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *BindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
typedef GLuint (APIENTRY *CreateShader)(GLenum);
typedef void (APIENTRY *ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void (APIENTRY *CompileShader)(GLuint);
typedef GLuint (APIENTRY *CreateProgram)(void);
typedef void (APIENTRY *AttachShader)(GLuint, GLuint);
typedef void (APIENTRY *LinkProgram)(GLuint);
typedef void (APIENTRY *UseProgram)(GLuint);
typedef void (APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
typedef GLint (APIENTRY *GetUniformLocation)(GLuint, const char *);
typedef void (APIENTRY *Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *BindAttribLocation)(GLuint, GLuint, const char *);
typedef void (APIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *EnableVertexAttribArray)(GLuint);
typedef HGLRC (WINAPI *CreateContextAttribs)(HDC, HGLRC, const int *);
typedef const char *(WINAPI *GetExtensionsString)(HDC);
typedef void (APIENTRY *GenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRY *BindVertexArray)(GLuint);

static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) { return DefWindowProcA(hwnd, message, wparam, lparam); }

static int close_to(const unsigned char *p, int r, int g, int b) {
  const int d = 3;
  return abs(p[0] - r) <= d && abs(p[1] - g) <= d && abs(p[2] - b) <= d;
}

static void read_at(int x, int y, unsigned char out[4]) {
  memset(out, 0, 4);
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);      /* (a crash must not lose what was printed) */
  WNDCLASSA cls;
  memset(&cls, 0, sizeof(cls));
  cls.style = CS_OWNDC;
  cls.lpfnWndProc = proc;
  cls.hInstance = GetModuleHandleA(NULL);
  cls.lpszClassName = "opengltest";
  RegisterClassA(&cls);
  HWND hwnd = CreateWindowA("opengltest", "OpenGL test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 200, 200, NULL, NULL, cls.hInstance, NULL);
  CHECK(hwnd != NULL);
  HDC dc = GetDC(hwnd);
  CHECK(dc != NULL);
  RECT client;
  GetClientRect(hwnd, &client);
  const int w = client.right, h = client.bottom;
  CHECK(w > 0 && h > 0);

  PIXELFORMATDESCRIPTOR pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  const int format = ChoosePixelFormat(dc, &pfd);
  CHECK(format > 0);
  CHECK(SetPixelFormat(dc, format, &pfd));
  CHECK(GetPixelFormat(dc) == format);

  HGLRC rc = wglCreateContext(dc);
  CHECK(rc != NULL);
  CHECK(wglGetCurrentContext() == NULL);
  CHECK(wglMakeCurrent(dc, rc));
  CHECK(wglGetCurrentContext() == rc && wglGetCurrentDC() == dc);

  const char *version = (const char *)glGetString(GL_VERSION);
  const char *vendor = (const char *)glGetString(GL_VENDOR);
  printf("opengltest: GL_RENDERER %s\n", (const char *)glGetString(GL_RENDERER));
  printf("opengltest: GL_VERSION %s, GL_VENDOR %s\n", version ? version : "(null)", vendor ? vendor : "(null)");
  CHECK(version && version[0] >= '1');
  GLint viewport[4] = {0};
  glGetIntegerv(GL_VIEWPORT, viewport);
  CHECK(viewport[2] == w && viewport[3] == h);
  CHECK(glGetError() == GL_NO_ERROR);

  /* fixed function */
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, w, 0, h, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glBegin(GL_TRIANGLES);
  glColor3f(1, 0, 0);
  glVertex2i(0, 0);
  glVertex2i(w, 0);
  glVertex2i(0, h);
  glEnd();
  unsigned char px[4];
  read_at(5, 5, px);
  CHECK(close_to(px, 255, 0, 0));
  read_at(w - 5, h - 5, px);
  CHECK(close_to(px, 0, 0, 255));

  /* a texture */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  const unsigned char green[4] = {0, 255, 0, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glEnable(GL_TEXTURE_2D);
  glColor3f(1, 1, 1);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2i(0, 0); glTexCoord2f(1, 0); glVertex2i(20, 0); glTexCoord2f(1, 1); glVertex2i(20, 20); glTexCoord2f(0, 1); glVertex2i(0, 20);
  glEnd();
  glDisable(GL_TEXTURE_2D);
  read_at(10, 10, px);
  CHECK(close_to(px, 0, 255, 0));

  /* display list */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glColor3f(1, 1, 0);
  glRecti(w - 20, 0, w, 20);
  glEndList();
  glCallList(list);
  read_at(w - 10, 10, px);
  CHECK(close_to(px, 255, 255, 0));
  CHECK(glGetError() == GL_NO_ERROR);

  /* extensions through wglGetProcAddress */
  GenBuffers genBuffers = (GenBuffers)(void *)wglGetProcAddress("glGenBuffers");
  BindBuffer bindBuffer = (BindBuffer)(void *)wglGetProcAddress("glBindBuffer");
  BufferData bufferData = (BufferData)(void *)wglGetProcAddress("glBufferData");
  CHECK(genBuffers && bindBuffer && bufferData);
  CHECK(wglGetProcAddress("glNoSuchFunction") == NULL);
  CHECK(wglGetProcAddress("glGenBuffersARB") != NULL);
  if (genBuffers && bindBuffer && bufferData) {
    const float quad[8] = {30, 30, 50, 30, 50, 50, 30, 50};
    GLuint vbo = 0;
    genBuffers(1, &vbo);
    bindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, vbo);
    bufferData(0x8892, sizeof(quad), quad, 0x88E4 /*GL_STATIC_DRAW*/);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, NULL);
    glColor3f(0, 1, 1);
    glDrawArrays(GL_QUADS, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    bindBuffer(0x8892, 0);
    read_at(40, 40, px);
    CHECK(close_to(px, 0, 255, 255));
  }

  /* a shader program */
  CreateShader createShader = (CreateShader)(void *)wglGetProcAddress("glCreateShader");
  ShaderSource shaderSource = (ShaderSource)(void *)wglGetProcAddress("glShaderSource");
  CompileShader compileShader = (CompileShader)(void *)wglGetProcAddress("glCompileShader");
  CreateProgram createProgram = (CreateProgram)(void *)wglGetProcAddress("glCreateProgram");
  AttachShader attachShader = (AttachShader)(void *)wglGetProcAddress("glAttachShader");
  LinkProgram linkProgram = (LinkProgram)(void *)wglGetProcAddress("glLinkProgram");
  UseProgram useProgram = (UseProgram)(void *)wglGetProcAddress("glUseProgram");
  GetProgramiv getProgramiv = (GetProgramiv)(void *)wglGetProcAddress("glGetProgramiv");
  GetUniformLocation getUniformLocation = (GetUniformLocation)(void *)wglGetProcAddress("glGetUniformLocation");
  Uniform4f uniform4f = (Uniform4f)(void *)wglGetProcAddress("glUniform4f");
  BindAttribLocation bindAttribLocation = (BindAttribLocation)(void *)wglGetProcAddress("glBindAttribLocation");
  VertexAttribPointer vertexAttribPointer = (VertexAttribPointer)(void *)wglGetProcAddress("glVertexAttribPointer");
  EnableVertexAttribArray enableVertexAttribArray = (EnableVertexAttribArray)(void *)wglGetProcAddress("glEnableVertexAttribArray");
  CHECK(createShader && shaderSource && compileShader && createProgram && linkProgram && useProgram && uniform4f);
  if (createShader && shaderSource && compileShader && createProgram && attachShader && linkProgram && useProgram && getProgramiv && getUniformLocation && uniform4f && bindAttribLocation &&
      vertexAttribPointer && enableVertexAttribArray) {
    const char *vs = "#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p, 0.0, 1.0); }\n";
    const char *fs = "#version 300 es\nprecision mediump float;\nuniform vec4 c;\nout vec4 o;\nvoid main(){ o = c; }\n";
    const GLuint v = createShader(0x8B31), f = createShader(0x8B30), p = createProgram();
    shaderSource(v, 1, &vs, NULL);
    shaderSource(f, 1, &fs, NULL);
    compileShader(v);
    compileShader(f);
    attachShader(p, v);
    attachShader(p, f);
    bindAttribLocation(p, 1, "p");
    linkProgram(p);
    GLint linked = 0;
    getProgramiv(p, 0x8B82 /*GL_LINK_STATUS*/, &linked);
    CHECK(linked == 1);
    useProgram(p);
    uniform4f(getUniformLocation(p, "c"), 1, 0, 1, 1);
    const float tri[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    enableVertexAttribArray(1);
    vertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    useProgram(0);
    read_at(w / 2 + 5, h / 2 + 5, px);
    CHECK(close_to(px, 255, 0, 255));
  }
  CHECK(glGetError() == GL_NO_ERROR);

  /* a font list */
  SelectObject(dc, GetStockObject(SYSTEM_FONT));
  CHECK(wglUseFontBitmapsA(dc, 32, 96, 1000));
  glColor3f(1, 1, 1);
  glRasterPos2i(60, 60);
  glListBase(1000 - 32);
  glCallLists(5, GL_UNSIGNED_BYTE, "Hello");
  GLfloat raster[4] = {0};
  glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
  CHECK(raster[0] > 60);      /* the glyph advances moved it */

  /* a second context sharing objects */
  HGLRC rc2 = wglCreateContext(dc);
  CHECK(rc2 != NULL);
  CHECK(wglShareLists(rc, rc2));
  CHECK(wglMakeCurrent(dc, rc2));
  CHECK(glIsList(list));
  CHECK(glIsTexture(tex));

  /* WGL_ARB_create_context */
  GetExtensionsString getExtensions = (GetExtensionsString)(void *)wglGetProcAddress("wglGetExtensionsStringARB");
  CHECK(getExtensions && strstr(getExtensions(dc), "WGL_ARB_create_context"));
  CreateContextAttribs createAttribs = (CreateContextAttribs)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
  CHECK(createAttribs != NULL);
  if (createAttribs) {
    const int attribs[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0};      /* 3.3 core */
    HGLRC core = createAttribs(dc, NULL, attribs);
    CHECK(core != NULL);
    const int too_new[] = {0x2091, 9, 0x2092, 0, 0};
    CHECK(createAttribs(dc, NULL, too_new) == NULL);
    if (core && wglMakeCurrent(dc, core)) {
      GLint mask = 0;
      glGetIntegerv(0x9126 /*GL_CONTEXT_PROFILE_MASK*/, &mask);
      CHECK(mask == 1);
      glBegin(GL_TRIANGLES);      /* not in a core context */
      CHECK(glGetError() == GL_INVALID_OPERATION);
      GenVertexArrays genVertexArrays = (GenVertexArrays)(void *)wglGetProcAddress("glGenVertexArrays");
      BindVertexArray bindVertexArray = (BindVertexArray)(void *)wglGetProcAddress("glBindVertexArray");
      CHECK(genVertexArrays && bindVertexArray);
      glClearColor(0, 1, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      read_at(1, 1, px);
      CHECK(close_to(px, 0, 255, 0));
      wglMakeCurrent(NULL, NULL);
      CHECK(wglDeleteContext(core));
    }
  }

  /* swap and show */
  CHECK(wglMakeCurrent(dc, rc));
  glClearColor(1, 0.5f, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  CHECK(SwapBuffers(dc));
  glReadBuffer(GL_FRONT);
  read_at(3, 3, px);
  CHECK(close_to(px, 255, 128, 0));
  glReadBuffer(GL_BACK);
  for (int frame = 0; frame < 30; ++frame) {
    glClearColor(frame / 30.0f, 0, 1 - frame / 30.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers(dc);
  }
  CHECK(glGetError() == GL_NO_ERROR);

  CHECK(wglMakeCurrent(NULL, NULL));
  CHECK(wglGetCurrentContext() == NULL);
  CHECK(wglDeleteContext(rc2));
  CHECK(wglDeleteContext(rc));
  CHECK(!wglDeleteContext(rc));
  ReleaseDC(hwnd, dc);
  DestroyWindow(hwnd);
  printf("opengltest: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
