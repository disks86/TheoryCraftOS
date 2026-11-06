#!/usr/bin/env bash
# TCP through QEMU's e1000e (82574L) and igb (82576) with a self-test kernel (-DTCOS_SELFTESTS=ON): the network self-test sends 256 KiB to an
# echo server on the host (started here, reached at the gateway) and checks what comes back. Passes when both interfaces get the bulk echo
# right with segments cut by the interface (TSO) and received segments coalesced (GRO), over IPv4 and over IPv6 (SLAAC from QEMU's router
# advertisement, the host at [fec0::2] reached on its loopback [::1]), the HTTP client fetches a page from http://[fec0::2]:7780/ over IPv6,
# and every TCP checksum on both wires is right and no frame is longer than 1514 bytes. Needs qemu-system-x86_64, OVMF, python3 and tcpdump.
# Usage: tests/nic_qemu_tcp.sh <self-test build dir> [seconds]   (the ESP is taken from build/ and the kernel from the self-test build)
set -uo pipefail
cd "$(dirname "$0")/.."
selftest=${1:?usage: tests/nic_qemu_tcp.sh <self-test build dir> [seconds]}
seconds=${2:-240}
work=${TMPDIR:-/tmp}/tcos-nic-qemu-tcp
mkdir -p "$work"
for tool in qemu-system-x86_64 tcpdump python3; do command -v $tool >/dev/null || { echo "skipped (needs $tool)"; exit 0; }; done
code=$(grep -m1 '^OVMF_CODE:' build/CMakeCache.txt | cut -d= -f2)
vars=$(grep -m1 '^OVMF_VARS:' build/CMakeCache.txt | cut -d= -f2)
grep -q '^TCOS_SELFTESTS:BOOL=ON' "$selftest/CMakeCache.txt" || { echo "$selftest is not a TCOS_SELFTESTS=ON build"; exit 1; }
cmake --build build --target esp -j"$(nproc)" >/dev/null && cmake --build "$selftest" --target BOOTX64 -j"$(nproc)" >/dev/null || exit 1
rm -rf "$work/esp" "$work"/n*.pcap "$work/debugcon.log"
cp -r build/esp "$work/esp" && cp "$selftest/BOOTX64.efi" "$work/esp/EFI/BOOT/BOOTX64.EFI"
cp "$vars" "$work/vars.fd" && chmod u+w "$work/vars.fd"
cat > "$work/echo.py" <<'PY'
import http.server, socket, socketserver, threading
class Tcp(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data: return
            self.request.sendall(data)
def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 7778))
    while True:
        d, a = s.recvfrom(65536); s.sendto(d, a)
class Tcp6(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    address_family = socket.AF_INET6
class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"hello over IPv6\n"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *args): pass
class Http6(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6
threading.Thread(target=udp, daemon=True).start()
threading.Thread(target=lambda: Http6(("::1", 7780), Page).serve_forever(), daemon=True).start()
threading.Thread(target=lambda: Tcp6(("::1", 7777), Echo).serve_forever(), daemon=True).start()
Tcp(("127.0.0.1", 7777), Echo).serve_forever()
PY
python3 -I "$work/echo.py" & echo_pid=$!
trap 'kill $echo_pid 2>/dev/null' EXIT
accel=()
[ -r /dev/kvm ] && [ -w /dev/kvm ] && accel=(-accel kvm -cpu host)
timeout "$seconds" qemu-system-x86_64 "${accel[@]}" -machine q35 -smp 4 -m 4G \
  -netdev user,id=n1 -object filter-dump,id=f1,netdev=n1,file="$work/n1.pcap" -device e1000e,netdev=n1,mac=52:54:00:00:00:e1 \
  -netdev user,id=n2 -object filter-dump,id=f2,netdev=n2,file="$work/n2.pcap" -device igb,netdev=n2,mac=52:54:00:00:00:1b \
  -vga std -display none \
  -drive if=pflash,format=raw,readonly=on,file="$code" -drive if=pflash,format=raw,file="$work/vars.fd" \
  -drive format=raw,file=fat:rw:"$work/esp" \
  -chardev file,id=dbg,path="$work/debugcon.log" -device isa-debugcon,iobase=0x402,chardev=dbg -serial null >/dev/null 2>"$work/qemu.err"

grep -aE "Network:   |bulk echo|IPv6: |ping6|HTTP over IPv6" "$work/debugcon.log"
failed=0
[ "$(grep -acE 'TCP bulk echo from host: ok, 256 KiB, [1-9][0-9]* segments cut by the interface, [1-9][0-9]* received segments coalesced' "$work/debugcon.log")" -eq 2 ] ||
  { echo "FAILED: not both interfaces did the bulk echo with segmentation and coalescing"; failed=1; }
[ "$(grep -acE 'TCP bulk echo over IPv6: ok, 256 KiB, [1-9][0-9]* segments cut by the interface, [1-9][0-9]* received segments coalesced' "$work/debugcon.log")" -eq 2 ] ||
  { echo "FAILED: not both interfaces did the IPv6 bulk echo with segmentation and coalescing"; failed=1; }
[ "$(grep -acE 'HTTP over IPv6: ok' "$work/debugcon.log")" -eq 2 ] || { echo "FAILED: not both interfaces fetched the page over IPv6"; failed=1; }
for n in "n1 e1000e" "n2 igb"; do
  set -- $n
  dump=$(tcpdump -nn -vv -e -r "$work/$1.pcap" tcp 2>/dev/null)
  good=$(grep -ac "(correct" <<<"$dump"); bad=$(grep -ac "(incorrect" <<<"$dump")
  longest=$(grep -aoE 'length [0-9]+:' <<<"$dump" | awk '{print $2+0}' | sort -n | tail -1)
  if [ "$bad" -ne 0 ] || [ "$good" -eq 0 ] || [ "${longest:-0}" -gt 1514 ]; then echo "$2: FAILED ($good right, $bad wrong TCP checksums, longest frame ${longest:-?})"; failed=1
  else echo "$2: $good TCP checksums right, longest frame $longest"; fi
done
exit $failed
