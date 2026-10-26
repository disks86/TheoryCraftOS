#!/usr/bin/env python3
"""Checks that the field blocks of the VCE 3.4 task commands (src/Hardware/VideoCodec/AMD/AmdVce3Messages.hpp) have the fields Mesa's radeon_vce.h declares and that they are
written in the order Mesa's radeon_vce_52.c writes them.

  check_vce3_messages.py path/to/mesa/src/gallium/drivers/radeonsi/radeon_vce.h path/to/.../radeon_vce_52.c

A block (rate control, motion estimation, pic control, rdo, task info) is a plain list of dwords in the port, written in declaration order; so the check is that, for each
block, (1) the port's declaration lists the same field names in the same order as Mesa's struct, and (2) Mesa's function writes the fields of its struct in that same order.
"""
import os
import re
import sys


def struct_fields(text, name):
    m = re.search(r'struct\s+' + name + r'\s*\{(.*?)\n\};', text, flags=re.S)
    if not m:
        sys.exit(f'struct not found: {name}')
    return re.findall(r'\bu?int32_t\s+(\w+);', m.group(1))


def port_fields(text, name):
    m = re.search(r'struct\s+' + name + r'\s*\{(.*?)\n\};', text, flags=re.S)
    if not m:
        sys.exit(f'port struct not found: {name}')
    out = []
    for decl in re.findall(r'uint32_t\s+([^;]*);', m.group(1)):
        out += re.findall(r'\b(\w+)\s*=', decl)
    return out


def written_fields(text, function, prefix):
    m = re.search(r'static void ' + function + r'\(struct rvce_encoder \*enc[^)]*\)\s*\{(.*?)\n\}', text, flags=re.S)
    if not m:
        sys.exit(f'function not found: {function}')
    return re.findall(r'RVCE_CS\(enc->enc_pic\.' + prefix + r'\.(\w+)\)', m.group(1))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    header = open(sys.argv[1]).read()
    source = open(sys.argv[2]).read()
    port = open(os.path.join(here, '..', 'src/Hardware/VideoCodec/AMD/AmdVce3Messages.hpp')).read()
    blocks = [('rate control', 'rvce_rate_control', 'RateControl', 'rate_control', 'rc'),
              ('motion estimation', 'rvce_motion_estimation', 'MotionEstimation', 'motion_estimation', 'me'),
              ('pic control', 'rvce_pic_control', 'PicControl', 'pic_control', 'pc'),
              ('rdo', 'rvce_rdo', 'Rdo', 'rdo', 'rdo'),
              ('task info', 'rvce_task_info', 'TaskInfo', 'task_info', 'ti')]
    failed = False
    for label, mesa_struct, port_struct, function, prefix in blocks:
        declared = struct_fields(header, mesa_struct)
        mine = port_fields(port, port_struct)
        written = written_fields(source, function, prefix)
        same_declaration = declared == mine
        same_writes = written == declared
        print(f'{label:20s} mesa struct {len(declared):3d}  port struct {len(mine):3d}  mesa writes {len(written):3d}  {"same" if same_declaration and same_writes else "DIFFERENT"}')
        if not (same_declaration and same_writes):
            failed = True
            for name, a, b in (('declaration', declared, mine), ('writes', written, declared)):
                for i, (x, y) in enumerate(zip(a, b)):
                    if x != y:
                        print(f'    {name}: first difference at {i}: {x} vs {y}')
                        break
                else:
                    if len(a) != len(b):
                        print(f'    {name}: lengths differ')
    sys.exit(1 if failed else 0)


main()
