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
"""Writes src/Windows/kernel32/locale_data.inc: the locales of kernel32.dll (the Windows locale identifiers and names, and the LOCALE_ values of each), made from the CLDR data
of the ICU library on the build machine (through ctypes), with the values of Windows for the locales it is known for (en-US and the invariant locale).

Usage: scripts/gen-locales.py [ICU version suffix, 78 by default]
"""
import ctypes
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V = sys.argv[1] if len(sys.argv) > 1 else "78"
uc = ctypes.CDLL("libicuuc.so.%s" % V)
i18n = ctypes.CDLL("libicui18n.so.%s" % V)


def fn(lib, name, restype, *argtypes):
    f = getattr(lib, "%s_%s" % (name, V))
    f.restype = restype
    f.argtypes = list(argtypes)
    return f


ERR = ctypes.POINTER(ctypes.c_int)
c_char_p, c_int32, c_void_p, c_uint16 = ctypes.c_char_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint16
uloc_getAvailable = fn(uc, "uloc_getAvailable", c_char_p, c_int32)
uloc_countAvailable = fn(uc, "uloc_countAvailable", c_int32)
uloc_getLCID = fn(uc, "uloc_getLCID", ctypes.c_uint32, c_char_p)
uloc_getLocaleForLCID = fn(uc, "uloc_getLocaleForLCID", c_int32, ctypes.c_uint32, c_char_p, c_int32, ERR)
uloc_getISO3Language = fn(uc, "uloc_getISO3Language", c_char_p, c_char_p)
uloc_getISO3Country = fn(uc, "uloc_getISO3Country", c_char_p, c_char_p)
uloc_getLanguage = fn(uc, "uloc_getLanguage", c_int32, c_char_p, c_char_p, c_int32, ERR)
uloc_getCountry = fn(uc, "uloc_getCountry", c_int32, c_char_p, c_char_p, c_int32, ERR)
uloc_getScript = fn(uc, "uloc_getScript", c_int32, c_char_p, c_char_p, c_int32, ERR)
uloc_addLikelySubtags = fn(uc, "uloc_addLikelySubtags", c_int32, c_char_p, c_char_p, c_int32, ERR)
uloc_getDisplayLanguage = fn(uc, "uloc_getDisplayLanguage", c_int32, c_char_p, c_char_p, ctypes.POINTER(c_uint16), c_int32, ERR)
uloc_getDisplayCountry = fn(uc, "uloc_getDisplayCountry", c_int32, c_char_p, c_char_p, ctypes.POINTER(c_uint16), c_int32, ERR)
uloc_getDisplayScript = fn(uc, "uloc_getDisplayScript", c_int32, c_char_p, c_char_p, ctypes.POINTER(c_uint16), c_int32, ERR)
ucurr_forLocale = fn(i18n, "ucurr_forLocale", c_int32, c_char_p, ctypes.POINTER(c_uint16), c_int32, ERR)
ucurr_getName = fn(uc, "ucurr_getName", ctypes.POINTER(c_uint16), ctypes.POINTER(c_uint16), c_char_p, c_int32, ctypes.POINTER(ctypes.c_int8), ctypes.POINTER(c_int32), ERR) \
    if hasattr(uc, "ucurr_getName_%s" % V) else None
udat_open = fn(i18n, "udat_open", c_void_p, c_int32, c_int32, c_char_p, ctypes.POINTER(c_uint16), c_int32, ctypes.POINTER(c_uint16), c_int32, ERR)
udat_toPattern = fn(i18n, "udat_toPattern", c_int32, c_void_p, ctypes.c_int8, ctypes.POINTER(c_uint16), c_int32, ERR)
udat_getSymbols = fn(i18n, "udat_getSymbols", c_int32, c_void_p, c_int32, c_int32, ctypes.POINTER(c_uint16), c_int32, ERR)
udat_close = fn(i18n, "udat_close", None, c_void_p)
udatpg_open = fn(i18n, "udatpg_open", c_void_p, c_char_p, ERR)
udatpg_getBestPattern = fn(i18n, "udatpg_getBestPattern", c_int32, c_void_p, ctypes.POINTER(c_uint16), c_int32, ctypes.POINTER(c_uint16), c_int32, ERR)
unum_open = fn(i18n, "unum_open", c_void_p, c_int32, ctypes.POINTER(c_uint16), c_int32, c_char_p, c_void_p, ERR)
unum_toPattern = fn(i18n, "unum_toPattern", c_int32, c_void_p, ctypes.c_int8, ctypes.POINTER(c_uint16), c_int32, ERR)
unum_getSymbol = fn(i18n, "unum_getSymbol", c_int32, c_void_p, c_int32, ctypes.POINTER(c_uint16), c_int32, ERR)
unum_close = fn(i18n, "unum_close", None, c_void_p)
ucal_open = fn(i18n, "ucal_open", c_void_p, ctypes.POINTER(c_uint16), c_int32, c_char_p, c_int32, ERR)
ucal_getAttribute = fn(i18n, "ucal_getAttribute", c_int32, c_void_p, c_int32)
ucal_close = fn(i18n, "ucal_close", None, c_void_p)
ulocdata_open = fn(i18n, "ulocdata_open", c_void_p, c_char_p, ERR)
ulocdata_getMeasurementSystem = fn(i18n, "ulocdata_getMeasurementSystem", c_int32, c_char_p, ERR)


def ustr(call, *args, size=256):
    """Calls an ICU function that fills a UChar buffer; returns the string."""
    buf = (c_uint16 * size)()
    err = ctypes.c_int(0)
    n = call(*args, buf, size, ctypes.byref(err))
    if err.value > 0 and err.value != -124:   # -124 is "string not terminated"
        return ""
    return "".join(chr(buf[i]) for i in range(max(0, min(n, size))))


def cstr(call, *args, size=64):
    buf = ctypes.create_string_buffer(size)
    err = ctypes.c_int(0)
    call(*args, buf, size, ctypes.byref(err))
    return buf.value.decode()


def jstr(s):
    return (c_uint16 * (len(s) + 1))(*[ord(c) for c in s], 0) if s else None


# ---- Windows constants --------------------------------------------------------------------------------------------------------------------------------------

header = open("/usr/x86_64-w64-mingw32/include/winnls.h").read()
CONST = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define (LOCALE_\w+)\s+(0x[0-9a-fA-F]+)\b", header)}
BASE_TYPES = sorted({v for k, v in CONST.items() if v < 0x2000 and not k.startswith(("LOCALE_ALL", "LOCALE_WINDOWS", "LOCALE_SUPPLEMENTAL", "LOCALE_ALTERNATE", "LOCALE_REPLACEMENT",
                                                                                      "LOCALE_NEUTRALDATA", "LOCALE_SPECIFICDATA", "LOCALE_ALLOW", "LOCALE_RETURN", "LOCALE_USE",
                                                                                      "LOCALE_NOUSER", "LOCALE_ENUMPROC"))})
NAMES = {}
for k, v in CONST.items():
    NAMES.setdefault(v, k)

# ---- the locales ---------------------------------------------------------------------------------------------------------------------------------------------

# Windows names are the BCP 47 tags; ICU uses a script for a few and the Windows tag leaves the script out for Chinese
def windows_name(icu):
    parts = icu.split("_")
    if parts[0] == "zh":
        if len(parts) == 3 and parts[1] in ("Hans", "Hant"):
            parts = [parts[0], parts[2]]
    return "-".join(parts)


def list_locales():
    seen = {}
    for i in range(uloc_countAvailable()):
        name = uloc_getAvailable(i).decode()
        lcid = uloc_getLCID(name.encode())
        if not lcid or lcid == 0x7F:
            continue
        canonical = cstr(uloc_getLocaleForLCID, lcid, size=64) if False else None
        err = ctypes.c_int(0)
        buf = ctypes.create_string_buffer(64)
        uloc_getLocaleForLCID(lcid, buf, 64, ctypes.byref(err))
        if buf.value.decode() != name:
            continue
        seen[lcid] = name
    # ICU answers some of the Windows locales with another script or none
    for lcid, name in {0x804: "zh_Hans_CN", 0x404: "zh_Hant_TW", 0xC04: "zh_Hant_HK", 0x1004: "zh_Hans_SG", 0x1404: "zh_Hant_MO", 0xC0A: "es_ES", 0x40A: "es_ES"}.items():
        if lcid == 0x40A:
            continue
        seen[lcid] = name
    return seen


def primary_lang(lcid):
    return lcid & 0x3FF


OEM = {  # language -> (ansi code page, oem code page, mac code page)
    "en": (1252, 437, 10000), "de": (1252, 850, 10000), "fr": (1252, 850, 10000), "es": (1252, 850, 10000), "it": (1252, 850, 10000), "pt": (1252, 850, 10000),
    "nl": (1252, 850, 10000), "sv": (1252, 850, 10000), "da": (1252, 850, 10000), "nb": (1252, 850, 10000), "nn": (1252, 850, 10000), "no": (1252, 850, 10000),
    "fi": (1252, 850, 10000), "is": (1252, 850, 10079), "ru": (1251, 866, 10007), "uk": (1251, 866, 10017), "be": (1251, 866, 10007), "bg": (1251, 866, 10007),
    "pl": (1250, 852, 10029), "cs": (1250, 852, 10029), "sk": (1250, 852, 10029), "hu": (1250, 852, 10029), "ro": (1250, 852, 10010), "sl": (1250, 852, 10029),
    "hr": (1250, 852, 10082), "sr": (1251, 855, 10007), "el": (1253, 737, 10006), "tr": (1254, 857, 10081), "he": (1255, 862, 10005), "ar": (1256, 720, 10004),
    "fa": (1256, 720, 10004), "ur": (1256, 720, 10004), "th": (874, 874, 10021), "ja": (932, 932, 10001), "ko": (949, 949, 10003), "vi": (1258, 1258, 10000),
    "lt": (1257, 775, 10029), "lv": (1257, 775, 10029), "et": (1257, 775, 10029), "hi": (0, 1, 0), "ka": (0, 1, 0), "ta": (0, 1, 0), "te": (0, 1, 0), "mr": (0, 1, 0),
    "bn": (0, 1, 0), "gu": (0, 1, 0), "kn": (0, 1, 0), "ml": (0, 1, 0), "pa": (0, 1, 0), "af": (1252, 850, 10000), "ca": (1252, 850, 10000), "eu": (1252, 850, 10000),
    "gl": (1252, 850, 10000), "id": (1252, 850, 10000), "ms": (1252, 850, 10000), "sw": (1252, 437, 10000), "az": (1254, 857, 10000), "kk": (1251, 866, 10000),
}


def codepages(lang, icu_name):
    if lang == "zh":
        return (950, 950, 10002) if ("Hant" in icu_name or icu_name.endswith(("_TW", "_HK", "_MO"))) else (936, 936, 10008)
    return OEM.get(lang, (1252, 437, 10000))


def abbrev_lang(icu_name):
    lang = icu_name.split("_")[0]
    special = {"en": "ENU", "de": "DEU", "fr": "FRA", "es": "ESP", "it": "ITA", "ja": "JPN", "ko": "KOR", "zh": "CHS", "ru": "RUS", "pt": "PTB", "nl": "NLD", "sv": "SVE",
               "da": "DAN", "fi": "FIN", "nb": "NOR", "pl": "PLK", "cs": "CSY", "hu": "HUN", "tr": "TRK", "el": "ELL", "he": "HEB", "ar": "ARA", "th": "THA"}
    return special.get(lang, uloc_getISO3Language(lang.encode()).decode().upper())


def convert_date_pattern(p):
    """An ICU/CLDR date or time pattern as a Windows picture."""
    out = []
    i = 0
    while i < len(p):
        c = p[i]
        if c == "'":
            j = p.find("'", i + 1)
            if j == i + 1:
                out.append("''")
                i += 2
                continue
            if j < 0:
                j = len(p)
            out.append("'" + p[i + 1:j] + "'")
            i = j + 1
            continue
        j = i
        while j < len(p) and p[j] == c:
            j += 1
        n = j - i
        if c in "yU":
            out.append("yyyy" if n != 2 else "yy")
        elif c in "ML":
            out.append("M" * min(n, 4) if n <= 4 else "MMMM")
        elif c == "d":
            out.append("d" * min(n, 2))
        elif c in "Ec":
            out.append("dddd" if n >= 4 else "ddd")
        elif c == "e":
            out.append("d" * min(n, 2) if n <= 2 else "dddd" if n >= 4 else "ddd")
        elif c in "Hk":
            out.append("H" * min(n, 2))
        elif c in "Kh":
            out.append("h" * min(n, 2))
        elif c == "m":
            out.append("m" * min(n, 2))
        elif c == "s":
            out.append("s" * min(n, 2))
        elif c in "ab":
            out.append("tt")
        elif c == "G":
            out.append("gg")
        elif c in "zZvVOXxS":
            pass
        elif c.isalpha():
            out.append(c * n)
        else:
            # literal characters: letters of a pattern that are not fields have to be quoted in a Windows picture
            out.append(c * n)
        i = j
    s = "".join(out)
    return s.replace(" ", " ").replace(" ", " ").strip()


def clean(s):
    return s.replace(" ", " ").replace(" ", " ")


def pattern_lists(pat):
    """Split a number pattern into (positive, negative)."""
    parts = pat.split(";")
    return parts[0], (parts[1] if len(parts) > 1 else "-" + parts[0])


def grouping_of(pattern):
    pos = pattern.split(";")[0]
    integer = pos.split(".")[0].replace("'", "")
    groups = [len(g) for g in integer.split(",")[1:]]
    if not groups:
        return "0"
    groups.reverse()
    # primary group first: 3;0 or 3;2;0
    out = [str(groups[0])]
    if len(groups) > 1 and groups[1] != groups[0]:
        out.append(str(groups[1]))
    out.append("0")
    return ";".join(out)


