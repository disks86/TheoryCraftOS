#!/usr/bin/env python3
"""Regenerates the Common Encryption fixtures that tests/MseTest.cpp and tests/PageTest.cpp check the Clear Key decryption against.

Every file is mse_muxed.mp4 (H.264 + AAC, two one-second fragments; see make_mse_fixtures.py) with its samples encrypted under one of the Common
Encryption schemes, so the decrypted samples must come out the same as the samples of the clear file:

eme_cenc.mp4    'cenc'  AES-CTR, 8-byte IVs per sample, subsamples around the NAL headers, per-sample IVs in senc
eme_cens.mp4    'cens'  AES-CTR with the 1:9 pattern, 8-byte IVs, senc
eme_cbc1.mp4    'cbc1'  AES-CBC without a pattern, 16-byte IVs, the IVs in saiz / saio instead of senc
eme_cbcs.mp4    'cbcs'  AES-CBC with the 1:9 pattern, a constant IV (the sound: all blocks), senc without IVs
eme_rotate.mp4  'cenc'  the second fragment is under another key (a 'seig' sample group and a pssh box in its moof)
eme_ffmpeg.mp4  'cenc'  written by ffmpeg itself (the independent check of the 'cenc' layout), key and key id as below

Keys: KEY1 / KID1 for everything, KEY2 / KID2 for the second fragment of eme_rotate.mp4 (see the constants).
Needs: ffmpeg (for eme_ffmpeg.mp4) and openssl (the script checks its own AES against it before writing anything). Usage: tests/data/make_eme_fixtures.py
"""
import os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)

KID1 = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
KEY1 = bytes.fromhex("f0e1d2c3b4a5968778695a4b3c2d1e0f")
KID2 = bytes.fromhex("101112131415161718191a1b1c1d1e1f")
KEY2 = bytes.fromhex("0f1e2d3c4b5a69788796a5b4c3d2e1f0")
CONSTANT_IV = bytes.fromhex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf")
W3C_SYSTEM = bytes.fromhex("1077efecc0b24d02ace33c1e52e2fb4b")
WIDEVINE_SYSTEM = bytes.fromhex("edef8ba979d64acea3c827dcd51d21ed")

# ---------------------------------------------------------------------------------------------------------------------------------- AES-128
SBOX = [0] * 256
def _build():
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xFF
        if q & 0x80: q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) & 0xFF ^ ((q << 2) | (q >> 6)) & 0xFF ^ ((q << 3) | (q >> 5)) & 0xFF ^ ((q << 4) | (q >> 4)) & 0xFF
        SBOX[p] = (x ^ 0x63) & 0xFF
        if p == 1: break
    SBOX[0] = 0x63
_build()

def _xtime(a): return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1

def expand_key(key):
    w = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(11)]

