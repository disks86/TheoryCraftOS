#!/usr/bin/env bash
# Windows sockets over IPv6 in the real kernel: builds tests/windows/sock6test.c with mingw, a kernel that runs it once the hardware is up
# (-DTCOS_WINE_TEST_PROGRAM, in a build directory of its own), and boots that under QEMU with an e1000e on the user network (SLAAC from QEMU's
# router advertisement). An echo server on the host's [::1]:7777 is what the program reaches as [fec0::2]:7777. Passes when every check of the
# program passed and the echo went through. Needs qemu-system-x86_64, OVMF, mingw (x86_64-w64-mingw32-gcc) and python3.
# Usage: tests/windows_ipv6_qemu.sh [seconds]
set -uo pipefail
cd "$(dirname "$0")/.."
seconds=${1:-240}
work=${TMPDIR:-/tmp}/tcos-windows-ipv6
mkdir -p "$work"
for tool in qemu-system-x86_64 x86_64-w64-mingw32-gcc python3; do command -v $tool >/dev/null || { echo "skipped (needs $tool)"; exit 0; }; done
code=$(grep -m1 '^OVMF_CODE:' build/CMakeCache.txt | cut -d= -f2)
vars=$(grep -m1 '^OVMF_VARS:' build/CMakeCache.txt | cut -d= -f2)
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/sock6test.exe" tests/windows/sock6test.c -lws2_32 -liphlpapi || exit 1
# (configured every time: the build directory may have been pointed at another program since)
cmake -S . -B "$work/build" -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-x86_64-uefi.cmake" -DTCOS_WINE_TEST_PROGRAM="$work/sock6test.exe" -DTCOS_WINE_TEST_FILES= >/dev/null || exit 1
cmake --build build --target esp -j"$(nproc)" >/dev/null && cmake --build "$work/build" --target BOOTX64 -j"$(nproc)" >/dev/null || exit 1
rm -rf "$work/esp" "$work/debugcon.log"
cp -r build/esp "$work/esp" && cp "$work/build/BOOTX64.efi" "$work/esp/EFI/BOOT/BOOTX64.EFI"
cp "$vars" "$work/vars.fd" && chmod u+w "$work/vars.fd"
cat > "$work/echo.py" <<'PY'
import socket, socketserver
class Tcp6(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    address_family = socket.AF_INET6
class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data: return
            self.request.sendall(data)
Tcp6(("::1", 7777), Echo).serve_forever()
PY
python3 -I "$work/echo.py" & echo_pid=$!
trap 'kill $echo_pid 2>/dev/null' EXIT
accel=()
[ -r /dev/kvm ] && [ -w /dev/kvm ] && accel=(-accel kvm -cpu host)
# (QEMU stops at the first line saying how the test program ended)
timeout "$seconds" qemu-system-x86_64 "${accel[@]}" -machine q35 -smp 4 -m 4G \
  -netdev user,id=n1 -device e1000e,netdev=n1,mac=52:54:00:00:00:e1 \
  -vga std -display none \
  -drive if=pflash,format=raw,readonly=on,file="$code" -drive if=pflash,format=raw,file="$work/vars.fd" \
  -drive format=raw,file=fat:rw:"$work/esp" \
  -chardev file,id=dbg,path="$work/debugcon.log" -device isa-debugcon,iobase=0x402,chardev=dbg -serial null >/dev/null 2>"$work/qemu.err" &
qemu_pid=$!
for _ in $(seq "$seconds"); do
  grep -aq "test program: result" "$work/debugcon.log" 2>/dev/null && break
  kill -0 $qemu_pid 2>/dev/null || break
  sleep 1
done
kill $qemu_pid 2>/dev/null
wait $qemu_pid 2>/dev/null
grep -aE "sock6test|FAIL:|test program: result" "$work/debugcon.log"
grep -aqE "sock6test: [0-9]+ checks, 0 failed" "$work/debugcon.log" || { echo "FAILED: sock6test did not pass"; exit 1; }
grep -aq "sock6test: echo over IPv6 ok" "$work/debugcon.log" || { echo "FAILED: no IPv6 echo through the host"; exit 1; }
echo "passed"