def build(lcid, icu):
    L = icu.encode()
    v = {}
    lang = cstr(uloc_getLanguage, L, size=16)
    region = cstr(uloc_getCountry, L, size=16)
    script = cstr(uloc_getScript, L, size=16)
    neutral = region == ""
    # the locale whose data a neutral locale shows: its most likely specific one
    if neutral:
        likely = cstr(uloc_addLikelySubtags, L, size=64)
        data_loc = likely.encode()
        region_loc = cstr(uloc_getCountry, data_loc, size=16)
    else:
        data_loc = L
        region_loc = region
    en = b"en"
    langname_en = ustr(uloc_getDisplayLanguage, L, en)
    langname_native = ustr(uloc_getDisplayLanguage, L, L)
    ctry_en = ustr(uloc_getDisplayCountry, data_loc, en) if not neutral else ""
    ctry_native = ustr(uloc_getDisplayCountry, data_loc, L) if not neutral else ""
    script_en = ustr(uloc_getDisplayScript, L, en)
    full_en = langname_en + (" (%s)" % (script_en if neutral and script else ctry_en) if (ctry_en or (neutral and script)) else "")
    full_native = langname_native + (" (%s)" % (ustr(uloc_getDisplayScript, L, L) if neutral and script else ctry_native) if (ctry_native or (neutral and script)) else "")
    ansi, oem, mac = codepages(lang, icu)
    err = ctypes.c_int(0)
    # dates
    def pattern(date_style, time_style):
        err.value = 0
        f = udat_open(time_style, date_style, data_loc, None, 0, None, 0, ctypes.byref(err))
        p = ustr(udat_toPattern, f, 0)
        syms = f
        return f, p
    f_short, p_short = pattern(3, -1)
    f_long, p_long = pattern(0, -1)
    f_time, p_time = pattern(-1, 2)
    f_short_time, p_short_time = pattern(-1, 3)
    pg = udatpg_open(data_loc, ctypes.byref(err))
    p_ym = ustr(udatpg_getBestPattern, pg, jstr("yMMMM"), 5)
    p_md = ustr(udatpg_getBestPattern, pg, jstr("MMMMd"), 5)
    def sym(f, kind, index):
        e = ctypes.c_int(0)
        return ustr(lambda *a: udat_getSymbols(a[0], a[1], a[2], a[3], a[4], a[5]), f, kind, index, size=64) if False else _sym(f, kind, index)
    def _sym(f, kind, index):
        buf = (c_uint16 * 64)()
        e = ctypes.c_int(0)
        n = udat_getSymbols(f, kind, index, buf, 64, ctypes.byref(e))
        return "".join(chr(buf[i]) for i in range(min(n, 64)))
    months = [_sym(f_long, 10, i) or _sym(f_long, 1, i) for i in range(12)]          # standalone (nominative) forms
    short_months = [_sym(f_long, 11, i) or _sym(f_long, 2, i) for i in range(12)]
    genitive = [_sym(f_long, 1, i) for i in range(12)]
    short_genitive = [_sym(f_long, 2, i) for i in range(12)]
    weekdays = [_sym(f_long, 3, i) for i in range(1, 8)]        # Sunday first
    short_weekdays = [_sym(f_long, 4, i) for i in range(1, 8)]
    am, pm = _sym(f_long, 5, 0), _sym(f_long, 5, 1)
    # numbers
    f_dec = unum_open(1, None, 0, data_loc, None, ctypes.byref(err))
    f_cur = unum_open(2, None, 0, data_loc, None, ctypes.byref(err))
    def usym(f, k):
        buf = (c_uint16 * 32)()
        e = ctypes.c_int(0)
        n = unum_getSymbol(f, k, buf, 32, ctypes.byref(e))
        return "".join(chr(buf[i]) for i in range(min(n, 32)))
    dec, grp = usym(f_dec, 0), usym(f_dec, 1)
    mdec, mgrp = usym(f_cur, 10), usym(f_cur, 17)
    cur_sym, cur_intl = usym(f_cur, 8), usym(f_cur, 9)
    zero = usym(f_dec, 4)
    digits = "".join(chr(ord(zero) + i) for i in range(10)) if zero else "0123456789"
    num_pat = ustr(unum_toPattern, f_dec, 0)
    cur_pat = ustr(unum_toPattern, f_cur, 0)
    cur_pos, cur_neg = pattern_lists(cur_pat)
    # first day of week
    cal = ucal_open(None, 0, data_loc, 1, ctypes.byref(err))
    first = ucal_getAttribute(cal, 1)
    mindays = ucal_getAttribute(cal, 3)
    ucal_close(cal)
    ms = ulocdata_getMeasurementSystem(data_loc, ctypes.byref(err))

    short_date = convert_date_pattern(p_short)
    long_date = convert_date_pattern(p_long)
    time_fmt = convert_date_pattern(p_time)
    short_time = convert_date_pattern(p_short_time)
    year_month = convert_date_pattern(p_ym)
    month_day = convert_date_pattern(p_md)
    # separators and orders
    def sep(p, letters):
        m = re.search(r"[%s]+([^%s']+)[%s]" % (letters, letters, letters), p)
        return m.group(1) if m else "/"
    date_sep = sep(short_date, "dMy")
    time_sep = sep(time_fmt, "Hhms")
    def order(p):
        pos = {c: p.find(c) for c in "dMy" if p.find(c) >= 0}
        names = "".join(sorted(pos, key=pos.get))
        return {"Mdy": 0, "dMy": 1, "yMd": 2}.get(names, 0)
    v[0x21] = str(order(short_date))
    v[0x22] = str(order(long_date))
    h24 = "H" in time_fmt
    v[0x23] = "1" if h24 else "0"
    v[0x24] = "1" if "yyyy" in short_date else "0"
    v[0x25] = "1" if re.search(r"HH|hh", time_fmt) else "0"
    v[0x26] = "1" if "dd" in short_date else "0"
    v[0x27] = "1" if "MM" in short_date.replace("MMM", "") else "0"
    v[0x1005] = "1" if re.search(r"tt\s*$", time_fmt) is None and "tt" in time_fmt else "0"
    # currency formats
    def curr_digits(p):
        m = re.search(r"\.([0#]+)", p)
        return str(len(m.group(1))) if m else "0"
    cur_digits = curr_digits(cur_pos)
    prefix = cur_pos.lstrip().startswith("¤")
    space = " " in cur_pos or " " in cur_pos.replace("'", "")
    v[0x1B] = str((0 if prefix and not space else 1 if not prefix and not space else 2 if prefix else 3))
    neg_shape = cur_neg.replace("¤", "$").replace(" ", " ")
    neg_shape = re.sub(r"[#0,]+(\.[#0]+)?", "1.1", neg_shape)
    NEG = {"($1.1)": 0, "-$1.1": 1, "$-1.1": 2, "$1.1-": 3, "(1.1$)": 4, "-1.1$": 5, "1.1-$": 6, "1.1$-": 7, "-1.1 $": 8, "-$ 1.1": 9, "1.1 $-": 10, "$ 1.1-": 11,
           "$ -1.1": 12, "1.1- $": 13, "($ 1.1)": 14, "(1.1 $)": 15}
    v[0x1C] = str(NEG.get(neg_shape, 0))
    neg_num = "-1.1"
    neg_num_shape = re.sub(r"[#0,]+(\.[#0]+)?", "1.1", pattern_lists(num_pat)[1])
    v[0x1010] = str({"(1.1)": 0, "-1.1": 1, "- 1.1": 2, "1.1-": 3, "1.1 -": 4}.get(neg_num_shape, 1))
    v[0x11] = curr_digits(num_pat) if re.search(r"\.", num_pat) else "0"
    if v[0x11] == "3" or v[0x11] == "0":
        v[0x11] = "2"
    v[0x12] = "1"
    cp_cur = ustr(ucurr_forLocale, data_loc, size=16)
    v[0x01] = "%04x" % lcid
    v[0x02] = full_en
    v[0x03] = abbrev_lang(icu) if not neutral else abbrev_lang(icu)
    v[0x04] = full_native
    v[0x05] = "1"
    v[0x06] = ctry_en
    v[0x07] = (uloc_getISO3Country(("_" + region_loc).encode() if False else (lang + "_" + region_loc).encode()).decode() if region_loc else "")
    v[0x08] = ctry_native
    v[0x09] = "%04x" % (lcid if not neutral else lcid | 0x400 if False else lcid)
    v[0x0A] = "1"
    v[0x0B] = str(oem)
    v[0x0C] = ";" if dec == "," else ","
    v[0x0D] = "1" if ms == 1 else "0"
    v[0x0E] = dec
    v[0x0F] = grp
    v[0x10] = grouping_of(num_pat)
    v[0x13] = digits
    v[0x14] = cur_sym
    v[0x15] = cur_intl
    v[0x16] = mdec
    v[0x17] = mgrp
    v[0x18] = grouping_of(cur_pat)
    v[0x19] = cur_digits
    v[0x1A] = cur_digits
    v[0x1D] = date_sep
    v[0x1E] = time_sep
    v[0x1F] = short_date
    v[0x20] = long_date
    v[0x1003] = time_fmt
    v[0x79] = short_time
    v[0x1006] = year_month
    v[0x78] = month_day
    v[0x28] = am
    v[0x29] = pm
    # Windows counts the days from Monday
    order_days = [1, 2, 3, 4, 5, 6, 0]
    for i, d in enumerate(order_days):
        v[0x2A + i] = weekdays[d]
        v[0x31 + i] = short_weekdays[d]
        v[0x60 + i] = short_weekdays[d][:2] if script in ("", "Latn") and lang not in ("ja", "zh", "ko") else short_weekdays[d]
    for i in range(12):
        v[0x38 + i] = months[i]
        v[0x44 + i] = short_months[i]
    for i in range(12):
        v[0x8038 + i] = genitive[i]
        v[0x8044 + i] = short_genitive[i]
    v[0x100E] = ""
    v[0x100F] = ""
    v[0x50] = ""
    v[0x51] = "-"
    v[0x52] = "3"
    v[0x53] = "0"
    v[0x54] = "1" if prefix else "0"
    v[0x55] = "1" if space else "0"
    v[0x56] = v[0x54]
    v[0x57] = v[0x55]
    v[0x59] = lang
    v[0x5A] = region_loc
    v[0x1001] = langname_en
    v[0x1002] = ctry_en
    v[0x1004] = str(ansi)
    v[0x1011] = str(mac)
    v[0x1012] = "037"
    v[0x1007] = ""
    v[0x1008] = ""
    v[0x1009] = "1"
    v[0x100A] = "9" if ms != 1 else "1"
    v[0x100B] = "0"
    v[0x100C] = str((first + 5) % 7)
    v[0x100D] = {1: "0", 7: "1", 4: "2"}.get(mindays, "0")
    v[0x1013] = ""
    v[0x1014] = "1" if lang in ("ar", "fa") else "1"
    v[0x5C] = windows_name(icu)
    v[0x5D] = "%H:%M:%S"
    v[0x6C] = ("Latn;" if script in ("", "Latn") else script + ";")
    v[0x69] = "NaN"
    v[0x6A] = "Infinity"
    v[0x6B] = "-Infinity"
    v[0x6D] = ""
    v[0x72] = full_en
    v[0x73] = full_native
    v[0x6F] = full_en
    v[0x71] = "1" if neutral else "0"
    v[0x74] = "0"
    v[0x75] = "0"
    v[0x76] = "%"
    v[0x77] = "‰"
    v[0x70] = "1" if lang in ("ar", "he", "fa", "ur") else "0"
    v[0x5B] = "244"
    v[0x7a] = lang
    v[0x7b] = windows_name(icu)
    v[0x7c] = long_date
    v[0x7e] = am[:1]
    v[0x7f] = pm[:1]
    v[0x67] = uloc_getISO3Language(lang.encode()).decode()
    v[0x68] = uloc_getISO3Country((lang + "_" + region_loc).encode()).decode() if region_loc else ""
    v[0x6E] = "Default"
    v[0x58] = ""
    v[0x5E] = ""
    for f in (f_short, f_long, f_time, f_short_time):
        udat_close(f)
    unum_close(f_dec)
    unum_close(f_cur)
    return v



