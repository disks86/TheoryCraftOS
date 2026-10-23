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
"""Builds the bundled font data (src/Font/FontData*.cpp) from OFL TrueType fonts.

Host-only developer tool, not part of the OS build; needs no packages. It subsets each source font to the
code points the browser needs, strips the TrueType hinting programs (the rasteriser does not run them), keeps
the 'kern' pairs between the kept glyphs, writes a fresh 'name' table (the Liberation fonts reserve the name
"Liberation", so the subsets are renamed "TCOS ...") and wraps the result as WOFF 1.0 to keep the binary small.

Usage: scripts/make_fonts.py [--liberation DIR] [--noto DIR] [--out DIR]
"""
import argparse
import struct
import zlib
import os

BASIC = [(0x20, 0x7E), (0xA0, 0xFF)]
LATIN_EXT = [(0x100, 0x17F), (0x192, 0x192), (0x1A0, 0x1A1), (0x1AF, 0x1B0), (0x2C6, 0x2C7), (0x2D8, 0x2DD)]
GREEK_CYR = [(0x370, 0x3FF), (0x400, 0x4FF)]
PUNCT = [(0x2010, 0x2027), (0x2030, 0x205E), (0x20A0, 0x20BF), (0x2100, 0x2122), (0x2126, 0x2126), (0x2153, 0x215E),
         (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x2311), (0x2500, 0x257F), (0x2580, 0x259F), (0x25A0, 0x25FF),
         (0x2600, 0x2603), (0x2605, 0x2606), (0x2660, 0x2667), (0x2713, 0x2714), (0xFB01, 0xFB02), (0xFFFD, 0xFFFD)]
FALLBACK = [(0x20, 0x7E), (0xA0, 0x24F), (0x250, 0x2FF), (0x300, 0x36F), (0x370, 0x52F), (0x1E00, 0x1EFF),
            (0x2000, 0x206F), (0x2070, 0x209F), (0x20A0, 0x20CF), (0x2100, 0x214F), (0x2150, 0x218F), (0x2190, 0x23FF),
            (0x2400, 0x2426), (0x2460, 0x24FF), (0x2500, 0x25FF), (0x2600, 0x2605), (0x2660, 0x2667), (0x2700, 0x27BF),
            (0xFB00, 0xFB06), (0xFFFD, 0xFFFD)]


def u16(d, o): return struct.unpack_from(">H", d, o)[0]
def i16(d, o): return struct.unpack_from(">h", d, o)[0]
def u32(d, o): return struct.unpack_from(">I", d, o)[0]


def parse_sfnt(d):
    n = u16(d, 4)
    tables = {}
    for i in range(n):
        tag = d[12 + 16 * i:16 + 16 * i].decode("latin1")
        off, ln = u32(d, 20 + 16 * i), u32(d, 24 + 16 * i)
        tables[tag] = d[off:off + ln]
    return tables


def cmap_map(cm):
    """code point -> glyph id, from the best Unicode subtable."""
    best = None
    for i in range(u16(cm, 2)):
        plat, enc, off = u16(cm, 4 + 8 * i), u16(cm, 6 + 8 * i), u32(cm, 8 + 8 * i)
        fmt = u16(cm, off)
        rank = 3 if (plat, enc, fmt) == (3, 10, 12) else 2 if (plat, enc, fmt) == (3, 1, 4) else 1 if fmt in (4, 12) and plat == 0 else 0
        if rank and (best is None or rank > best[0]): best = (rank, off, fmt)
    _, off, fmt = best
    m = {}
    if fmt == 12:
        for g in range(u32(cm, off + 12)):
            s, e, gl = struct.unpack_from(">III", cm, off + 16 + 12 * g)
            for c in range(s, e + 1): m[c] = gl + c - s
    else:
        segx2 = u16(cm, off + 6)
        seg = segx2 // 2
        ends, starts = off + 14, off + 16 + segx2
        deltas, ros = starts + segx2, starts + 2 * segx2
        for k in range(seg):
            e, s, dl, ro = u16(cm, ends + 2 * k), u16(cm, starts + 2 * k), u16(cm, deltas + 2 * k), u16(cm, ros + 2 * k)
            for c in range(s, e + 1):
                if c == 0xFFFF: continue
                if ro == 0: g = (c + dl) & 0xFFFF
                else:
                    g = u16(cm, ros + 2 * k + ro + 2 * (c - s))
                    if g: g = (g + dl) & 0xFFFF
                if g: m[c] = g
    return m


