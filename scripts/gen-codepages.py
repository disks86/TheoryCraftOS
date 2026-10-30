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
"""Writes src/Windows/kernel32/codepage_data.inc: the tables of the code pages kernel32 converts (single byte ones, and the double byte ones 932, 936, 949 and 950), made with
the codecs of Python. For each page: the characters of the single bytes, for the double byte pages the lead bytes and the characters of the pairs, and the reverse (sorted by
character).

Usage: scripts/gen-codepages.py
"""
import os
import sys
import unicodedata

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SINGLE = [
    (437, "cp437", "OEM - United States"), (720, "cp720", "OEM - Arabic"), (737, "cp737", "OEM - Greek"), (775, "cp775", "OEM - Baltic"),
    (850, "cp850", "OEM - Multilingual Latin I"), (852, "cp852", "OEM - Latin II"), (855, "cp855", "OEM - Cyrillic"), (857, "cp857", "OEM - Turkish"),
    (858, "cp858", "OEM - Multilingual Latin I + Euro"), (860, "cp860", "OEM - Portuguese"), (861, "cp861", "OEM - Icelandic"), (862, "cp862", "OEM - Hebrew"),
    (863, "cp863", "OEM - Canadian French"), (864, "cp864", "OEM - Arabic"), (865, "cp865", "OEM - Nordic"), (866, "cp866", "OEM - Russian"),
    (869, "cp869", "OEM - Modern Greek"), (874, "cp874", "ANSI/OEM - Thai"), (1250, "cp1250", "ANSI - Central Europe"), (1251, "cp1251", "ANSI - Cyrillic"),
    (1252, "cp1252", "ANSI - Latin I"), (1253, "cp1253", "ANSI - Greek"), (1254, "cp1254", "ANSI - Turkish"), (1255, "cp1255", "ANSI - Hebrew"),
    (1256, "cp1256", "ANSI - Arabic"), (1257, "cp1257", "ANSI - Baltic"), (1258, "cp1258", "ANSI/OEM - Vietnamese"), (10000, "mac_roman", "MAC - Roman"),
    (10007, "mac_cyrillic", "MAC - Cyrillic"), (10029, "mac_latin2", "MAC - Latin 2"), (20127, "ascii", "US-ASCII"), (20866, "koi8_r", "Russian - KOI8"),
    (21866, "koi8_u", "Ukrainian - KOI8-U"), (28591, "iso8859_1", "ISO 8859-1 Latin 1"), (28592, "iso8859_2", "ISO 8859-2 Central European"),
    (28593, "iso8859_3", "ISO 8859-3 Latin 3"), (28594, "iso8859_4", "ISO 8859-4 Baltic"), (28595, "iso8859_5", "ISO 8859-5 Cyrillic"),
    (28596, "iso8859_6", "ISO 8859-6 Arabic"), (28597, "iso8859_7", "ISO 8859-7 Greek"), (28598, "iso8859_8", "ISO 8859-8 Hebrew"),
    (28599, "iso8859_9", "ISO 8859-9 Turkish"), (28603, "iso8859_13", "ISO 8859-13 Estonian"), (28605, "iso8859_15", "ISO 8859-15 Latin 9"),
]
DOUBLE = [(932, "cp932", "ANSI/OEM - Japanese Shift-JIS"), (936, "cp936", "ANSI/OEM - Simplified Chinese GBK"), (949, "cp949", "ANSI/OEM - Korean"),
          (950, "cp950", "ANSI/OEM - Traditional Chinese Big5")]


def single_byte(codec):
    table = []
    for b in range(256):
        try:
            ch = bytes([b]).decode(codec)
            table.append(ord(ch) if len(ch) == 1 and not 0xF8F0 <= ord(ch) <= 0xF8FF else 0xFFFD)
        except UnicodeDecodeError:
            # Windows maps the bytes a page leaves undefined in 0x80-0x9f to the control character of the same number
            table.append(b if 0x80 <= b <= 0x9F else 0xFFFD)
    return table


