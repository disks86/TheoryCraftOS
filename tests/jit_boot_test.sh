#!/usr/bin/env bash
# Boots the real image of each architecture under QEMU and checks the "JIT: self test passed" line the image prints once its cores run: the JIT's code
# memory (page tables on x86-64, the firmware's memory attribute protocol on AArch64, the RISC-V page tables) gave executable pages and the generated code ran.
# Usage: tests/jit_boot_test.sh [x86_64] [aarch64] [riscv64]   (default: all). Needs cmake, ninja, qemu-system-*, the edk2 firmware and the cross toolchain
# of each target (see cmake/toolchain-*.cmake); an architecture whose pieces are missing is skipped.
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-jit-boot
archs=("$@"); [ ${#archs[@]} -eq 0 ] && archs=(x86_64 aarch64 riscv64)
failed=0

boot() {   # boot <seconds> <qemu command...>: serial/debug output with the colour codes and the doubled characters of the two consoles removed
  local secs=$1; shift
  timeout "$secs" "$@" 2>&1 | python3 -c '
import sys,re
data=sys.stdin.buffer.read().decode("latin1").replace("\r","")
data=re.sub(r"\x1b\[[0-9;?]*[A-Za-z]","",data)
out=[]
for line in data.split("\n"):
    if len(line)>=4 and len(line)%2==0 and all(line[i]==line[i+1] for i in range(0,len(line),2)): line=line[::2]
    out.append(line)
print("\n".join(l for l in out if l.strip()))'
}

for arch in "${archs[@]}"; do
  echo "== JIT boot test: $arch"
  dir="$work/$arch"; mkdir -p "$dir"
  case $arch in
    x86_64) toolchain=x86_64-w64-mingw32-g++; qemu=qemu-system-x86_64 ;;
    aarch64) toolchain=clang; qemu=qemu-system-aarch64 ;;
    riscv64) toolchain=clang; qemu=qemu-system-riscv64 ;;
    *) echo "unknown architecture"; failed=1; continue ;;
  esac
  if ! command -v "$qemu" >/dev/null || ! command -v "$toolchain" >/dev/null || ! command -v ninja >/dev/null; then echo "skipped (needs $qemu, $toolchain, ninja)"; continue; fi
  if ! (cmake -S . -B "$dir" -G Ninja -DTCOS_ARCH=$arch >/dev/null && ninja -C "$dir" >"$dir/build.log" 2>&1); then echo "build failed (see $dir/build.log)"; failed=1; continue; fi
  case $arch in
    x86_64)
      code=/usr/share/edk2/x64/OVMF_CODE.4m.fd; vars=/usr/share/edk2/x64/OVMF_VARS.4m.fd
      [ -f "$code" ] || { echo "skipped (no OVMF)"; continue; }
      cp "$vars" "$dir/vars.fd"
      log=$(cd "$dir" && boot 120 qemu-system-x86_64 -machine q35 -smp 4 -m 4G -drive if=pflash,format=raw,readonly=on,file=$code -drive if=pflash,format=raw,file=vars.fd -drive format=raw,file=fat:rw:esp -vga none -device VGA,vgamem_mb=64 -display none -chardev stdio,id=dbg -device isa-debugcon,iobase=0x402,chardev=dbg -serial null) ;;
    aarch64)
      code=$(ls /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/edk2/aarch64/QEMU_EFI.fd 2>/dev/null | head -1)
      [ -n "$code" ] || { echo "skipped (no AAVMF)"; continue; }
      ninja -C "$dir" firmware-image >/dev/null 2>&1
      log=$(cd "$dir" && boot 60 qemu-system-aarch64 -machine virt -cpu max -smp 4 -m 2G -drive if=pflash,format=raw,readonly=on,file=AAVMF_CODE.img -drive format=raw,file=fat:rw:esp -nographic -serial mon:stdio -display none) ;;
    riscv64)
      code=/usr/share/edk2/riscv64/RISCV_VIRT_CODE.fd
      [ -f "$code" ] || { echo "skipped (no RISC-V edk2)"; continue; }
      ninja -C "$dir" firmware-image >/dev/null 2>&1
      log=$(cd "$dir" && boot 90 qemu-system-riscv64 -machine virt,pflash0=pflash0,pflash1=pflash1 -cpu rv64 -smp 4 -m 2G -blockdev node-name=pflash0,driver=file,read-only=on,filename=$code -blockdev node-name=pflash1,driver=file,filename=RISCV_VIRT_VARS.fd -drive if=none,id=esp,format=raw,file=fat:rw:esp -device virtio-blk-pci,drive=esp -nographic -serial mon:stdio -display none) ;;
  esac
  line=$(echo "$log" | grep "JIT:" | head -1)
  if echo "$line" | grep -q "self test passed"; then echo "$line"; else echo "FAILED: ${line:-no JIT line}"; echo "$log" | grep -i "panic\|fault" | head -3; failed=1; fi
done
exit $failed
