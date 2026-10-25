#!/usr/bin/env python3
# TheoryCraftOS - a minimal UEFI operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Rewrites a position-independent RISC-V 64 ELF executable (linked at 0 with scripts/riscv64-uefi.ld) as a UEFI PE32+
application, the way EDK2's GenFw does: .text/.rodata/.data/.bss become PE sections at the same addresses, and the
ELF's R_RISCV_RELATIVE relocations become a PE base relocation table (IMAGE_REL_BASED_DIR64) after their addends are
written into the image, because the PE loader adds the load delta to the value already there.

usage: elf2efi.py input.elf output.efi
"""
import struct
import sys

R_RISCV_RELATIVE = 3
PAGE = 0x1000
FILE_ALIGN = 0x200
MACHINE_RISCV64 = 0x5064
SUBSYSTEM_EFI_APPLICATION = 10
IMAGE_REL_BASED_ABSOLUTE = 0
IMAGE_REL_BASED_DIR64 = 10
SECTIONS = [(".text", 0x60000020), (".rodata", 0x40000040), (".data", 0xC0000040), (".bss", 0xC0000080)]


def align(value, to):
    return (value + to - 1) // to * to


def fail(message):
    sys.exit("elf2efi: " + message)


def read_elf(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail("not a little-endian ELF64 file")
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        "<HHIQQQIHHHHHH", data, 16)
    if e_machine != 243:
        fail("not a RISC-V ELF file")
    if e_type != 3:
        fail("not a position-independent executable (link with -pie)")
    raw = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    names = data[raw[e_shstrndx][4]:raw[e_shstrndx][4] + raw[e_shstrndx][5]]
    sections = {}
    for (name, kind, flags, addr, offset, size, _, _, _, _) in raw:
        text = names[name:names.index(b"\0", name)].decode()
        sections[text] = dict(kind=kind, flags=flags, addr=addr, offset=offset, size=size)
    return data, e_entry, sections


def main():
    if len(sys.argv) != 3:
        fail("usage: elf2efi.py input.elf output.efi")
    data, entry, elf = read_elf(sys.argv[1])

    # Lay the sections out at their ELF addresses and apply the relative relocations to the file contents.
    present = [(name, flags) for (name, flags) in SECTIONS if name in elf and elf[name]["size"] > 0]
    if not present:
        fail("no loadable sections")
    for (name, _) in present:
        if elf[name]["addr"] % PAGE:
            fail(name + " does not start on a page boundary (wrong linker script?)")
    image_end = max(align(elf[name]["addr"] + elf[name]["size"], PAGE) for (name, _) in present)
    memory = bytearray(image_end)
    for (name, _) in present:
        s = elf[name]
        if s["kind"] != 8:  # SHT_NOBITS
            memory[s["addr"]:s["addr"] + s["size"]] = data[s["offset"]:s["offset"] + s["size"]]

    relocations = []
    rela = elf.get(".rela.dyn")
    if rela:
        for i in range(rela["size"] // 24):
            (offset, info, addend) = struct.unpack_from("<QQq", data, rela["offset"] + i * 24)
            if info & 0xFFFFFFFF != R_RISCV_RELATIVE:
                fail("unsupported dynamic relocation type %d at %#x" % (info & 0xFFFFFFFF, offset))
            if offset + 8 > image_end:
                fail("relocation outside the image: %#x" % offset)
            memory[offset:offset + 8] = struct.pack("<q", addend)
            relocations.append(offset)
    relocations.sort()

    # Base relocation blocks: one per 4 KiB page, padded to a multiple of 4 bytes.
    reloc = bytearray()
    i = 0
    while i < len(relocations):
        page = relocations[i] & ~(PAGE - 1)
        entries = []
        while i < len(relocations) and relocations[i] & ~(PAGE - 1) == page:
            entries.append((IMAGE_REL_BASED_DIR64 << 12) | (relocations[i] & (PAGE - 1)))
            i += 1
        if len(entries) % 2:
            entries.append(IMAGE_REL_BASED_ABSOLUTE << 12)
        reloc += struct.pack("<II", page, 8 + 2 * len(entries)) + struct.pack("<%dH" % len(entries), *entries)

    # (name, virtual address, virtual size, raw bytes, flags)
    out = []
    for (name, flags) in present:
        s = elf[name]
        raw_bytes = b"" if s["kind"] == 8 else bytes(memory[s["addr"]:s["addr"] + s["size"]])
        out.append((name, s["addr"], s["size"], raw_bytes, flags))
    reloc_rva = image_end
    out.append((".reloc", reloc_rva, len(reloc), bytes(reloc), 0x42000040))
    size_of_image = align(reloc_rva + max(len(reloc), 1), PAGE)

    headers_size = align(0x40 + 4 + 20 + 240 + 40 * len(out), FILE_ALIGN)
    file_pos = headers_size
    sections = []
    for (name, rva, vsize, raw_bytes, flags) in out:
        raw_size = align(len(raw_bytes), FILE_ALIGN)
        sections.append((name, rva, vsize, file_pos if raw_bytes else 0, raw_size if raw_bytes else 0, flags, raw_bytes))
        file_pos += raw_size if raw_bytes else 0

    size_code = sum(s[4] for s in sections if s[5] & 0x20)
    size_init = sum(s[4] for s in sections if s[5] & 0x40 and not s[5] & 0x20)
    size_uninit = sum(align(s[2], FILE_ALIGN) for s in sections if s[5] & 0x80)
    base_of_code = min(s[1] for s in sections if s[5] & 0x20)

    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    coff = struct.pack("<4sHHIIIHH", b"PE\0\0", MACHINE_RISCV64, len(sections), 0, 0, 0, 240, 0x0022)
    optional = struct.pack("<HBBIIIII", 0x20B, 0, 0, size_code, size_init, size_uninit, entry, base_of_code)
    optional += struct.pack("<QIIHHHHHHIIIIHHQQQQII", 0, PAGE, FILE_ALIGN, 0, 0, 0, 0, 0, 0, 0, size_of_image,
                            headers_size, 0, SUBSYSTEM_EFI_APPLICATION, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    directories = bytearray(16 * 8)
    struct.pack_into("<II", directories, 5 * 8, reloc_rva, len(reloc))
    optional += directories
    assert len(optional) == 240, len(optional)

    table = b"".join(
        struct.pack("<8sIIIIIIHHI", n.encode(), vsize, rva, raw_size, ptr, 0, 0, 0, 0, flags)
        for (n, rva, vsize, ptr, raw_size, flags, _) in sections)
    image = bytearray(dos + coff + optional + table)
    image += bytes(headers_size - len(image))
    for (_, _, _, _, raw_size, _, raw_bytes) in sections:
        if raw_bytes:
            image += raw_bytes + bytes(raw_size - len(raw_bytes))
    with open(sys.argv[2], "wb") as f:
        f.write(image)


main()
