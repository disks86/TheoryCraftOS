#!/bin/bash
# Runs the differential test of the AtomBIOS interpreter against the Linux driver's (see atom_diff.cpp).
#
#   run.sh path/to/dir-with-atom.c-atom.h-atombios.h [first-seed] [count]
#
# The directory holds atom.c, atom.h and atombios.h of the Linux kernel (tag v6.12, drivers/gpu/drm/amd/amdgpu/ and drivers/gpu/drm/amd/include/). They are compiled in user space with
# the stubs in this directory (shim.h and the headers the kernel would provide), together with this OS's AtomBios, under the address and undefined behaviour sanitizers.
set -e
kernel="$1"
[ -f "$kernel/atom.c" ] || { echo "usage: $0 dir-with-atom.c [first] [count]"; exit 2; }
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
out="$(mktemp -d)"
# the kernel files and the stubs side by side (a quoted include looks next to the including file first, and the kernel directory may hold headers of the real tree)
mkdir "$out/src"
cp "$kernel/atom.c" "$kernel/atom.h" "$kernel/atombios.h" "$out/src/"
cp -r "$here"/shim.h "$here"/atom-types.h "$here"/atom-names.h "$here"/atom-bits.h "$here"/atomfirmware.h "$here"/ObjectID.h "$here"/pptable.h "$here"/amdgpu.h "$here"/linux "$here"/drm "$out/src/"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1"
gcc -std=gnu11 $san -w -include "$out/src/shim.h" -I"$out/src" -c "$here/linux_side.c" -o "$out/linux_side.o"
g++ -std=c++20 $san -I"$root/src" -I"$root/tests" -c "$here/atom_diff.cpp" -o "$out/atom_diff.o"
g++ -std=c++20 $san -I"$root/src" -c "$root/src/Hardware/GraphicsProcessingUnit/AMD/AtomBios.cpp" -o "$out/AtomBios.o"
g++ -std=c++20 $san -I"$root/src" -c "$root/tests/KernelStubs.cpp" -o "$out/KernelStubs.o"
g++ $san "$out/atom_diff.o" "$out/AtomBios.o" "$out/linux_side.o" "$out/KernelStubs.o" -o "$out/atom_diff"
"$out/atom_diff" "${2:-1}" "${3:-2000}"
