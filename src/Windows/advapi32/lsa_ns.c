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

/* advapi32.dll: the LSA functions of a domain (trusts, forests, claims, account protection) on a machine that is in no domain: they answer STATUS_NOT_SUPPORTED. */
#include <windows.h>
#define STATUS_NOT_SUPPORTED_ ((LONG)0xC00000BB)
#define PVOID void *
typedef LONG NTSTATUS_;

#define LSA_NOT_SUPPORTED(name, ...) __declspec(dllexport) NTSTATUS_ WINAPI name(__VA_ARGS__) { return STATUS_NOT_SUPPORTED_; }
LSA_NOT_SUPPORTED(LsaCreateTrustedDomain, PVOID a, PVOID b, DWORD c, PVOID d)
LSA_NOT_SUPPORTED(LsaOpenTrustedDomain, PVOID a, PVOID b, DWORD c, PVOID d)
LSA_NOT_SUPPORTED(LsaQueryInfoTrustedDomain, PVOID a, ULONG b, PVOID c)
LSA_NOT_SUPPORTED(LsaSetInformationTrustedDomain, PVOID a, ULONG b, PVOID c)
LSA_NOT_SUPPORTED(LsaQueryDomainInformationPolicy, PVOID a, ULONG b, PVOID c)
LSA_NOT_SUPPORTED(LsaSetDomainInformationPolicy, PVOID a, ULONG b, PVOID c)
LSA_NOT_SUPPORTED(LsaQueryForestTrustInformation, PVOID a, PVOID b, PVOID c)
LSA_NOT_SUPPORTED(LsaQueryForestTrustInformation2, PVOID a, PVOID b, ULONG c, PVOID d)
LSA_NOT_SUPPORTED(LsaSetForestTrustInformation, PVOID a, PVOID b, PVOID c, BOOLEAN d, PVOID e)
LSA_NOT_SUPPORTED(LsaSetForestTrustInformation2, PVOID a, PVOID b, ULONG c, PVOID d, ULONG e, BOOLEAN f, PVOID g)
LSA_NOT_SUPPORTED(LsaInvokeTrustScanner, PVOID a, PVOID b, PVOID c, PVOID d)
LSA_NOT_SUPPORTED(LsaGetAppliedCAPIDs, PVOID a, PVOID b, PVOID c)
LSA_NOT_SUPPORTED(LsaQueryCAPs, PVOID a, ULONG b, PVOID c, PVOID d)
LSA_NOT_SUPPORTED(LsaSetCAPs, PVOID a, ULONG b, ULONG c)
LSA_NOT_SUPPORTED(LsaGetDeviceRegistrationInfo, PVOID a, PVOID b)
LSA_NOT_SUPPORTED(LsaEnableUserArso, PVOID a)
LSA_NOT_SUPPORTED(LsaDisableUserArso, PVOID a)
LSA_NOT_SUPPORTED(LsaIsUserArsoAllowed, PVOID a, PVOID b)
LSA_NOT_SUPPORTED(LsaIsUserArsoEnabled, PVOID a, PVOID b)
LSA_NOT_SUPPORTED(LsaICLookupNames, PVOID a, ULONG b, PVOID c, PVOID d, PVOID e)
LSA_NOT_SUPPORTED(LsaICLookupNamesWithCreds, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
LSA_NOT_SUPPORTED(LsaICLookupSids, PVOID a, ULONG b, PVOID c, PVOID d, PVOID e)
LSA_NOT_SUPPORTED(LsaICLookupSidsWithCreds, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
LSA_NOT_SUPPORTED(LsaManageSidNameMapping, ULONG a, PVOID b, PVOID c)