def glyph_slices(t):
    loca, glyf = t["loca"], t["glyf"]
    n = u16(t["maxp"], 4)
    long = i16(t["head"], 50)
    offs = [u32(loca, 4 * i) if long else 2 * u16(loca, 2 * i) for i in range(n + 1)]
    return [glyf[offs[i]:offs[i + 1]] for i in range(n)]


def components(g):
    """(offset of the glyph-index field, glyph index) for each component of a composite glyph."""
    out, p = [], 10
    while True:
        flags, gi = u16(g, p), u16(g, p + 2)
        out.append((p + 2, gi, flags, p))
        p += 4 + (4 if flags & 1 else 2)
        if flags & 8: p += 2
        elif flags & 0x40: p += 4
        elif flags & 0x80: p += 8
        if not flags & 0x20: return out, p


def rewrite_glyph(g, remap):
    if not g: return b""
    nc = i16(g, 0)
    if nc >= 0:
        ilen = u16(g, 10 + 2 * nc)
        new = g[:10 + 2 * nc] + b"\0\0" + g[12 + 2 * nc + ilen:]
    else:
        comps, end = components(g)
        new = bytearray(g[:10])
        for k, (gp, gi, flags, p) in enumerate(comps):
            nxt = comps[k + 1][3] if k + 1 < len(comps) else end
            piece = bytearray(g[p:nxt])
            struct.pack_into(">H", piece, 0, flags & ~0x100)
            struct.pack_into(">H", piece, 2, remap[gi])
            new += piece
    while len(new) % 4: new += b"\0"
    return bytes(new)


def kern_pairs(t):
    k = t.get("kern")
    pairs = {}
    if not k or u16(k, 0) != 0: return pairs
    p = 4
    for _ in range(u16(k, 2)):
        ln, cov = u16(k, p + 2), u16(k, p + 4)
        if cov >> 8 == 0 and cov & 1 and not cov & 6:
            for j in range(u16(k, p + 6)):
                l, r, v = struct.unpack_from(">HHh", k, p + 14 + 6 * j)
                pairs[(l, r)] = v
        p += ln
    return pairs


def build_kern(pairs):
    items = sorted(pairs.items())
    if not items: return None
    out = struct.pack(">HH", 0, 0)
    count = 0
    for s in range(0, len(items), 8000):
        chunk = items[s:s + 8000]
        n = len(chunk)
        sr = 1
        while sr * 2 <= n: sr *= 2
        es = sr.bit_length() - 1
        out += struct.pack(">HHHHHHH", 0, 14 + 6 * n, 1, n, sr * 6, es, n * 6 - sr * 6)
        for (l, r), v in chunk: out += struct.pack(">HHh", l, r, v)
        count += 1
    return struct.pack(">HH", 0, count) + out[4:]


def build_cmap(m):
    """A format 4 (BMP) and a format 12 subtable."""
    bmp = sorted((c, g) for c, g in m.items() if c < 0xFFFF)
    segs = []
    for c, g in bmp:
        if segs and segs[-1][1] + 1 == c and segs[-1][2] + (c - segs[-1][0]) == g:
            segs[-1][1] = c
        else:
            segs.append([c, c, g])
    segs.append([0xFFFF, 0xFFFF, 0])
    n = len(segs)
    sr = 1
    while sr * 2 <= n: sr *= 2
    f4 = struct.pack(">HHHHHHH", 4, 16 + 8 * n, 0, n * 2, sr * 2, sr.bit_length() - 1, n * 2 - sr * 2)
    f4 += b"".join(struct.pack(">H", s[1]) for s in segs) + b"\0\0"
    f4 += b"".join(struct.pack(">H", s[0]) for s in segs)
    f4 += b"".join(struct.pack(">H", (s[2] - s[0]) & 0xFFFF) if s[0] != 0xFFFF else struct.pack(">H", 1) for s in segs)
    f4 += b"\0\0" * n
    groups = []
    for c, g in sorted(m.items()):
        if groups and groups[-1][1] + 1 == c and groups[-1][2] + (c - groups[-1][0]) == g:
            groups[-1][1] = c
        else:
            groups.append([c, c, g])
    f12 = struct.pack(">HHIII", 12, 0, 16 + 12 * len(groups), 0, len(groups))
    f12 += b"".join(struct.pack(">III", *g) for g in groups)
    head = struct.pack(">HH", 0, 2) + struct.pack(">HHI", 3, 1, 20) + struct.pack(">HHI", 3, 10, 20 + len(f4))
    return head + f4 + f12