def encrypt_block(rks, block):
    s = [b ^ k for b, k in zip(block, rks[0])]
    for r in range(1, 11):
        s = [SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]   # ShiftRows (column-major state)
        if r < 10:
            out = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                out += [_xtime(a[0]) ^ (_xtime(a[1]) ^ a[1]) ^ a[2] ^ a[3], a[0] ^ _xtime(a[1]) ^ (_xtime(a[2]) ^ a[2]) ^ a[3],
                        a[0] ^ a[1] ^ _xtime(a[2]) ^ (_xtime(a[3]) ^ a[3]), (_xtime(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ _xtime(a[3])]
            s = out
        s = [b ^ k for b, k in zip(s, rks[r])]
    return bytes(s)

def ctr_stream(key, iv, nblocks, first_block=0):
    """Keystream blocks of CTR mode: the 16-byte IV as a big-endian counter."""
    rks = expand_key(key)
    counter = int.from_bytes(iv + bytes(16 - len(iv)), "big") + first_block   # (an 8-byte IV is the high half of the counter block)
    return b"".join(encrypt_block(rks, ((counter + i) & ((1 << 128) - 1)).to_bytes(16, "big")) for i in range(nblocks))

def cbc_encrypt(key, iv, data):
    rks = expand_key(key)
    out, prev = bytearray(), iv
    for i in range(0, len(data), 16):
        prev = encrypt_block(rks, bytes(a ^ b for a, b in zip(data[i:i + 16], prev)))
        out += prev
    return bytes(out), prev

def self_check():
    # FIPS-197 appendix B / C.1
    rks = expand_key(bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c"))
    assert encrypt_block(rks, bytes.fromhex("3243f6a8885a308d313198a2e0370734")).hex() == "3925841d02dc09fbdc118597196a0b32"
    rks = expand_key(bytes.fromhex("000102030405060708090a0b0c0d0e0f"))
    assert encrypt_block(rks, bytes.fromhex("00112233445566778899aabbccddeeff")).hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"
    # against openssl: CTR and CBC over random-ish data
    data = bytes((i * 37 + 11) & 0xFF for i in range(100))
    iv = bytes(range(16))
    ref = subprocess.run(["openssl", "enc", "-aes-128-ctr", "-K", KEY1.hex(), "-iv", iv.hex()], input=data, capture_output=True, check=True).stdout
    ks = ctr_stream(KEY1, iv, 7)
    assert bytes(a ^ b for a, b in zip(data, ks)) == ref
    ref = subprocess.run(["openssl", "enc", "-aes-128-cbc", "-nopad", "-K", KEY1.hex(), "-iv", iv.hex()], input=data[:96], capture_output=True, check=True).stdout
    assert cbc_encrypt(KEY1, iv, data[:96])[0] == ref

# --------------------------------------------------------------------------------------------------------------------------------- boxes
def box(kind, payload=b""):
    return struct.pack(">I4s", 8 + len(payload), kind) + payload

def full(kind, version, flags, payload=b""):
    return box(kind, struct.pack(">I", (version << 24) | flags) + payload)

def split(data):
    out, at = [], 0
    while at + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        out.append((kind, data[at + 8:at + size]))
        at += size
    return out

def join(children):
    return b"".join(box(k, p) for k, p in children)

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"moof", b"traf", b"mvex", b"edts", b"dinf"}
def tree(data):
    return [(k, tree(p) if k in CONTAINERS else p) for k, p in split(data)]
def untree(nodes):
    return b"".join(box(k, untree(p) if isinstance(p, list) else p) for k, p in nodes)

def find(nodes, kind):
    return [n for n in nodes if n[0] == kind]

def handler_of(trak):
    mdia = find(trak[1], b"mdia")[0]
    return find(mdia[1], b"hdlr")[0][1][8:12]

def track_id_of(trak):
    tkhd = find(trak[1], b"tkhd")[0][1]
    return struct.unpack(">I", tkhd[12:16] if tkhd[0] == 0 else tkhd[20:24])[0]

def stbl_of(trak):
    return find(find(find(trak[1], b"mdia")[0][1], b"minf")[0][1], b"stbl")[0][1]

# ------------------------------------------------------------------------------------------------------------------------------ the scheme
class Scheme:
    def __init__(self, name, pattern, iv_size, constant_iv, aux_in_saio=False):
        self.name, self.pattern, self.iv_size, self.constant_iv, self.aux_in_saio = name, pattern, iv_size, constant_iv, aux_in_saio

SCHEMES = {
    "cenc": Scheme(b"cenc", (0, 0), 8, None),
    "cens": Scheme(b"cens", (1, 9), 8, None),
    "cbc1": Scheme(b"cbc1", (0, 0), 16, None, aux_in_saio=True),
    "cbcs": Scheme(b"cbcs", (1, 9), 0, CONSTANT_IV),
}

def tenc_box(scheme, kid, audio=False):
    crypt, skip = scheme.pattern
    if audio and scheme.name == b"cbcs":
        crypt, skip = 0, 0
    version = 1 if scheme.name in (b"cens", b"cbcs") else 0
    payload = bytes([0, (crypt << 4) | skip if version else 0, 1, scheme.iv_size]) + kid
    if scheme.iv_size == 0:
        payload += bytes([16]) + scheme.constant_iv
    return full(b"tenc", version, 0, payload)

def pssh_box(system, kids, data=b"", version=1):
    payload = system
    if version == 1:
        payload += struct.pack(">I", len(kids)) + b"".join(kids)
    return full(b"pssh", version, 0, payload + struct.pack(">I", len(data)) + data)

def protect_entry(entry, scheme, kid, audio):
    """A sample entry (avc1 / mp4a) as the 'encv' / 'enca' entry with its sinf."""
    size, kind = struct.unpack(">I4s", entry[:8])
    body = entry[8:]
    skip = 28 if audio else 78
    head, children = body[:skip], body[skip:]
    sinf = box(b"sinf", box(b"frma", kind) + full(b"schm", 0, 0, scheme.name + struct.pack(">I", 0x00010000)) + box(b"schi", tenc_box(scheme, kid, audio)))
    return box(b"enca" if audio else b"encv", head + children + sinf)

def nal_length_size(entry):
    body = entry[8:]
    for kind, payload in split(body[78:]):
        if kind == b"avcC":
            return (payload[4] & 3) + 1
    raise SystemExit("no avcC")

# ---------------------------------------------------------------------------------------------------------------------------- the samples
def video_subsamples(sample, nal_size):
    """[(clear, protected)] with the NAL headers (and every NAL that is not a slice) in the clear."""
    out, clear, at = [], 0, 0
    while at < len(sample):
        length = int.from_bytes(sample[at:at + nal_size], "big")
        nal_type = sample[at + nal_size] & 0x1F
        total = nal_size + length
        if 1 <= nal_type <= 5 and length > 1:
            out.append((clear + nal_size + 1, length - 1))
            clear = 0
        else:
            clear += total
        at += total
    if clear or not out:
        out.append((clear, 0))
    fixed = []
    for c, p in out:   # (a clear part is at most 65535 bytes)
        while c > 65535:
            fixed.append((65535, 0)); c -= 65535
        fixed.append((c, p))
    return fixed

def pattern_blocks(length, crypt, skip):
    """Which byte ranges of a protected range are encrypted: whole blocks only, the pattern from the start; (0, 0) is every block."""
    blocks = length // 16
    ranges, b = [], 0
    if crypt == 0 and skip == 0:
        return [(0, blocks * 16)] if blocks else []
    while b < blocks:
        n = min(crypt, blocks - b)
        ranges.append((b * 16, (b + n) * 16))
        b += crypt + skip
    return ranges

def align_subsamples(subsamples):
    """'cbc1' has no pattern and no partial block: the protected part of a subsample is a whole number of blocks, the rest of it is clear."""
    return [(c + p % 16, p - p % 16) for c, p in subsamples]

def encrypt_sample(scheme, key, iv, sample, subsamples, crypt, skip):
    out = bytearray(sample)
    if subsamples is None:
        subsamples = [(0, len(sample))]
    at = 0
    protected = []   # (start, length) of every protected range
    for clear, prot in subsamples:
        at += clear
        protected.append((at, prot))
        at += prot
    ctr_block = 0          # blocks of keystream used (cenc: bytes)
    ctr_bytes = 0
    chain = iv
    for start, length in protected:
        if scheme.name == b"cenc":
            ks = ctr_stream(key, iv, (ctr_bytes + length + 15) // 16)
            for i in range(length):
                out[start + i] = sample[start + i] ^ ks[ctr_bytes + i]
            ctr_bytes += length
        elif scheme.name == b"cens":
            for lo, hi in pattern_blocks(length, crypt, skip):
                ks = ctr_stream(key, iv, (hi - lo) // 16, ctr_block)
                for i in range(hi - lo):
                    out[start + lo + i] = sample[start + lo + i] ^ ks[i]
                ctr_block += (hi - lo) // 16
        elif scheme.name == b"cbc1":
            whole = (length // 16) * 16
            enc, chain = cbc_encrypt(key, chain, sample[start:start + whole])
            out[start:start + whole] = enc
        elif scheme.name == b"cbcs":
            chain = iv   # each subsample starts again with the IV
            for lo, hi in pattern_blocks(length, crypt, skip):
                enc, chain = cbc_encrypt(key, chain, sample[start + lo:start + hi])
                out[start + lo:start + hi] = enc
    return bytes(out)

# -------------------------------------------------------------------------------------------------------------------------------- the file
def protect(src_name, dst_name, scheme_name, rotate=False):
    scheme = SCHEMES[scheme_name]
    data = open(src_name, "rb").read()
    top = split(data)
    # --- the initialization segment
    moov = tree(box(b"moov", top[1][1]))[0]
    nal_size, kinds = 4, {}
    new_children = []
    for kind, payload in moov[1]:
        if kind == b"trak":
            handler = handler_of((kind, payload))
            kinds[track_id_of((kind, payload))] = handler
            audio = handler == b"soun"
            def patch_stbl(nodes):
                out = []
                for k, p in nodes:
                    if k == b"stsd":
                        entry_count = struct.unpack(">I", p[4:8])[0]
                        assert entry_count == 1
                        entry = p[8:]
                        nonlocal_nal[0] = nal_length_size(entry) if not audio else 4
                        out.append((k, p[:8] + protect_entry(entry, scheme, KID1, audio)))
                    else:
                        out.append((k, p))
                return out
            nonlocal_nal = [4]
            trak = []
            for k, p in payload:
                if k == b"mdia":
                    mdia = []
                    for k2, p2 in p:
                        if k2 == b"minf":
                            minf = [(k3, patch_stbl(p3) if k3 == b"stbl" else p3) for k3, p3 in p2]
                            mdia.append((k2, minf))
                        else:
                            mdia.append((k2, p2))
                    trak.append((k, mdia))
                else:
                    trak.append((k, p))
            if not audio:
                nal_size = nonlocal_nal[0]
            new_children.append((kind, trak))
        else:
            new_children.append((kind, payload))
    psshs = [pssh_box(W3C_SYSTEM, [KID1]), pssh_box(WIDEVINE_SYSTEM, [], b"widevine-data-" + KID1, version=0)]
    init = box(b"moov", untree(new_children) + b"".join(psshs))
    out = [top[0] and box(b"ftyp", top[0][1]), init]
    # --- the media segments
    sample_counter = 0
    fragment = 0
    for i in range(2, len(top)):
        kind, payload = top[i]
        if kind != b"moof":
            continue
        moof = tree(box(b"moof", payload))[0]
        mdat_payload = top[i + 1][1]
        moof_size = 8 + len(payload)
        trafs, added_total = [], 0
        sample_ranges = []   # per traf: [(offset in mdat payload, size)]
        rotated = rotate and fragment == 1
        new_moof_children = []
        for k, p in moof[1]:
            if k != b"traf":
                new_moof_children.append((k, p))
                continue
            tfhd = find(p, b"tfhd")[0][1]
            track = struct.unpack(">I", tfhd[4:8])[0]
            audio = kinds[track] == b"soun"
            trun = find(p, b"trun")[0][1]
            flags = struct.unpack(">I", b"\0" + trun[1:4])[0]
            count = struct.unpack(">I", trun[4:8])[0]
            at = 8 + (4 if flags & 1 else 0) + (4 if flags & 4 else 0)
            data_offset = struct.unpack(">i", trun[8:12])[0] if flags & 1 else 0
            sizes = []
            tfhd_flags = struct.unpack(">I", b"\0" + tfhd[1:4])[0]
            default_size = 0
            at_t = 8 + (8 if tfhd_flags & 1 else 0) + (4 if tfhd_flags & 2 else 0) + (4 if tfhd_flags & 8 else 0)
            if tfhd_flags & 0x10:
                default_size = struct.unpack(">I", tfhd[at_t:at_t + 4])[0]
            per = (4 if flags & 0x100 else 0) + (4 if flags & 0x200 else 0) + (4 if flags & 0x400 else 0) + (4 if flags & 0x800 else 0)
            for s in range(count):
                row = trun[at + s * per:at + (s + 1) * per]
                size = default_size
                if flags & 0x200:
                    off = 4 if flags & 0x100 else 0
                    size = struct.unpack(">I", row[off:off + 4])[0]
                sizes.append(size)
            sample_ranges.append((track, audio, data_offset - moof_size - 8, sizes))
        # encrypt
        mdat = bytearray(mdat_payload)
        aux_blocks = []
        senc_per_traf = []
        key, kid = (KEY2, KID2) if rotated else (KEY1, KID1)
        for track, audio, offset, sizes in sample_ranges:
            entries, ivs = [], []
            pos = offset
            use_key, use_kid = (key, kid) if not audio else (KEY1, KID1)
            crypt, skip = scheme.pattern
            if audio and scheme.name == b"cbcs":
                crypt, skip = 0, 0
            for size in sizes:
                sample = bytes(mdat[pos:pos + size])
                sample_counter += 1
                if scheme.iv_size == 8:
                    iv = struct.pack(">Q", sample_counter)
                elif scheme.iv_size == 16:
                    iv = bytes((sample_counter * 7 + j) & 0xFF for j in range(16))
                else:
                    iv = scheme.constant_iv
                subs = None if audio else video_subsamples(sample, nal_size)
                if subs is not None and scheme.name == b"cbc1":
                    subs = align_subsamples(subs)
                mdat[pos:pos + size] = encrypt_sample(scheme, use_key, iv, sample, subs, crypt, skip)
                entries.append(subs)
                ivs.append(iv if scheme.iv_size else b"")
                pos += size
            aux = b""
            for iv, subs in zip(ivs, entries):
                aux += iv
                if subs is not None:
                    aux += struct.pack(">H", len(subs)) + b"".join(struct.pack(">HI", c, p) for c, p in subs)
            senc_per_traf.append((audio, entries, aux, len(sizes)))
        # the traf boxes again, with the encryption boxes after the trun
        fragment_added = 0
        pending_aux = b""
        new_trafs = []
        index = 0
        for k, p in moof[1]:
            if k != b"traf":
                continue
            audio, entries, aux, count = senc_per_traf[index]
            extra = []
            has_subs = any(e is not None for e in entries)
            if scheme.aux_in_saio:
                # the sample information is placed after the samples in the mdat; saiz says how big each is, saio where
                sizes_list = []
                at = 0
                for subs in entries:
                    n = scheme.iv_size + (2 + 6 * len(subs) if subs is not None else 0)
                    sizes_list.append(n)
                saiz = full(b"saiz", 0, 0, struct.pack(">BI", 0, count) + bytes(sizes_list))
                saio = full(b"saio", 0, 0, struct.pack(">II", 1, 0))   # (the offset is patched below)
                extra += [(b"saiz", saiz[8:]), (b"saio", saio[8:])]
                pending_aux += aux
            else:
                senc = struct.pack(">I", (0 << 24) | (2 if has_subs else 0)) + struct.pack(">I", count) + aux
                extra.append((b"senc", senc))
            if rotated and not audio:
                sgpd = struct.pack(">I", (1 << 24)) + b"seig" + struct.pack(">II", 20, 1) + bytes([0, 0, 1, scheme.iv_size]) + KID2
                sbgp = struct.pack(">I", 0) + b"seig" + struct.pack(">I", 1) + struct.pack(">II", count, 1)
                extra += [(b"sgpd", sgpd), (b"sbgp", sbgp)]
            new_trafs.append((k, [n for n in p] + extra))
            index += 1
        # the moof with the new traf boxes: its size changes, the sample positions with it
        moof_children = []
        ti = 0
        for k, p in moof[1]:
            if k == b"traf":
                moof_children.append(new_trafs[ti])
                ti += 1
            else:
                moof_children.append((k, p))
        if rotated:
            moof_children.insert(1, (b"pssh", pssh_box(W3C_SYSTEM, [KID2])[8:]))
        new_moof = untree(moof_children)
        new_size = 8 + len(new_moof)
        delta = new_size - moof_size
        # trun data offsets move by the growth of the moof
        def fix_truns(nodes):
            out = []
            for k, p in nodes:
                if k == b"traf":
                    fixed = []
                    for k2, p2 in p:
                        if k2 == b"trun":
                            fl = struct.unpack(">I", b"\0" + p2[1:4])[0]
                            if fl & 1:
                                off = struct.unpack(">i", p2[8:12])[0] + delta
                                p2 = p2[:8] + struct.pack(">i", off) + p2[12:]
                        fixed.append((k2, p2))
                    out.append((k, fixed))
                else:
                    out.append((k, p))
            return out
        moof_children = fix_truns(moof_children)
        if scheme.aux_in_saio:
            # the saio offsets: the aux data sits after the samples, in traf order
            mdat_samples = len(mdat)
            cursor = new_size + 8 + mdat_samples
            patched = []
            for k, p in moof_children:
                if k == b"traf":
                    fixed = []
                    for k2, p2 in p:
                        if k2 == b"saio":
                            p2 = p2[:8] + struct.pack(">I", cursor)
                            # the size of this traf's aux data: from its saiz
                            saiz_payload = [x for x in p if x[0] == b"saiz"][0][1]
                            n = struct.unpack(">I", saiz_payload[5:9])[0]
                            cursor += sum(saiz_payload[9:9 + n])
                        fixed.append((k2, p2))
                    patched.append((k, fixed))
                else:
                    patched.append((k, p))
            moof_children = patched
            mdat = mdat + bytearray(pending_aux)
        new_moof = untree(moof_children)
        out.append(box(b"moof", new_moof))
        out.append(box(b"mdat", bytes(mdat)))
        fragment += 1
    open(dst_name, "wb").write(b"".join(out))

def main():
    self_check()
    for name in SCHEMES:
        protect("mse_muxed.mp4", "eme_%s.mp4" % name, name)
    protect("mse_muxed.mp4", "eme_rotate.mp4", "cenc", rotate=True)
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=25", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "2",
                    "-c:v", "libx264", "-profile:v", "main", "-bf", "2", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "64k",
                    "-movflags", "+frag_keyframe+empty_moov+default_base_moof", "-encryption_scheme", "cenc-aes-ctr", "-encryption_key", KEY1.hex(), "-encryption_kid", KID1.hex(),
                    "eme_ffmpeg.mp4"], check=True)

if __name__ == "__main__":
    main()