def double_byte(codec):
    singles = single_byte(codec)
    leads = []
    pairs = {}
    for lead in range(0x81, 0xFF):
        row = {}
        for trail in range(0x40, 0x100):
            try:
                ch = bytes([lead, trail]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                row[trail] = ord(ch)
        if row:
            leads.append(lead)
            pairs[lead] = row
    return singles, leads, pairs


def emit_words(out, name, words):
    out.append("static const WORD %s[%d] = {" % (name, len(words)))
    line = []
    for i, w in enumerate(words):
        line.append("0x%04x" % w)
        if len(line) == 16:
            out.append(",".join(line) + ",")
            line = []
    if line:
        out.append(",".join(line) + ",")
    out.append("};")


def emit_dwords(out, name, values):
    out.append("static const DWORD %s[%d] = {" % (name, len(values)))
    line = []
    for v in values:
        line.append("0x%08x" % v)
        if len(line) == 8:
            out.append(",".join(line) + ",")
            line = []
    if line:
        out.append(",".join(line) + ",")
    out.append("};")


def best_fit(seen):
    """Characters the page lacks, mapped to what their compatibility decomposition starts with, when the page has that (a plain letter for an accented one)."""
    result = {}
    for w in range(0x80, 0x10000):
        if w in seen or 0xD800 <= w < 0xE000:
            continue
        base = unicodedata.normalize("NFKD", chr(w))
        if base != chr(w) and ord(base[0]) in seen and ord(base[0]) < 0x80:
            result[w] = seen[ord(base[0])]
    return result


def main():
    out = ["/* Generated by scripts/gen-codepages.py: the code page tables of kernel32.dll (see there). Not compiled by itself. */", ""]
    entries = []
    for cp, codec, name in SINGLE:
        table = single_byte(codec)
        emit_words(out, "cp%d_to_wide" % cp, table)
        rev = sorted({(w << 8) | b for b, w in enumerate(table) if w != 0xFFFD and (w >= 0x80 or True)})
        # one byte per character: the lowest byte that has it
        seen = {}
        for b, w in enumerate(table):
            if w != 0xFFFD and w not in seen:
                seen[w] = b
        rev = sorted((w << 16) | b for w, b in seen.items())
        emit_dwords(out, "cp%d_from_wide" % cp, rev)
        fit = sorted((w << 16) | v for w, v in best_fit(seen).items())
        emit_dwords(out, "cp%d_best_fit" % cp, fit or [0])
        entries.append('  { %d, 0, "%s", cp%d_to_wide, NULL, NULL, cp%d_from_wide, %d, cp%d_best_fit, %d },' % (cp, name, cp, cp, len(rev), cp, len(fit)))
    for cp, codec, name in DOUBLE:
        singles, leads, pairs = double_byte(codec)
        emit_words(out, "cp%d_to_wide" % cp, singles)
        index = [0] * 256
        for i, lead in enumerate(leads):
            index[lead] = i + 1
        out.append("static const BYTE cp%d_lead[256] = {%s};" % (cp, ",".join(str(x) for x in index)))
        table = []
        for lead in leads:
            row = [0xFFFD] * 256
            for trail, w in pairs[lead].items():
                row[trail] = w
            table.extend(row)
        emit_words(out, "cp%d_double" % cp, table)
        seen = {}
        for b, w in enumerate(singles):
            if w != 0xFFFD and w not in seen:
                seen[w] = b
        for lead in leads:
            for trail, w in pairs[lead].items():
                if w not in seen:
                    seen[w] = (lead << 8) | trail
        rev = sorted((w << 16) | v for w, v in seen.items())
        emit_dwords(out, "cp%d_from_wide" % cp, rev)
        fit = sorted((w << 16) | v for w, v in best_fit(seen).items())
        emit_dwords(out, "cp%d_best_fit" % cp, fit or [0])
        entries.append('  { %d, 1, "%s", cp%d_to_wide, cp%d_lead, cp%d_double, cp%d_from_wide, %d, cp%d_best_fit, %d },' % (cp, name, cp, cp, cp, cp, len(rev), cp, len(fit)))
    out.append("")
    out.append("static const struct CodePageTable { WORD cp; BYTE double_byte; const char *name; const WORD *single; const BYTE *lead_index; const WORD *pairs; const DWORD *reverse; DWORD reverse_count; const DWORD *best_fit; DWORD best_fit_count; } g_code_pages[] = {")
    out.extend(entries)
    out.append("};")
    out.append("")
    path = os.path.join(root, "src", "Windows", "kernel32", "codepage_data.inc")
    with open(path, "w") as f:
        f.write("\n".join(out))
    print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))


main()
