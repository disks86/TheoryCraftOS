/*
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

// The registers of the Realtek PCI-E card reader (RTS5249 family; the RTS525A in the Dell XPS 15 9575 is PCI 10EC:525A), from Linux's include/linux/rtsx_pci.h. Two spaces:
//  - the host registers in BAR 0 (command and data buffer addresses and control, the register access window, the interrupt status): `rtsx::kHost...`;
//  - the chip's own registers (0xFxxx), reached through the command queue (many in one go) or the register window (one at a time): `rtsx::k...`.
#pragma once
#include <stdint.h>

namespace hardware {

class IRtsxRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

 protected:
  constexpr IRtsxRegisters() = default;
  ~IRtsxRegisters() = default;
};

namespace rtsx {

// ---- host registers (BAR 0) ----
constexpr uint32_t kHcbar = 0x00, kHcbctlr = 0x04, kHdbar = 0x08, kHdbctlr = 0x0C, kHaimr = 0x10, kBipr = 0x14, kBier = 0x18;
constexpr uint32_t kStartCmd = 1u << 31, kHardwareResponse = 0x40000000u, kStopCmd = 1u << 28;
constexpr uint32_t kTrigDma = 1u << 31, kAdmaMode = 0x02u << 26, kStopDma = 1u << 28;
constexpr uint32_t kHaimrWriteStart = 0xC0000000u, kHaimrReadStart = 0x80000000u, kHaimrTransEnd = 0x80000000u;
// BIPR / BIER bits
constexpr uint32_t kCmdDoneInt = 1u << 31, kDataDoneInt = 1u << 30, kTransOkInt = 1u << 29, kTransFailInt = 1u << 28, kSdInt = 1u << 25, kSdOcInt = 1u << 22;
constexpr uint32_t kSdWriteProtect = 1u << 19, kSdExist = 1u << 16;
// ADMA descriptors (64 bit): address << 32 | length << 12 | options
constexpr uint32_t kSgValid = 0x01, kSgEnd = 0x02, kSgTransData = 0x20;
constexpr uint32_t kHostCommandBytes = 1024, kHostSgBytes = 3072, kReservedBytes = 4096, kMaxSgLength = 0x80000;

// ---- command queue words: type << 30 | address << 16 | mask << 8 | data ----
constexpr uint32_t kReadRegCmd = 0, kWriteRegCmd = 1, kCheckRegCmd = 2;

// ---- the chip's registers ----
constexpr uint16_t kSdCfg1 = 0xFDA0, kSdCfg2 = 0xFDA1, kSdCfg3 = 0xFDA2, kSdStat1 = 0xFDA3, kSdStat2 = 0xFDA4, kSdBusStat = 0xFDA5, kSdPadCtl = 0xFDA6, kSdSamplePointCtl = 0xFDA7,
                   kSdPushPointCtl = 0xFDA8, kSdCmd0 = 0xFDA9, kSdCmd1 = 0xFDAA, kSdCmd2 = 0xFDAB, kSdCmd3 = 0xFDAC, kSdCmd4 = 0xFDAD, kSdByteCntL = 0xFDAF, kSdByteCntH = 0xFDB0,
                   kSdBlockCntL = 0xFDB1, kSdBlockCntH = 0xFDB2, kSdTransfer = 0xFDB3;
constexpr uint16_t kCardClkEn = 0xFD69, kCardSelect = 0xFD5C, kCardShareMode = 0xFD52, kCardStop = 0xFD54, kCardOe = 0xFD55, kCardPwrCtl = 0xFD50, kCardDataSource = 0xFD5B,
                   kCardDriveSel = 0xFD53, kCardPullCtl1 = 0xFD60, kCardPullCtl2 = 0xFD61, kCardPullCtl3 = 0xFD62, kCardPullCtl4 = 0xFD63;
constexpr uint16_t kSd30ClkDriveSel = 0xFD5A, kSd30CmdDriveSel = 0xFD5E, kSd30DatDriveSel = 0xFD5F;
constexpr uint16_t kClkCtl = 0xFC02, kClkDiv = 0xFC03, kSsDivN0 = 0xFC0F, kSsCtl1 = 0xFC11, kSsCtl2 = 0xFC12, kFpdctl = 0xFC00, kOltLedCtl = 0xFC1E, kGpioCtl = 0xFC1F;
constexpr uint16_t kPwrGateCtrl = 0xFE75, kLdoPwrSel = 0xFE78, kLdoConfig2 = 0xFF71, kLdoVccCfg1 = 0xFF73;
constexpr uint16_t kDmatc3 = 0xFE28, kDmatc2 = 0xFE29, kDmatc1 = 0xFE2A, kDmatc0 = 0xFE2B, kDmactl = 0xFE2C, kRbctl = 0xFE34, kIrqstat0 = 0xFE21;
constexpr uint16_t kPpbufBase2 = 0xFA00;                           // the 512 byte ping-pong buffer (also where a long response lands)

// bits
constexpr uint8_t kSdClkDivideMask = 0xC0, kSdClkDivide0 = 0x00, kSdClkDivide128 = 0x80, kSdBusWidthMask = 0x03, kSdBusWidth1 = 0x00, kSdBusWidth4 = 0x01;
constexpr uint8_t kSdModeSelectMask = 0x0C, kSd20Mode = 0x00, kSdAsyncFifoNotRst = 0x10;
constexpr uint8_t kSdRspLen0 = 0x00, kSdRspLen6 = 0x01, kSdNoCalculateCrc7 = 0x80, kSdNoCheckCrc7 = 0x04, kSdNoWaitBusyEnd = 0x00, kSdNoCheckWaitCrcTo = 0x20;
constexpr uint8_t kSdRspTypeR0 = 0x04, kSdRspTypeR1 = 0x01, kSdRspTypeR1b = 0x09, kSdRspTypeR2 = 0x02, kSdRspTypeR3 = 0x05;
constexpr uint8_t kSdCrc7Err = 0x80, kSdCmdStart = 0x40;
constexpr uint8_t kSdTransferStart = 0x80, kSdTransferEnd = 0x40, kSdStatIdle = 0x20, kSdTransferErr = 0x10;
constexpr uint8_t kSdTmNormalWrite = 0x00, kSdTmAutoWrite3 = 0x01, kSdTmAutoRead2 = 0x0E, kSdTmCmdRsp = 0x08, kSdTmNormalRead = 0x0C;
constexpr uint8_t kSdClkToggleEn = 0x80, kSdClkForceStop = 0x40;
constexpr uint8_t kSdStop = 0x04, kSdClrErr = 0x40;
constexpr uint8_t kSdClkEn = 0x04, kSdModSel = 2, kCardShareMask = 0x0F, kCardShare48Sd = 0x04, kSdOutputEn = 0x04;
constexpr uint8_t kPingpongBuffer = 0x01, kRingBuffer = 0x00;
constexpr uint8_t kSdPowerMask = 0x03, kSdVccPartialPowerOn = 0x02, kSdVccPowerOn = 0x00, kSdPowerOff = 0x03, kLdo3318PwrMask = 0x06;
constexpr uint8_t kLdoVccTuneMask = 0x07, kLdoVcc3v3 = 0x07;
constexpr uint8_t kChangeClk = 0x01, kClkLowFreq = 0x01, kSscRstb = 0x80, kSscDepthMask = 0x07, kSscPowerDown = 0x01;
constexpr uint8_t kDmaEn = 0x01, kDmaDirToCard = 0x00, kDmaDirFromCard = 0x02, kDma512 = 0x20, kDmaPackSizeMask = 0x30, kDmaRst = 0x80, kRbFlush = 0x80;
constexpr uint8_t kDmaDoneInt = 0x80;
constexpr uint8_t kSd20Tx14Ahead = 0x10, kSd20RxPosEdge = 0x00, kSd20RxSelMask = 0x08;

}  // namespace rtsx
}  // namespace hardware