def build_name(family, style, copyright_text, license_text, license_url):
    full = family + (" " + style if style != "Regular" else "")
    ps = full.replace(" ", "-")
    recs = [(0, copyright_text), (1, family if style in ("Regular", "Bold", "Italic", "Bold Italic") else family), (2, style),
            (3, ps + ";subset"), (4, full), (5, "Version 1.0"), (6, ps), (13, license_text), (14, license_url)]
    strs, body = [], b""
    for nid, text in recs:
        enc = text.encode("utf-16-be")
        strs.append((nid, len(enc), len(body)))
        body += enc
    out = struct.pack(">HHH", 0, len(recs), 6 + 12 * len(recs))
    for nid, ln, off in strs: out += struct.pack(">HHHHHH", 3, 1, 0x409, nid, ln, off)
    return out + body


def checksum(b):
    b = b + b"\0" * (-len(b) % 4)
    return sum(struct.unpack(">%dI" % (len(b) // 4), b)) & 0xFFFFFFFF


def subset(path, ranges, family, style, copyright_text, license_text, license_url, want_kern=True):
    d = open(path, "rb").read()
    t = parse_sfnt(d)
    cm = cmap_map(t["cmap"])
    keep = {c: g for c, g in cm.items() if any(a <= c <= b for a, b in ranges)}
    glyphs = glyph_slices(t)
    need = {0} | set(keep.values())
    todo = list(need)
    while todo:
        g = glyphs[todo.pop()]
        if g and i16(g, 0) < 0:
            for _, gi, _, _ in components(g)[0]:
                if gi not in need: need.add(gi); todo.append(gi)
    order = sorted(need)
    remap = {g: i for i, g in enumerate(order)}
    n = len(order)
    glyf, offs = b"", []
    for g in order:
        offs.append(len(glyf))
        glyf += rewrite_glyph(glyphs[g], remap)
    offs.append(len(glyf))
    long = len(glyf) >= 0x20000
    loca = b"".join(struct.pack(">I" if long else ">H", o if long else o // 2) for o in offs)
    hhea = bytearray(t["hhea"])
    struct.pack_into(">H", hhea, 34, n)
    hmtx = b""
    nh = u16(t["hhea"], 34)
    for g in order:
        adv = u16(t["hmtx"], 4 * min(g, nh - 1))
        lsb = i16(t["hmtx"], 4 * g + 2) if g < nh else i16(t["hmtx"], 4 * nh + 2 * (g - nh))
        hmtx += struct.pack(">Hh", adv, lsb)
    head = bytearray(t["head"])
    struct.pack_into(">I", head, 8, 0)
    struct.pack_into(">h", head, 50, 1 if long else 0)
    maxp = bytearray(t["maxp"][:32])
    struct.pack_into(">H", maxp, 4, n)
    out = {"head": bytes(head), "hhea": bytes(hhea), "maxp": bytes(maxp), "hmtx": hmtx, "cmap": build_cmap({c: remap[g] for c, g in keep.items()}),
           "loca": loca, "glyf": glyf, "OS/2": t["OS/2"], "name": build_name(family, style, copyright_text, license_text, license_url)}
    if want_kern:
        kp = {(remap[l], remap[r]): v for (l, r), v in kern_pairs(t).items() if l in remap and r in remap}
        k = build_kern(kp)
        if k: out["kern"] = k
    return out, n, len(keep)


def to_woff(tables, flavor=0x00010000):
    tags = sorted(tables)
    data = []
    for tag in tags:
        raw = tables[tag]
        comp = zlib.compress(raw, 9)
        data.append((tag, raw, comp if len(comp) < len(raw) else raw))
    off = 44 + 20 * len(tags)
    entries, body = b"", b""
    total = 12 + 16 * len(tags)
    for tag, raw, c in data:
        entries += struct.pack(">4sIIII", tag.encode("latin1"), off + len(body), len(c), len(raw), checksum(raw))
        body += c + b"\0" * (-len(c) % 4)
        total += len(raw) + (-len(raw) % 4)
    length = off + len(body)
    hdr = struct.pack(">4sIIHHIHHIIIII", b"wOFF", flavor, length, len(tags), 0, total, 1, 0, 0, 0, 0, 0, 0)
    return hdr + entries + body


def emit(path, header_comment, fn_name, faces):
    with open(path, "w") as f:
        f.write("""/*
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

""")
        f.write(header_comment + "\n// GENERATED by scripts/make_fonts.py - do not edit. Subsets of the fonts named above, stored as WOFF 1.0.\n")
        f.write('#include "Font/BundledFonts.hpp"\n\nnamespace font {\n\nnamespace {\n')
        for name, blob, _, _, _ in faces:
            f.write("constexpr uint8_t %s[%d] = {\n" % (name, len(blob)))
            for i in range(0, len(blob), 24):
                f.write("  " + ",".join("0x%02x" % b for b in blob[i:i + 24]) + ",\n")
            f.write("};\n")
        f.write("}  // namespace\n\nArrayView<const BundledFontInfo> %s() {\n  static const BundledFontInfo kFaces[] = {\n" % fn_name)
        for name, _, family, weight, italic in faces:
            f.write('      {"%s", %d, %s, ConstByteView(%s)},\n' % (family, weight, "true" if italic else "false", name))
        f.write("  };\n  return ArrayView<const BundledFontInfo>(kFaces);\n}\n\n}  // namespace font\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--liberation", default="/usr/share/fonts/liberation")
    ap.add_argument("--noto", default="/usr/share/fonts/noto")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "src", "Font"))
    a = ap.parse_args()
    lib_copy = "Digitized data copyright (c) 2010 Google Corporation. Copyright (c) 2012 Red Hat, Inc. Liberation is a trademark of Red Hat, Inc."
    noto_copy = "Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic). Noto is a trademark of Google LLC."
    ofl = "This Font Software is licensed under the SIL Open Font License, Version 1.1."
    url = "https://openfontlicense.org"
    styles = [("Regular", 400, False, "Regular"), ("Bold", 700, False, "Bold"), ("Italic", 400, True, "Italic"), ("BoldItalic", 700, True, "Bold Italic")]
    families = [("Sans", "TCOS Sans", "LiberationSans", BASIC + LATIN_EXT + GREEK_CYR + PUNCT, "Liberation Sans 2.1"),
                ("Serif", "TCOS Serif", "LiberationSerif", BASIC + LATIN_EXT + GREEK_CYR + PUNCT, "Liberation Serif 2.1"),
                ("Mono", "TCOS Mono", "LiberationMono", BASIC + LATIN_EXT + GREEK_CYR + PUNCT, "Liberation Mono 2.1")]
    total = 0
    for key, fam, src, ranges, srcname in families:
        faces = []
        for fname, weight, italic, sname in styles:
            tabs, n, nc = subset("%s/%s-%s.ttf" % (a.liberation, src, fname), ranges, fam, sname, lib_copy, ofl, url)
            woff = to_woff(tabs)
            total += len(woff)
            print(fam, sname, "glyphs", n, "codepoints", nc, "woff bytes", len(woff))
            faces.append(("k%s%s" % (key, fname), woff, fam, weight, italic))
        emit(os.path.join(a.out, "FontData%s.cpp" % key), "// %s (Regular, Bold, Italic, Bold Italic), Copyright (c) 2010 Google Corporation and (c) 2012 Red Hat, Inc.,\n// SIL Open Font License 1.1 (see LICENSE-Liberation.txt), renamed \"%s\" as the OFL reserved font name requires." % (srcname, fam),
             "bundled_%s_faces" % key.lower(), faces)
    faces = []
    for fname, weight, italic, sname in [("Regular", 400, False, "Regular")]:
        tabs, n, nc = subset("%s/NotoSans-%s.ttf" % (a.noto, fname), FALLBACK, "TCOS Fallback", sname, noto_copy, ofl, url, want_kern=False)
        woff = to_woff(tabs)
        total += len(woff)
        print("TCOS Fallback", sname, "glyphs", n, "codepoints", nc, "woff bytes", len(woff))
        faces.append(("kFallback%s" % fname, woff, "TCOS Fallback", weight, italic))
    emit(os.path.join(a.out, "FontDataFallback.cpp"), "// Noto Sans Regular 2.015, Copyright 2022 The Noto Project Authors, SIL Open Font License 1.1 (see LICENSE-Noto.txt).",
         "bundled_fallback_faces", faces)
    print("total woff bytes", total)


if __name__ == "__main__":
    main()
