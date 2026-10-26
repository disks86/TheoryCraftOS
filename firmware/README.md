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
| `LICENSE.amdgpu` | The license these files are distributed under | Must stay with them |

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
```

`vegam_uvd.bin` was added from the same repository (`amdgpu/vegam_uvd.bin`, listed in WHENCE as redistributable under LICENSE.amdgpu), also unmodified. `vegam_vce.bin` (`amdgpu/vegam_vce.bin`, the same WHENCE entry style) was fetched from the kernel.org linux-firmware tree and added unmodified too.

## License

These files are AMD's, not part of TheoryCraftOS, and are not covered by the AGPL that covers the rest of the repository. They are redistributed
in binary form only, under `LICENSE.amdgpu` (reproduced here, as it requires). No reverse engineering, decompilation or disassembly is permitted.
