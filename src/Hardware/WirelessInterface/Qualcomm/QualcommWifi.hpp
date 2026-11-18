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

// The Qualcomm Atheros chip drivers behind WifiChipFactory: ath10k (Ath10kChip) for the 802.11ac PCIe chips, ath11k/ath12k (Ath11kChip) for the 802.11ax/be
// ones. ath9k (no firmware, a host-side MAC) and ath9k_htc (USB) have no chip driver here yet: make_qualcomm_wifi_chip returns null for them.
#pragma once
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath10kChip.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath11kChip.hpp"