DEFAULT_SPECIFIC = {"ar": "ar_SA", "sr": "sr_Latn_RS", "zh_Hant": "zh_Hant_HK", "zh_Hans": "zh_Hans_CN", "zh": "zh_Hans_CN", "no": "nb_NO", "nb": "nb_NO"}


def currency_names(data_loc):
    err = ctypes.c_int(0)
    code = ustr(ucurr_forLocale, data_loc, size=16)
    if not code or ucurr_getName is None:
        return "", ""
    out = []
    for loc in (b"en", data_loc):
        is_choice = ctypes.c_int8(0)
        n = c_int32(0)
        e = ctypes.c_int(0)
        p = ucurr_getName(jstr(code), loc, 1, ctypes.byref(is_choice), ctypes.byref(n), ctypes.byref(e)) if False else None
        out.append("")
    return out


def patch_en_us(v):
    v.update({0x1F: "M/d/yyyy", 0x25: "0", 0x26: "0", 0x27: "0", 0x24: "1", 0x1003: "h:mm:ss tt", 0x79: "h:mm tt", 0x1006: "MMMM yyyy", 0x78: "MMMM d", 0x1C: "0", 0x1B: "0", 0x1005: "0",
              0x1007: "US Dollar", 0x1008: "US Dollar", 0x52: "3", 0x53: "0", 0x1010: "1", 0x100A: "1", 0x100C: "6", 0x1007: "US Dollar", 0x7C: "dddd, MMMM d, yyyy"})


