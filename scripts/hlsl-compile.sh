#!/usr/bin/env bash
# Compiles one HLSL file to Direct3D bytecode with the D3DCompile of Wine's d3dcompiler_47 (vkd3d-shader), called from a small program built with the
# mingw compiler: for the shaders and test fixtures that are checked in compiled (scripts/gen-d3d9-shaders.sh, scripts/gen-d3d9-test-shaders.sh). The
# build does not need it. Needs x86_64-w64-mingw32-gcc and wine. The profile says what comes out: vs_4_0 / ps_4_0 a DXBC container, vs_1_1 .. vs_3_0
# and ps_2_0 .. ps_3_0 the token stream of Direct3D 9.
# Usage: scripts/hlsl-compile.sh <in.hlsl> <entry point> <profile> <out>
set -euo pipefail
[ $# -eq 4 ] || { echo "usage: $0 <in.hlsl> <entry point> <profile> <out>" >&2; exit 2; }
work=${TMPDIR:-/tmp}/tcos-hlsl-compile
mkdir -p "$work"
if [ ! -f "$work/hc.exe" ]; then
  cat > "$work/hc.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <d3dcompiler.h>
int main(int argc, char **argv) {
  static char source[1 << 16];
  if (argc < 5) return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 3;
  const size_t n = fread(source, 1, sizeof source, f);
  fclose(f);
  ID3DBlob *code = NULL, *errors = NULL;
  const HRESULT hr = D3DCompile(source, n, argv[1], NULL, NULL, argv[2], argv[3], 0, 0, &code, &errors);
  if (errors) fprintf(stderr, "%.*s\n", (int)errors->lpVtbl->GetBufferSize(errors), (char *)errors->lpVtbl->GetBufferPointer(errors));
  if (FAILED(hr) || !code) return 1;
  f = fopen(argv[4], "wb");
  if (!f) return 4;
  fwrite(code->lpVtbl->GetBufferPointer(code), 1, code->lpVtbl->GetBufferSize(code), f);
  fclose(f);
  return 0;
}
C
  x86_64-w64-mingw32-gcc -O1 -static -o "$work/hc.exe" "$work/hc.c" -ld3dcompiler_47
fi
cp "$1" "$work/in.hlsl"
rm -f "$work/out.bin"
(cd "$work" && WINEDEBUG=-all wine hc.exe in.hlsl "$2" "$3" out.bin) 2> >(grep -v "radv is not a conformant" >&2)
cp "$work/out.bin" "$4"
