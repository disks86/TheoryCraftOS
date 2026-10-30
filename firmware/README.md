# /firmware

Microcode the OS loads at boot from `/firmware` on its root volume. The build copies this folder into the
boot volume (`<build>/esp/firmware`); on an installed machine copy the files to `/firmware` on the system volume.

## Contents

| File | For | Notes |
|---|---|---|
| `raven_vcn.bin` | AMD Video Core Next 1.0 (VCN 1) - Ryzen 2000 "Raven Ridge" APUs (PCI 1002:15DD, revision < 8) | |
| `picasso_vcn.bin` | VCN 1.0 - Ryzen 3000 mobile "Picasso" (PCI 1002:15D8, revision < 8) | |
| `raven2_vcn.bin` | VCN 1.0 - "Raven2" / "Dali" (PCI 1002:15DD or 15D8, revision >= 8) | |
| `vegam_uvd.bin` | Unified Video Decoder 6.3 - Radeon RX Vega M (PCI 1002:694C, 694E, 694F; amdgpu's VEGAM), firmware 1.130 | |
| `vegam_vce.bin` | Video Coding Engine 3.4 (H.264 encode) - Radeon RX Vega M (PCI 1002:694C, 694E, 694F; amdgpu's VEGAM), firmware 53.21 (binary id 3) | |
| `vegam_pfp.bin`, `vegam_me.bin`, `vegam_ce.bin` | Command processor (graphics) microcode: PFP, ME, CE - Radeon RX Vega M (amdgpu's VEGAM), versions 243, 164, 137 | Loaded by the SMC |
| `vegam_mec.bin`, `vegam_mec2.bin` | Command processor (compute) microcode, MEC 1 and 2, version 718 | `vegam_mec2.bin` only for the jump table entry |
| `vegam_rlc.bin` | Run list controller microcode, version 16 | Loaded by the SMC |
| `vegam_sdma.bin`, `vegam_sdma1.bin` | DMA engine microcode (SDMA 3.1), version 4 | Loaded by the SMC |
| `iwlwifi-9000-pu-b0-jf-b0-46.ucode` | Intel Wireless-AC 9560 (iwlwifi 9000 family, PU MAC with the JF radio; Dell XPS 15 9575), firmware core43, API 46 | The driver tries newer API numbers first and takes the first file it finds |
| `LICENSE.amdgpu` | The license these files are distributed under | Must stay with them |
| `LICENCE.iwlwifi_firmware` | The license the Intel file is distributed under | Must stay with it |

The three VCN 1 images are byte-identical in linux-firmware today (same SHA-256 below); they are kept under all three names because that is how the
driver names them and AMD may make them differ. The file for a machine is chosen the way the Linux amdgpu driver chooses it (`amdgpu_ucode_ip_version_decode`, soc15.c): revision >= 8 is
Raven2, otherwise device 15D8 is Picasso, otherwise Raven.

## Provenance

Copied unmodified from the linux-firmware repository (`amdgpu/raven_vcn.bin`, `amdgpu/picasso_vcn.bin`, `amdgpu/raven2_vcn.bin`,
`LICENSES/LICENSE.amdgpu`), https://gitlab.com/kernel-firmware/linux-firmware, which lists them in its WHENCE file as
"Redistributable. See LICENSE.amdgpu for details." SHA-256:

```
d376048d547b418effa4f357a4d1cddd418d101874f6a679144ab4df069f3aeb  picasso_vcn.bin
d376048d547b418effa4f357a4d1cddd418d101874f6a679144ab4df069f3aeb  raven2_vcn.bin
d376048d547b418effa4f357a4d1cddd418d101874f6a679144ab4df069f3aeb  raven_vcn.bin
e05664b587a1585d98db3097cf4a55119ee4af2da68e65d3d43b3f57065a95fb  vegam_uvd.bin
b063be1f8093b41f527eecc7c592ff92c06d1c9bd5d2b4560e9dcd065f49898a  vegam_vce.bin
3c088e0cf4dadfe067c525a63337387659e1d81d8a5896ba6b8cbc5b36762950  vegam_pfp.bin
c63ac86b770df364dc23c50ed6360a4823ad0e37120df12e4fb74bf574e3b536  vegam_me.bin
c0d4acb3f1bd1def58cee933103069ce2b6b6d9b6a63591bd811b51a05ac0d67  vegam_ce.bin
dbbe48008aaa667b64e2fbd7e93975200ae89b62aadaa6387b5ef14259701c20  vegam_mec.bin
97bd777ed7ad8cf6f2825dfdde1ed508c9f4b824db0c336b4e1d4ccc6c1136a5  vegam_mec2.bin
c729f89ade4fcd05c390f7bbf1fcb93eda47ac6911027c707add844181004b47  vegam_rlc.bin
9d964f4896b6713a1e41b8eca5ef665598290e4cc801d58fb2257845b7112040  vegam_sdma.bin
477f1eb3a796c1248f38fb98ff389e9bb49d7cae746556055484a60668f509a2  vegam_sdma1.bin
```

`vegam_uvd.bin` was added from the same repository (`amdgpu/vegam_uvd.bin`, listed in WHENCE as redistributable under LICENSE.amdgpu), also unmodified. `vegam_vce.bin` (`amdgpu/vegam_vce.bin`, the same WHENCE entry style) was fetched from the kernel.org linux-firmware tree and added unmodified too. The graphics engine files (`amdgpu/vegam_{pfp,me,ce,mec,mec2,rlc,sdma,sdma1}.bin`) come from the same tree, likewise unmodified.

`iwlwifi-9000-pu-b0-jf-b0-46.ucode` is `intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-46.ucode` of linux-firmware (the `linux-firmware-intel` 20260916-1 package), decompressed from its `.zst` and otherwise unmodified; `LICENCE.iwlwifi_firmware` is the license file of that package. SHA-256 (of the decompressed file, 1519568 bytes):

```
9a0c5d938646942f9798e6212579c25771e4bb44301cd409b82abe361ca2c3e0  iwlwifi-9000-pu-b0-jf-b0-46.ucode
```

It is read by `src/Hardware/WirelessInterface/Intel/IwlFirmware` (`IwlTest` with `IWL_FIRMWARE_FILE=firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode` takes it apart and damages it): 13 runtime and 9 init sections, 57 command versions, which include the versions the driver writes (ADD_STA 10, SCAN_REQ_UMAC 8, ADD_STA_KEY 1, TX_CMD 6, PHY_CONTEXT 2). The file lists the legacy commands under the long group (1); `IwlFirmware::command_version_of` looks there too.

## License

These files are AMD's, not part of TheoryCraftOS, and are not covered by the AGPL that covers the rest of the repository. They are redistributed
in binary form only, under `LICENSE.amdgpu` (reproduced here, as it requires). No reverse engineering, decompilation or disassembly is permitted.

The Intel file is Intel's, redistributed in binary form only under `LICENCE.iwlwifi_firmware` (reproduced here, as it requires): the copyright notice and disclaimer travel with it, Intel's name may not be used to endorse anything, and no reverse engineering, decompilation or disassembly is permitted.