INVARIANT = {
    0x01: "007f", 0x1001: "Invariant Language", 0x03: "IVL", 0x04: "Invariant Language", 0x05: "1", 0x1002: "Invariant Country", 0x07: "IVC", 0x08: "Invariant Country", 0x09: "0409",
    0x0A: "1", 0x0B: "437", 0x1004: "1252", 0x1011: "10000", 0x0C: ",", 0x0D: "0", 0x0E: ".", 0x0F: ",", 0x10: "3;0", 0x11: "2", 0x12: "1", 0x1010: "1", 0x13: "0123456789",
    0x14: "\u00a4", 0x15: "XDR", 0x16: ".", 0x17: ",", 0x18: "3;0", 0x19: "2", 0x1A: "2", 0x1B: "0", 0x1C: "0", 0x1D: "/", 0x1E: ":", 0x1F: "MM/dd/yyyy", 0x20: "dddd, dd MMMM yyyy",
    0x1003: "HH:mm:ss", 0x21: "0", 0x22: "1", 0x23: "1", 0x1005: "0", 0x24: "1", 0x25: "1", 0x26: "1", 0x27: "1", 0x28: "AM", 0x29: "PM", 0x1009: "1", 0x100B: "0", 0x100C: "6",
    0x100D: "0", 0x50: "+", 0x51: "-", 0x52: "3", 0x53: "0", 0x54: "1", 0x55: "0", 0x56: "1", 0x57: "0", 0x59: "iv", 0x5A: "IV", 0x1012: "037", 0x100A: "9",
    0x1007: "International Monetary Fund", 0x1008: "International Monetary Fund", 0x1006: "yyyy MMMM", 0x1014: "1", 0x5C: "", 0x6C: "Latn;", 0x02: "Invariant Language (Invariant Country)",
    0x06: "Invariant Country", 0x1013: "Math Alphanumerics",
}


def invariant_record():
    v = build(0x409, "en_US")
    for k in list(v):
        v[k] = ""
    days = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
    months = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"]
    base = build(0x409, "en_US")
    for k in base:
        v[k] = base[k]
    for i in range(7):
        v[0x2A + i] = days[i]
        v[0x31 + i] = days[i][:3]
    for i in range(12):
        v[0x38 + i] = months[i]
        v[0x44 + i] = months[i][:3]
    v.update(INVARIANT)
    v[0x6F] = "Invariant Language (Invariant Country)"
    v[0x72] = v[0x6F]
    v[0x73] = v[0x6F]
    v[0x59] = "iv"
    v[0x67] = "ivl"
    v[0x68] = "IVC"
    v[0x1002] = "Invariant Country"
    v[0x5B] = "244"
    v[0x78] = "MMMM dd"
    v[0x79] = "HH:mm"
    v[0x7C] = v[0x20]
    v[0x7E] = "A"
    v[0x7F] = "P"
    v[0x7A] = ""
    v[0x7B] = ""
    return v


# The GEOID of the nations (Windows numbers them in its own order) and the few regions the tests and programs ask for.
GEOIDS = dict(AG=2, AF=3, DZ=4, AZ=5, AL=6, AM=7, AD=8, AO=9, AS=10, AR=11, AU=12, AT=14, BH=17, BB=18, BW=19, BM=20, BE=21, BS=22, BD=23, BZ=24, BA=25, BO=26, MM=27, BJ=28,
              BY=29, SB=30, BR=32, BT=34, BG=35, BN=37, BI=38, CA=39, KH=40, TD=41, LK=42, CG=43, CD=44, CN=45, CL=46, CM=49, KM=50, CO=51, CR=54, CF=55, CU=56, CV=57, CY=59,
              DK=61, DJ=62, DM=63, DO=65, EC=66, EG=67, IE=68, GQ=69, EE=70, ER=71, SV=72, ET=73, CZ=75, FI=77, FJ=78, FM=80, FO=81, FR=84, GM=86, GA=87, GE=88, GH=89, GI=90,
              GD=91, GL=93, DE=94, GR=98, GT=99, GN=100, GY=101, HT=103, HK=104, HN=106, HR=108, HU=109, IS=110, ID=111, IN=113, IO=114, IR=116, IL=117, IT=118, CI=119, IQ=121,
              JP=122, JM=124, JO=126, KE=129, KG=130, KP=131, KI=133, KR=134, KW=136, KZ=137, LA=138, LB=139, LV=140, LT=141, LR=142, SK=143, LI=145, LS=146, LU=147, LY=148,
              MG=149, MO=151, MD=152, MN=154, MW=156, ML=157, MC=158, MA=159, MU=160, MR=162, MT=163, OM=164, MV=165, MX=166, MY=167, MZ=168, NE=173, VU=174, NG=175, NL=176,
              NO=177, NP=178, NR=180, SR=181, NI=182, NZ=183, PS=184, PY=185, PE=187, PK=190, PL=191, PA=192, PT=193, PG=194, PW=195, GW=196, QA=197, RE=198, MH=199, RO=200,
              PH=201, PR=202, RU=203, RW=204, SA=205, PM=206, KN=207, SC=208, ZA=209, SN=210, SI=212, SL=213, SM=214, SG=215, SO=216, ES=217, LC=218, SD=219, SE=221, SY=222,
              CH=223, AE=224, TT=225, TH=227, TJ=228, TO=231, TG=232, ST=233, TN=234, TR=235, TV=236, TW=237, TM=238, TZ=239, UG=240, UA=241, GB=242, US=244, BF=245, UY=246,
              UZ=247, VC=248, VE=249, VN=251, VI=252, VA=253, NA=254, WS=259, SZ=260, YE=261, ZM=262, ZW=263, ME=270, RS=271, SS=276, AI=300, AQ=301, AW=302, KY=307, CX=309,
              CC=311, CK=312, FK=315, GF=317, PF=318, TF=319, GP=321, GU=322, GG=324, JE=328, MQ=330, YT=331, MS=332, NC=334, NU=335, NF=336, MP=337, PN=339, SH=343, TK=347,
              TC=349, VG=351, WF=353)
EASTERN_EUROPE_GEO = 47609
SOUTHERN_EUROPE_GEO = 47610
WORLD_GEO = 39070
EASTERN_EUROPE = {"RU", "BY", "UA", "MD", "BG", "CZ", "HU", "PL", "RO", "SK"}
SOUTHERN_EUROPE = {"AL", "AD", "BA", "HR", "ES", "GI", "GR", "IT", "ME", "MK", "MT", "PT", "RS", "SI", "SM", "VA", "XK"}


def write_geo():
    out = ["/* Generated by scripts/gen-locales.py (ICU %s): the nations of GetGeoInfo. Not compiled by itself. */" % V, ""]
    nations = sorted(GEOIDS.items(), key=lambda kv: kv[1])
    numeric = fn(i18n, "uregion_getRegionFromCode", c_void_p, c_char_p, ERR)
    numeric_code = fn(i18n, "uregion_getNumericCode", c_int32, c_void_p)
    rows = []
    pool = bytearray(b"\0")
    offsets = {"": 0}
    def intern(text):
        if text not in offsets:
            offsets[text] = len(pool)
            pool.extend(text.encode() + b"\0")
        return offsets[text]
    for iso2, geoid in nations:
        err = ctypes.c_int(0)
        region = numeric(iso2.encode(), ctypes.byref(err))
        un = numeric_code(region) if region else 0
        iso3 = uloc_getISO3Country(("en_" + iso2).encode()).decode() or ""
        name = ustr(uloc_getDisplayCountry, ("en_" + iso2).encode(), b"en")
        parent = EASTERN_EUROPE_GEO if iso2 in EASTERN_EUROPE else SOUTHERN_EUROPE_GEO if iso2 in SOUTHERN_EUROPE else WORLD_GEO
        rows.append((geoid, iso2, iso3, un, parent, intern(name)))
    out.append("#define NLS_GEO_COUNT %d" % len(rows))
    out.append("static const struct { DWORD geoid; char iso2[3]; char iso3[4]; WORD un; DWORD parent; DWORD name; } k_geo_nations[NLS_GEO_COUNT] = {")
    for r in rows:
        out.append('{%d,"%s","%s",%d,%d,%d},' % r)
    out.append("};")
    out.append('static const char k_geo_pool[%d] = "%s";' % (len(pool) + 1, "".join("\\000" if b == 0 else ("\\%03o" % b if b >= 0x80 or b == 0x22 or b == 0x5C else chr(b)) for b in pool)))
    with open(os.path.join(root, "src", "Windows", "kernel32", "geo_data.inc"), "w") as f:
        f.write("\n".join(out) + "\n")


