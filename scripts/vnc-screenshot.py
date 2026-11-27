#!/usr/bin/env python3
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
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
#
# One frame of a QEMU display as a PNG, read over QEMU's VNC server (-vnc unix:<socket>). Under -display egl-headless the monitor's screendump has no
# surface to read, but the VNC server still has the scanout. Speaks just enough RFB 3.8: no authentication, raw encoding, 32-bit true colour.
# Usage: scripts/vnc-screenshot.py <unix socket> <out.png>
import socket
import struct
import sys
import zlib


def read_exact(s, n):
    data = bytearray()
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise EOFError("the VNC server closed the connection")
        data += chunk
    return bytes(data)


def write_png(path, width, height, rgb_rows):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: vnc-screenshot.py <unix socket> <out.png>")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(sys.argv[1])
    read_exact(s, 12)                      # the server's version
    s.sendall(b"RFB 003.008\n")
    count = read_exact(s, 1)[0]
    types = read_exact(s, count)
    if 1 not in types:
        sys.exit("the VNC server asks for authentication")
    s.sendall(b"\x01")                     # no authentication
    if struct.unpack(">I", read_exact(s, 4))[0] != 0:
        sys.exit("the VNC server refused the connection")
    s.sendall(b"\x01")                     # shared
    width, height = struct.unpack(">HH", read_exact(s, 4))
    read_exact(s, 16)                      # its pixel format
    name_length = struct.unpack(">I", read_exact(s, 4))[0]
    read_exact(s, name_length)
    # 32 bits per pixel, depth 24, little-endian, true colour, red at bit 16, green at 8, blue at 0.
    s.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
    s.sendall(struct.pack(">BxHi", 2, 1, 0))    # SetEncodings: raw only
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))   # a full (not incremental) update
    pixels = bytearray(width * height * 4)
    covered = 0
    while covered < width * height:
        kind = read_exact(s, 1)[0]
        if kind == 2:                      # bell
            continue
        if kind == 3:                      # cut text
            read_exact(s, 3)
            read_exact(s, struct.unpack(">I", read_exact(s, 4))[0])
            continue
        if kind == 1:                      # colour map entries
            read_exact(s, 1)
            read_exact(s, struct.unpack(">HH", read_exact(s, 4))[1] * 6)
            continue
        if kind != 0:
            sys.exit(f"unexpected VNC message {kind}")
        read_exact(s, 1)
        rects = struct.unpack(">H", read_exact(s, 2))[0]
        for _ in range(rects):
            x, y, w, h, encoding = struct.unpack(">HHHHi", read_exact(s, 12))
            if encoding == -223:           # desktop size: the frame comes again at the new size
                width, height = w, h
                pixels = bytearray(width * height * 4)
                covered = 0
                s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))
                continue
            if encoding != 0:
                sys.exit(f"unexpected encoding {encoding}")
            data = read_exact(s, w * h * 4)
            for row in range(h):
                start = ((y + row) * width + x) * 4
                pixels[start:start + w * 4] = data[row * w * 4:(row + 1) * w * 4]
            covered += w * h
    rows = []
    for row in range(height):
        line = pixels[row * width * 4:(row + 1) * width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3] = line[2::4]
        rgb[1::3] = line[1::4]
        rgb[2::3] = line[0::4]
        rows.append(bytes(rgb))
    write_png(sys.argv[2], width, height, rows)


if __name__ == "__main__":
    main()
