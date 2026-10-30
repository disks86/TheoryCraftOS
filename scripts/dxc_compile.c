// Compiles an HLSL file to DXIL with a dxcompiler library (libdxcompiler.so): the way the tests/data/dxbc/dxil_*.dxil files were made.
//   cc -o dxc_compile scripts/dxc_compile.c -ldl
//   ./dxc_compile /path/to/libdxcompiler.so in.hlsl out.dxil ps_6_0 [entry]
// (-Vd: the validator of that library refuses the bitcode its own compiler writes for typed texture loads.)
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef struct { unsigned a; unsigned short b, c; unsigned char d[8]; } GUID_;
typedef struct IUnk { void **v; } IUnk;
typedef struct { const void *Ptr; size_t Size; unsigned Encoding; } DxcBuffer;
typedef long (*CreateFn)(const GUID_*, const GUID_*, void**);
int main(int argc, char** argv) {
  if (argc < 5) return 2;
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  CreateFn create = (CreateFn)dlsym(lib, "DxcCreateInstance");
  GUID_ clsid = {0x73E22D93, 0xE6CE, 0x47F3, {0xB5,0xBF,0xF0,0x66,0x4F,0x39,0xC1,0xB0}};
  GUID_ iid = {0x228B4687, 0x5A6A, 0x4730, {0x90,0x0C,0x97,0x02,0xB2,0x20,0x3F,0x54}};
  GUID_ iid_result = {0xCEDB484A, 0xD4E9, 0x445A, {0xB9,0x91,0xCA,0x21,0xCA,0x15,0x7D,0xC2}};
  IUnk* compiler = 0;
  long hr = create(&clsid, &iid, (void**)&compiler);
  if (hr) { fprintf(stderr, "create: %lx\n", hr); return 1; }
  FILE* f = fopen(argv[2], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char* src = malloc(n + 1); fread(src, 1, n, f); fclose(f);
  DxcBuffer buf = {src, (size_t)n, 65001};
  wchar_t prof[32], entry[32];
  mbstowcs(prof, argv[4], 32); mbstowcs(entry, argc > 5 ? argv[5] : "main", 32);
  const wchar_t* args[] = {L"-T", prof, L"-E", entry, L"-Vd"};
  IUnk* result = 0;
  // IDxcCompiler3::Compile is slot 3
  typedef long (*CompileFn)(IUnk*, DxcBuffer*, const wchar_t**, unsigned, void*, const GUID_*, void**);
  hr = ((CompileFn)compiler->v[3])(compiler, &buf, args, 5, 0, &iid_result, (void**)&result);
  if (hr) { fprintf(stderr, "compile: %lx\n", hr); return 1; }
  // IDxcOperationResult: GetStatus 3, GetResult 4, GetErrorBuffer 5
  int status = -1;
  ((long (*)(IUnk*, int*))result->v[3])(result, &status);
  IUnk* errors = 0;
  ((long (*)(IUnk*, IUnk**))result->v[5])(result, &errors);
  if (errors) {
    void* ep = ((void* (*)(IUnk*))errors->v[3])(errors); size_t es = ((size_t (*)(IUnk*))errors->v[4])(errors);
    if (es) fwrite(ep, 1, es, stderr);
  }
  if (status) { fprintf(stderr, "status %x\n", status); return 1; }
  IUnk* blob = 0;
  ((long (*)(IUnk*, IUnk**))result->v[4])(result, &blob);
  void* p = ((void* (*)(IUnk*))blob->v[3])(blob); size_t s = ((size_t (*)(IUnk*))blob->v[4])(blob);
  f = fopen(argv[3], "wb"); fwrite(p, 1, s, f); fclose(f);
  printf("%zu bytes\n", s);
  return 0;
}