def main():
    locales = list_locales()
    names = {lcid: windows_name(icu) for lcid, icu in locales.items()}
    rows = []
    for lcid in sorted(locales):
        icu = locales[lcid]
        v = build(lcid, icu)
        if icu == "en_US":
            patch_en_us(v)
        rows.append([lcid, names[lcid], icu, v])
    # the invariant locale and the alternate sort orders
    rows.append([0x7F, "", "", invariant_record()])
    alternates = [(0x10407, "de-DE_phoneb", 0x407, "Phonebook Sort"), (0x1040E, "hu-HU_technl", 0x40E, "Technical Sort"), (0x40411, "ja-JP_radstr", 0x411, "Radical/Stroke Sort"),
                  (0x10437, "ka-GE_modern", 0x437, "Modern Sort"), (0x20804, "zh-CN_stroke", 0x804, "Stroke Sort"), (0x30404, "zh-TW_pronun", 0x404, "Pronunciation Sort"),
                  (0x40404, "zh-TW_radstr", 0x404, "Radical/Stroke Sort")]
    by_lcid = {r[0]: r for r in rows}
    for lcid, name, base, sort_name in alternates:
        src = by_lcid[base]
        v = dict(src[3])
        v[0x5C] = name
        v[0x7B] = name
        v[0x1013] = sort_name
        rows.append([lcid, name, src[2], v])
    # Windows locales ICU has no data for: they get the data of English (United States) under their own names
    for lcid, name, iso in ((0x465, "dv-MV", "dv"), (0x143B, "smj-SE", "smj")):
        if lcid not in by_lcid:
            v = dict(by_lcid[0x409][3])
            v[0x59] = iso
            v[0x5C] = name
            v[0x7B] = name
            v[0x01] = "%04x" % lcid
            v[0x09] = "%04x" % lcid
            rows.append([lcid, name, "dv_MV", v])
    rows.sort(key=lambda r: r[0])
    index_of_lcid = {r[0]: i for i, r in enumerate(rows)}
    # the default specific locale of each neutral locale
    default_of = []
    for lcid, name, icu, v in rows:
        if lcid == 0x7F or lcid > 0xFFFF:
            default_of.append(index_of_lcid[lcid])
            continue
        if icu and cstr(uloc_getCountry, icu.encode(), size=16) == "":
            likely = DEFAULT_SPECIFIC.get(icu) or cstr(uloc_addLikelySubtags, icu.encode(), size=64)
            region = cstr(uloc_getCountry, likely.encode(), size=16)
            lang = cstr(uloc_getLanguage, likely.encode(), size=16)
            script = cstr(uloc_getScript, likely.encode(), size=16)
            # the specific locale of the same language (and script) with that region
            target = None
            for cand_lcid, cand_name, cand_icu, _ in rows:
                if cand_icu and cstr(uloc_getLanguage, cand_icu.encode(), size=16) == lang and cstr(uloc_getCountry, cand_icu.encode(), size=16) == region:
                    cs = cstr(uloc_getScript, cand_icu.encode(), size=16)
                    if (not script or not cs or cs == script) and cand_lcid & 0x3FF == cand_lcid and False:
                        pass
                    if not script or not cs or cs == script:
                        target = cand_lcid
                        break
            default_of.append(index_of_lcid[target] if target is not None else index_of_lcid[lcid])
        else:
            default_of.append(index_of_lcid[lcid])
    # a neutral locale shows the data of its specific locale under its own name
    names_by_index = [r[1] for r in rows]
    for i, (lcid, name, icu, v) in enumerate(rows):
        if default_of[i] != i:
            data = dict(rows[default_of[i]][3])
            data[0x5C] = name
            data[0x71] = "1"
            data[0x6D] = ""
            data[0x7B] = name
            rows[i][3] = data
    for i, (lcid, name, icu, v) in enumerate(rows):
        if default_of[i] == i and lcid != 0x7F and lcid <= 0xFFFF and icu:
            lang = cstr(uloc_getLanguage, icu.encode(), size=16)
            script = cstr(uloc_getScript, icu.encode(), size=16)
            parent = ""
            for j, (l2, n2, i2, _) in enumerate(rows):
                if default_of[j] != j and i2 and cstr(uloc_getCountry, i2.encode(), size=16) == "" and cstr(uloc_getLanguage, i2.encode(), size=16) == lang:
                    s2 = cstr(uloc_getScript, i2.encode(), size=16)
                    if lang == "zh":
                        if (not s2 and False) or s2 != ("Hant" if ("Hant" in icu or icu.endswith(("_TW", "_HK", "_MO"))) else "Hans"):
                            continue
                    parent = n2
                    break
            v[0x6D] = parent
    # the tables
    pool = bytearray(b"\0")
    offsets = {"": 0}
    def intern(text):
        if text in offsets:
            return offsets[text]
        data = text.encode("utf-8") + b"\0"
        offsets[text] = len(pool)
        pool.extend(data)
        return offsets[text]
    types = [t for t in BASE_TYPES if t not in (0x58, 0x5E)] + list(range(0x8038, 0x8044)) + list(range(0x8044, 0x8050))
    rows_out = []
    for lcid, name, icu, v in rows:
        rows_out.append([intern(v.get(t, "")) for t in types])
    out = ["/* Generated by scripts/gen-locales.py (ICU %s): the locales of kernel32.dll. Not compiled by itself. */" % V, ""]
    out.append("#define NLS_LOCALE_COUNT %d" % len(rows))
    out.append("#define NLS_SLOT_COUNT %d" % len(types))
    out.append("static const WORD k_locale_types[NLS_SLOT_COUNT] = {%s};" % ",".join("0x%x" % t for t in types))
    out.append("static const DWORD k_locale_lcid[NLS_LOCALE_COUNT] = {%s};" % ",".join("0x%x" % r[0] for r in rows))
    out.append("static const WORD k_locale_default[NLS_LOCALE_COUNT] = {%s};   /* the specific locale a neutral one stands for (itself for the others) */" % ",".join(str(d) for d in default_of))
    out.append("static const DWORD k_locale_strings[NLS_LOCALE_COUNT][NLS_SLOT_COUNT] = {")
    for r in rows_out:
        out.append("{" + ",".join(str(x) for x in r) + "},")
    out.append("};")
    # the pool as a string literal in octal escapes
    chunks = []
    line = []
    for b in pool:
        if b == 0:
            line.append("\\000")
        elif b == ord('"'):
            line.append('\\"')
        elif b == ord("\\"):
            line.append("\\\\")
        elif 32 <= b < 127 and chr(b) not in "?":
            line.append(chr(b))
        else:
            line.append("\\%03o" % b)
        if len(line) >= 40:
            chunks.append('"' + "".join(line) + '"')
            line = []
    if line:
        chunks.append('"' + "".join(line) + '"')
    out.append("static const char k_locale_pool[%d] =" % (len(pool) + 1))
    out.append("\n".join(chunks) + ";")
    path = os.path.join(root, "src", "Windows", "kernel32", "locale_data.inc")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s (%d bytes, %d locales, pool %d)" % (path, os.path.getsize(path), len(rows), len(pool)))
    # ntdll: only the identifiers and the names
    nm = bytearray()
    name_offsets = []
    for r in rows:
        name_offsets.append(len(nm))
        nm.extend(r[1].encode() + b"\0")
    out = ["/* Generated by scripts/gen-locales.py (ICU %s): the locale names and identifiers of ntdll.dll. Not compiled by itself. */" % V, ""]
    out.append("#define RTL_LOCALE_COUNT %d" % len(rows))
    out.append("static const DWORD k_rtl_locale_lcid[RTL_LOCALE_COUNT] = {%s};" % ",".join("0x%x" % r[0] for r in rows))
    out.append("static const WORD k_rtl_locale_default[RTL_LOCALE_COUNT] = {%s};" % ",".join(str(d) for d in default_of))
    out.append("static const WORD k_rtl_locale_name[RTL_LOCALE_COUNT] = {%s};" % ",".join(str(o) for o in name_offsets))
    out.append('static const char k_rtl_locale_names[%d] = "%s";' % (len(nm) + 1, "".join("\\000" if b == 0 else chr(b) for b in nm)))
    with open(os.path.join(root, "src", "Windows", "ntdll", "locale_names.inc"), "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
    write_geo()
