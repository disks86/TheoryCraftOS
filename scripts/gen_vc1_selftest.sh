#!/bin/bash
# Makes the streams the VCN VC-1 self-test decodes (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc): a Main profile and an Advanced profile stream of eight 64x64
# pictures with B pictures (so the engine needs both of its reference slots), from scripts/gen_vc1_synthetic.py (there is no VC-1 encoder to be had). Not kept in
# tests/data; only the generated C++ arrays are. Usage: scripts/gen_vc1_selftest.sh
set -e
python3 -I "$(dirname "$0")/gen_vc1_synthetic.py" --selftest src/Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc
