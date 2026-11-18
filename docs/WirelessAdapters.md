# Wi-Fi adapters

The system knows the popular Wi-Fi adapters of Intel, Realtek, MediaTek, Qualcomm and Broadcom by their PCI and USB IDs (319 entries), and has a chip
driver for each common family. Only the Intel Wireless-AC 9000 series has the full stack, from firmware start through scanning and joining a network to
WPA2 (`src/Hardware/WirelessInterface/Intel/Iwl*`). The other chip drivers bring a chip as far as identifying it and loading or checking its firmware. None
of this has run on real hardware.

## Layout

`src/Hardware/WirelessInterface/`:

| File | What it is |
| --- | --- |
| `WifiAdapterModel.hpp` | One adapter: vendor, bus, IDs, driver family (named after the Linux driver it follows), standard, streams, firmware file |
| `WifiAdapterCatalog.*` | Looks up an adapter across every vendor's table (`<vendor>_wifi_models()`) |
| `IWifiChip.hpp` | The lower half of a driver: `start(firmware)`, state, chip ID and revision, MAC address, firmware name. `WifiChipBus` gives it a register window (PCIe BAR or a vendor's USB register wrapper) or an `IWifiSdio` |
| `WifiChipFactory.*` | `make_wifi_chip(model, bus, allocator)`, which passes the model to the vendor's `make_<vendor>_wifi_chip` |
| `WifiFirmwareSource.*` | `IWifiFirmwareSource` and `FileSystemFirmwareSource` (`/firmware/<name>`, names that leave the folder are refused) |
| `WifiAdapterProbe.*` | `find_wifi_adapters(pci, usb)` (reads IDs only), `WifiChipSlot` (maps the BAR or hands over the USB port, then starts the chip) |
| `Intel/`, `Realtek/`, `MediaTek/`, `Qualcomm/`, `Broadcom/` | Each vendor's table, firmware parsers, register layers and chip classes |

Firmware files go in `/firmware` under the relative names linux-firmware uses, so a copy of linux-firmware can be dropped in as it is. Examples:
`rtw88/rtw8822c_fw.bin`, `mediatek/WIFI_RAM_CODE_MT7961_1.bin`, `ath10k/QCA6174/hw3.0/firmware-6.bin`, `brcm/brcmfmac43455-sdio.bin`, and Intel's
`iwlwifi-*.ucode` at the top level.

## Status per family

| Family | Chips | How far `start()` gets |
| --- | --- | --- |
| Intel 9000 (iwlwifi mvm) | 9260, 9461/9462, 9560 | Full driver (`IwlWirelessInterface`, `-DTCOS_INTEL_WIFI=ON`) |
| Intel 7000/8000, 22000, AX210, BE200 | 7260–3168, 8260/8265, AX200/AX201, AX210/AX211/AX411, BE200/BE201 | Identified, firmware named (iwl-drv.c rules) and checked; boot not written |
| Realtek rtw88 | 8822CE/BE, 8821CE, 8723DE (+USB) | Power-on sequence, eFuse and MAC address, firmware checked; 8723D downloads its firmware; H2C commands and the DDMA download not written |
| Realtek rtw89 | 8852AE/BE/CE, 8851BE, 8922AE | Identified, multi-firmware file parsed and checked; power-on and download not written |
| Realtek rtl8xxxu, 8812AU, rtlwifi | USB and older PCIe | Listed in the table; 8812A/8821A get as far as Identified |
| MediaTek mt7921/mt7922 | MT7921, RZ608, RZ616 (PCIe) | Driver/firmware ownership handshake, WFDMA, ROM patch and RAM download over the MCU, reaching FirmwareLoaded; the capability query (MAC address) is not written |
| MediaTek USB (mt7921u, mt76x2u, mt76x0u, mt7601u) | USB dongles | Identified over vendor requests, firmware parsed |
| MediaTek mt7925, mt7615 | MT7925/RZ717 PCIe, MT7615/7663 | Listed in the table only |
| Qualcomm ath10k | QCA6174, QCA9377, QCA988x | Reset, chip ID, BMI download of firmware, board data (chosen by PCI subsystem IDs) and OTP, then BMI_DONE; HTC/WMI not written |
| Qualcomm ath11k/ath12k | WCN6855, QCA6390, QCN9074, WCN7850, QCN9274 | Hardware version read, `amss.bin` and `board-2.bin` checked, SBL over BHI, MHI READY reached; BHIe and QMI not written |
| Qualcomm ath9k, ath9k_htc | AR92xx–QCA9565, AR9271 | Listed in the table only |
| Broadcom brcmfmac | BCM4350–4387 (PCIe), 43430/43455/43439 (SDIO) | ChipID, EROM scan, firmware/NVRAM/CLM names, ARM reset, firmware + NVRAM written into RAM, shared structure parsed, reaching FirmwareLoaded; the FullMAC commands (MAC address, CLM upload) are not written |
| Broadcom brcmsmac, b43, wl | Older SoftMAC parts, BCM4352/4360 | Listed in the table only (wl-only chips have no open firmware interface) |

## At boot

Every boot lists the adapters it finds in the boot log. This reads IDs only. With `-DTCOS_WIFI_CHIPS=ON`, each listed adapter's chip driver is also started
and the outcome is logged. That switch is off by default because none of these drivers has run on real hardware. The Intel 9000 series is left to
`TCOS_INTEL_WIFI`.

## Tests

```bash
tests/run_host_tests.sh WifiAdaptersTest RealtekWifiTest MediaTekWifiTest QualcommWifiTest BroadcomWifiTest
```

Each test checks its parsers against synthetic files with truncation and bit-flip cases, and runs its chip drivers against register or backplane models.
Real (decompressed) linux-firmware files can be added through environment variables: `IWL_FIRMWARE_DIR`, `RTW88_FIRMWARE_FILES` / `RTW89_FIRMWARE_FILES`
(`:`-separated), `MEDIATEK_FIRMWARE_DIR`, `BRCMF_FIRMWARE_DIR`, and `ATH10K_FIRMWARE_FILE`, `ATH10K_BOARD_FILE`, `ATH11K_BOARD_FILE`, `ATH12K_BOARD_FILE`, `ATH12K_FIRMWARE_FILE`, `ATH_AMSS_FILE`.
All parsers were run against the files in this machine's `/usr/lib/firmware`.

## Not verified

Register offsets, power sequences and other hardware details come from the Linux drivers as remembered, not checked against their source. Each file marks
the values it is least sure of. Nothing listed here has been tried on an adapter.
