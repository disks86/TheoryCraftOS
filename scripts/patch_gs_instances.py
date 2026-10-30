#!/usr/bin/env python3
# Makes tests/data/dxbc/gs_copy_inst2.dxbc from gs_copy.dxbc: the same geometry shader with `dcl_gs_instance_count 2` (the Wine HLSL compiler cannot build [instance(n)] shaders), so it runs twice
# for each primitive and draws the same pictures twice over.
import struct, os
root = os.path.join(os.path.dirname(__file__), '..', 'tests', 'data', 'dxbc')
data = bytearray(open(os.path.join(root, 'gs_copy.dxbc'), 'rb').read())
count = struct.unpack_from('<I', data, 28)[0]
offsets = list(struct.unpack_from('<%dI' % count, data, 32))
for k, off in enumerate(offsets):
    if bytes(data[off:off + 4]) in (b'SHEX', b'SHDR'):
        size = struct.unpack_from('<I', data, off + 4)[0]
        body = off + 8
        words = list(struct.unpack_from('<%dI' % (size // 4), data, body))
        i = 2
        while i < len(words):
            length = words[i] >> 24 if (words[i] & 0x7FF) != 0x35 else 0
            op = words[i] & 0x7FF
            if op == 94:   # dcl_maxout: the instance count goes after it
                new = words[:i + length] + [206 | (2 << 24), 2] + words[i + length:]
                new[1] = len(new)
                tail_old = bytes(data[body + size:])
                out = bytearray(data[:body]) + struct.pack('<%dI' % len(new), *new) + tail_old
                delta = 8
                struct.pack_into('<I', out, off + 4, size + delta)
                for j in range(count):
                    if offsets[j] > off:
                        struct.pack_into('<I', out, 32 + 4 * j, offsets[j] + delta)
                struct.pack_into('<I', out, 24, struct.unpack_from('<I', data, 24)[0] + delta)
                open(os.path.join(root, 'gs_copy_inst2.dxbc'), 'wb').write(out)
                raise SystemExit(0)
            i += length
raise SystemExit('no dcl_maxout found')
