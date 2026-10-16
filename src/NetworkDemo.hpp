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

#pragma once
#include "hardware/NetworkInterface/INetworkInterface.hpp"
#include "uefi.hpp"

// Sends an ARP "who has 10.0.2.2?" request out of the interface (the gateway
// address of QEMU's built-in user-mode network) and waits up to two seconds for
// the answer, logging what it sees. Returns true if the gateway answered, which
// shows that both sending and receiving work. On a network that is not
// 10.0.2.0/24 there is simply no answer.
bool run_network_probe(hardware::INetworkInterface& nic, efi::SystemTable* st);
