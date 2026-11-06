#!/usr/bin/env bash
# Boots the kernel in QEMU with the emulated network controllers that the consumer NIC drivers also run: `e1000e` (Intel 82574L, the e1000e
# driver), `igb` (Intel 82576, the igb/igc driver) and `usb-net` on xHCI (CDC-ECM, after skipping its RNDIS configuration). Every netdev's
# traffic is captured with filter-dump, and a NIC passes when its capture holds a whole DHCP exchange (Discover, Offer, Request, ACK): the
# driver sent and received. The guest's UDP checksums must be right (the PCI drivers offload them), and the two PCI controllers must run on
# MSI-X interrupts with several RSS receive queues. Needs qemu-system-x86_64, OVMF, tcpdump and a configured build directory (default: build). Not part of
# run_host_tests.sh because it boots a VM.
# Usage: tests/nic_qemu.sh [build-dir] [seconds]
set -uo pipefail
cd "$(dirname "$0")/.."
build=${1:-build}
seconds=${2:-80}
work=${TMPDIR:-/tmp}/tcos-nic-qemu
mkdir -p "$work"
rm -f "$work"/n*.pcap "$work/debugcon.log"
for tool in qemu-system-x86_64 tcpdump; do command -v $tool >/dev/null || { echo "skipped (needs $tool)"; exit 0; }; done
code=$(grep -m1 '^OVMF_CODE:' "$build/CMakeCache.txt" | cut -d= -f2)
vars=$(grep -m1 '^OVMF_VARS:' "$build/CMakeCache.txt" | cut -d= -f2)
[ -n "$code" ] && [ -n "$vars" ] || { echo "skipped (no OVMF in $build/CMakeCache.txt)"; exit 0; }
cmake --build "$build" --target esp -j"$(nproc)" >/dev/null || exit 1
cp "$vars" "$work/vars.fd" && chmod u+w "$work/vars.fd"
accel=()
[ -r /dev/kvm ] && [ -w /dev/kvm ] && accel=(-accel kvm -cpu host)

timeout "$seconds" qemu-system-x86_64 "${accel[@]}" -machine q35 -smp 4 -m 4G \
  -netdev user,id=n1 -object filter-dump,id=f1,netdev=n1,file="$work/n1.pcap" -device e1000e,netdev=n1,mac=52:54:00:00:00:e1 \
  -netdev user,id=n2 -object filter-dump,id=f2,netdev=n2,file="$work/n2.pcap" -device igb,netdev=n2,mac=52:54:00:00:00:1b \
  -device qemu-xhci,id=xhci -netdev user,id=n3 -object filter-dump,id=f3,netdev=n3,file="$work/n3.pcap" -device usb-net,netdev=n3,bus=xhci.0,mac=52:54:00:00:00:ec \
  -vga std -display none \
  -drive if=pflash,format=raw,readonly=on,file="$code" -drive if=pflash,format=raw,file="$work/vars.fd" \
  -drive format=raw,file=fat:rw:"$build/esp" \
  -chardev file,id=dbg,path="$work/debugcon.log" -device isa-debugcon,iobase=0x402,chardev=dbg -serial null >/dev/null 2>"$work/qemu.err"

grep -a "Network:" "$work/debugcon.log"
failed=0
for n in "n1 e1000e 52:54:00:00:00:e1 82574L" "n2 igb 52:54:00:00:00:1b 82576" "n3 usb-net 52:54:00:00:00:ec USB"; do
  set -- $n
  steps=$(tcpdump -nn -v -r "$work/$1.pcap" 2>/dev/null | grep -aoE 'DHCP-Message \(53\), length 1: [A-Za-z]+' | awk '{print $NF}' | tr '\n' ' ')
  # the guest's own UDP datagrams (DHCP), with the checksum tcpdump recomputes: offloaded or not, it has to be right
  sent=$(tcpdump -nn -vv -e -r "$work/$1.pcap" "ether src $3 and udp" 2>/dev/null)
  good=$(grep -ac "udp sum ok" <<<"$sent"); bad=$(grep -acE "bad udp cksum" <<<"$sent")
  if [[ "$steps" != *"Discover Offer Request ACK"* ]]; then echo "$2: FAILED (DHCP saw: ${steps:-nothing})"; failed=1; continue; fi
  if [ "$bad" -ne 0 ] || [ "$good" -eq 0 ]; then echo "$2: FAILED ($good right and $bad wrong UDP checksums from the guest)"; failed=1; continue; fi
  # the PCI controllers take MSI (QEMU's e1000e and igb have it); the USB adapter is polled
  mode=$(grep -a "Network:   .*$4" "$work/debugcon.log" | head -1)
  if [ "$4" != USB ] && [[ "$mode" != *"interrupts (MSI"* ]]; then echo "$2: FAILED (not interrupt driven: ${mode:-no line})"; failed=1; continue; fi
  if [ "$4" != USB ] && [[ "$mode" != *"MSI-X"* ]]; then echo "$2: FAILED (no MSI-X receive queues: $mode)"; failed=1; continue; fi
  echo "$2: DHCP done, $good UDP checksums right${mode:+, ${mode##*: }}"
done
exit $failed
