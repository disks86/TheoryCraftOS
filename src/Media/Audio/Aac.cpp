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

// AAC-LC decoder (MPEG-2/4 Advanced Audio Coding, Low Complexity profile) and an ADTS reader around it.
//
// Implemented: all the LC coding tools - spectral Huffman codebooks 1-11 with escapes, scalefactors, section and window-group
// handling, short-block sequences with sine and Kaiser-Bessel-derived windows, pulse data, temporal noise shaping, mid/side
// and intensity stereo, perceptual noise substitution - through to PCM. Channels beyond the front pair are decoded but not
// output. The Huffman code lengths and codewords and the scalefactor-band widths are the ones in ISO/IEC 14496-3, here taken
// from libxaac (Ittiam Systems, Apache License 2.0); everything else is computed.
// Not handled: Main/LTP prediction, scalable and SSR profiles, coupling channels, gain control, SBR/PS (an HE-AAC stream
// decodes as its AAC-LC core at the core sampling rate), error-resilient and other non-LC object types.
#include "Math/FloatMath.hpp"
#include "Media/Audio/AacInternal.hpp"
#include "Media/Audio/Mdct.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"

namespace media {
namespace aac {

namespace {
constexpr uint8_t kWidthsL96[41] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 12, 12, 12, 12, 12, 16, 16, 24, 28, 36, 44, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};
constexpr uint8_t kWidthsS96[12] = {4, 4, 4, 4, 4, 4, 8, 8, 8, 16, 28, 36};
constexpr uint8_t kWidthsL64[47] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 16, 16, 16, 20, 24, 24, 28, 36, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40};
constexpr uint8_t kWidthsL48[49] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 12, 12, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 96};
constexpr uint8_t kWidthsS48[14] = {4, 4, 4, 4, 4, 8, 8, 8, 12, 12, 12, 16, 16, 16};
constexpr uint8_t kWidthsL32[51] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 12, 12, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32};
constexpr uint8_t kWidthsL24[47] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 12, 12, 12, 12, 16, 16, 16, 20, 20, 24, 24, 28, 28, 32, 36, 36, 40, 44, 48, 52, 52, 64, 64, 64, 64, 64};
constexpr uint8_t kWidthsS24[15] = {4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 12, 12, 16, 16, 20};
constexpr uint8_t kWidthsL16[43] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 20, 20, 20, 24, 24, 28, 28, 32, 36, 40, 40, 44, 48, 52, 56, 60, 64, 64, 64};
constexpr uint8_t kWidthsS16[15] = {4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 12, 12, 16, 20, 20};
constexpr uint8_t kWidthsL8[40] = {12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 20, 20, 20, 20, 24, 24, 24, 28, 28, 32, 36, 36, 40, 44, 48, 52, 56, 60, 64, 80};
constexpr uint8_t kWidthsS8[15] = {4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 12, 16, 20, 20};
constexpr uint8_t kLen1[81] = {11, 9, 11, 10, 7, 10, 11, 9, 11, 10, 7, 10, 7, 5, 7, 9, 7, 10, 11, 9, 11, 9, 7, 9, 11, 9, 11, 9, 7, 9, 7, 5, 7, 9, 7, 9, 7, 5, 7, 5, 1, 5, 7, 5, 7, 9, 7, 9, 7, 5, 7, 9, 7, 9, 11, 9, 11, 9, 7, 9, 11, 9, 11, 10, 7, 9, 7, 5, 7, 9, 7, 10, 11, 9, 11, 10, 7, 9, 11, 9, 11};
constexpr uint32_t kCode1[81] = {2040, 497, 2045, 1013, 104, 1008, 2039, 492, 2037, 1009, 114, 1012, 116, 17, 118, 491, 108, 1014, 2044, 481, 2033, 496, 97, 502, 2034, 490, 2043, 498, 105, 493, 119, 23, 111, 486, 100, 485, 103, 21, 98, 18, 0, 20, 101, 22, 109, 489, 99, 484, 107, 19, 113, 483, 112, 499, 2046, 487, 2035, 495, 96, 494, 2032, 482, 2042, 1011, 106, 488, 117, 16, 115, 500, 110, 1015, 2038, 480, 2041, 1010, 102, 501, 2047, 503, 2036};
constexpr uint8_t kLen2[81] = {9, 7, 9, 8, 6, 8, 9, 8, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 9, 7, 8, 8, 6, 8, 9, 7, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 6, 5, 6, 5, 3, 5, 6, 5, 6, 8, 6, 7, 6, 5, 6, 8, 6, 8, 9, 7, 9, 8, 6, 8, 8, 7, 9, 8, 6, 7, 6, 4, 6, 8, 6, 7, 9, 7, 9, 7, 6, 8, 9, 7, 9};
constexpr uint32_t kCode2[81] = {499, 111, 509, 235, 35, 234, 503, 232, 506, 242, 45, 112, 32, 6, 43, 110, 40, 233, 505, 102, 248, 231, 27, 241, 500, 107, 501, 236, 42, 108, 44, 10, 39, 103, 26, 245, 36, 8, 31, 9, 0, 7, 29, 11, 48, 239, 28, 100, 30, 12, 41, 243, 47, 240, 508, 113, 498, 244, 33, 230, 247, 104, 504, 238, 34, 101, 49, 2, 38, 237, 37, 106, 507, 114, 510, 105, 46, 246, 511, 109, 502};
constexpr uint8_t kLen3[81] = {1, 4, 8, 4, 5, 8, 9, 9, 10, 4, 6, 9, 6, 6, 9, 9, 9, 10, 9, 10, 13, 9, 9, 11, 11, 10, 12, 4, 6, 10, 6, 7, 10, 10, 10, 12, 5, 7, 11, 6, 7, 10, 9, 9, 11, 9, 10, 13, 8, 9, 12, 10, 11, 12, 8, 10, 15, 9, 11, 15, 13, 14, 16, 8, 10, 14, 9, 10, 14, 12, 12, 15, 11, 12, 16, 10, 11, 15, 12, 12, 15};
constexpr uint32_t kCode3[81] = {0, 9, 239, 11, 25, 240, 491, 486, 1010, 10, 53, 495, 52, 55, 489, 493, 487, 1011, 494, 1005, 8186, 492, 498, 2041, 2040, 1016, 4088, 8, 56, 1014, 54, 117, 1009, 1003, 1004, 4084, 24, 118, 2036, 57, 116, 1007, 499, 500, 2038, 488, 1002, 8188, 242, 497, 4091, 1013, 2035, 4092, 238, 1015, 32766, 496, 2037, 32765, 8187, 16378, 65535, 241, 1008, 16380, 490, 1006, 16379, 4086, 4090, 32764, 2034, 4085, 65534, 1012, 2039, 32763, 4087, 4089, 32762};
constexpr uint8_t kLen4[81] = {4, 5, 8, 5, 4, 8, 9, 8, 11, 5, 5, 8, 5, 4, 8, 8, 7, 10, 9, 8, 11, 8, 8, 10, 11, 10, 11, 4, 5, 8, 4, 4, 8, 8, 8, 10, 4, 4, 8, 4, 4, 7, 8, 7, 9, 8, 8, 10, 7, 7, 9, 10, 9, 10, 8, 8, 11, 8, 7, 10, 11, 10, 12, 8, 7, 10, 7, 7, 9, 10, 9, 11, 11, 10, 12, 10, 9, 11, 11, 10, 11};
constexpr uint32_t kCode4[81] = {7, 22, 246, 24, 8, 239, 495, 243, 2040, 25, 23, 237, 21, 1, 226, 240, 112, 1008, 494, 241, 2042, 238, 228, 1010, 2038, 1007, 2045, 5, 20, 242, 9, 4, 229, 244, 232, 1012, 6, 2, 231, 3, 0, 107, 227, 105, 499, 235, 230, 1014, 110, 106, 500, 1004, 496, 1017, 245, 236, 2043, 234, 111, 1015, 2041, 1011, 4095, 233, 109, 1016, 108, 104, 501, 1006, 498, 2036, 2039, 1009, 4094, 1005, 497, 2037, 2046, 1013, 2044};
constexpr uint8_t kLen5[81] = {13, 12, 11, 11, 10, 11, 11, 12, 13, 12, 11, 10, 9, 8, 9, 10, 11, 12, 12, 10, 9, 8, 7, 8, 9, 10, 11, 11, 9, 8, 5, 4, 5, 8, 9, 11, 10, 8, 7, 4, 1, 4, 7, 8, 11, 11, 9, 8, 5, 4, 5, 8, 9, 11, 11, 10, 9, 8, 7, 8, 9, 10, 11, 12, 11, 10, 9, 8, 9, 10, 11, 12, 13, 12, 12, 11, 10, 10, 11, 12, 13};
constexpr uint32_t kCode5[81] = {8191, 4087, 2036, 2024, 1009, 2030, 2041, 4088, 8189, 4093, 2033, 1000, 488, 240, 492, 1006, 2034, 4090, 4084, 1007, 498, 232, 112, 236, 496, 1002, 2035, 2027, 491, 234, 26, 8, 25, 238, 495, 2029, 1008, 242, 115, 11, 0, 10, 113, 243, 2025, 2031, 494, 239, 24, 9, 27, 235, 489, 2028, 2038, 1003, 499, 237, 114, 233, 497, 1005, 2039, 4086, 2032, 1001, 493, 241, 490, 1004, 2040, 4089, 8188, 4092, 4085, 2026, 1011, 1010, 2037, 4091, 8190};
constexpr uint8_t kLen6[81] = {11, 10, 9, 9, 9, 9, 9, 10, 11, 10, 9, 8, 7, 7, 7, 8, 9, 10, 9, 8, 6, 6, 6, 6, 6, 8, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 8, 6, 6, 6, 6, 6, 8, 9, 10, 9, 8, 7, 7, 7, 7, 8, 10, 11, 10, 9, 9, 9, 9, 9, 10, 11};
constexpr uint32_t kCode6[81] = {2046, 1021, 497, 491, 500, 490, 496, 1020, 2045, 1014, 485, 234, 108, 113, 104, 240, 486, 1015, 499, 239, 50, 39, 40, 38, 49, 235, 503, 488, 111, 46, 8, 4, 6, 41, 107, 494, 495, 114, 45, 2, 0, 3, 47, 115, 506, 487, 110, 43, 7, 1, 5, 44, 109, 492, 505, 238, 48, 36, 42, 37, 51, 236, 498, 1016, 484, 237, 106, 112, 105, 116, 241, 1018, 2047, 1017, 502, 493, 504, 489, 501, 1019, 2044};
constexpr uint8_t kLen7[64] = {1, 3, 6, 7, 8, 9, 10, 11, 3, 4, 6, 7, 8, 8, 9, 9, 6, 6, 7, 8, 8, 9, 9, 10, 7, 7, 8, 8, 9, 9, 10, 10, 8, 8, 9, 9, 10, 10, 10, 11, 9, 8, 9, 9, 10, 10, 11, 11, 10, 9, 9, 10, 10, 11, 12, 12, 11, 10, 10, 10, 11, 11, 12, 12};
constexpr uint32_t kCode7[64] = {0, 5, 55, 116, 242, 491, 1005, 2039, 4, 12, 53, 113, 236, 238, 494, 501, 54, 52, 114, 234, 241, 489, 499, 1013, 115, 112, 235, 240, 497, 496, 1004, 1018, 243, 237, 488, 495, 1007, 1009, 1017, 2043, 493, 239, 490, 498, 1011, 1016, 2041, 2044, 1006, 492, 500, 1012, 1015, 2040, 4093, 4094, 2038, 1008, 1010, 1014, 2042, 2045, 4092, 4095};
constexpr uint8_t kLen8[64] = {5, 4, 5, 6, 7, 8, 9, 10, 4, 3, 4, 5, 6, 7, 7, 8, 5, 4, 4, 5, 6, 7, 7, 8, 6, 5, 5, 6, 6, 7, 8, 8, 7, 6, 6, 6, 7, 7, 8, 9, 8, 7, 6, 7, 7, 8, 8, 10, 9, 7, 7, 8, 8, 8, 9, 9, 10, 8, 8, 8, 9, 9, 9, 10};
constexpr uint32_t kCode8[64] = {14, 5, 16, 48, 111, 241, 506, 1022, 3, 0, 4, 18, 44, 106, 117, 248, 15, 2, 6, 20, 46, 105, 114, 245, 47, 17, 19, 42, 50, 108, 236, 250, 113, 43, 45, 49, 109, 112, 242, 505, 239, 104, 51, 107, 110, 238, 249, 1020, 504, 116, 115, 237, 240, 246, 502, 509, 1021, 243, 244, 247, 503, 507, 508, 1023};
constexpr uint8_t kLen9[169] = {1, 3, 6, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13, 3, 4, 6, 7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 6, 6, 7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 12, 8, 7, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 13, 9, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 10, 9, 9, 10, 11, 11, 11, 12, 11, 12, 12, 13, 13, 11, 9, 10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 13, 13, 11, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 13, 14, 11, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 14, 14, 12, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 12, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 15, 15, 13, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15};
constexpr uint32_t kCode9[169] = {0, 5, 55, 231, 478, 974, 985, 1992, 1997, 4040, 4061, 8164, 8172, 4, 12, 53, 114, 234, 237, 482, 977, 979, 992, 2008, 4047, 4053, 54, 52, 113, 232, 236, 481, 975, 989, 987, 2000, 4039, 4052, 4068, 230, 112, 233, 477, 483, 978, 988, 1996, 1994, 2014, 4056, 4074, 8155, 479, 235, 476, 486, 981, 990, 1995, 2013, 2012, 4045, 4066, 4071, 8161, 976, 480, 484, 982, 1989, 2001, 2011, 4050, 2016, 4057, 4075, 8163, 8169, 1988, 485, 983, 1990, 1999, 2010, 4043, 4058, 4067, 4073, 8166, 8179, 8183, 2003, 984, 993, 2004, 2009, 4051, 4062, 8157, 8153, 8162, 8170, 8177, 8182, 2002, 980, 986, 1991, 2007, 2018, 4046, 4059, 8152, 8174, 16368, 8180, 16370, 2017, 991, 1993, 2006, 4042, 4048, 4069, 4070, 8171, 8175, 16371, 16372, 16373, 4064, 1998, 2005, 4038, 4049, 4065, 8160, 8168, 8176, 16369, 16376, 16374, 32764, 4072, 2015, 4041, 4055, 4060, 8156, 8159, 8173, 8181, 16377, 16379, 32765, 32766, 8167, 4044, 4054, 4063, 8158, 8154, 8165, 8178, 16378, 16375, 16380, 16381, 32767};
constexpr uint8_t kLen10[169] = {6, 5, 6, 6, 7, 8, 9, 10, 10, 10, 11, 11, 12, 5, 4, 4, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 6, 4, 5, 5, 6, 6, 7, 8, 8, 9, 9, 10, 10, 6, 5, 5, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10, 7, 6, 6, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 8, 7, 6, 7, 7, 7, 8, 8, 8, 9, 10, 10, 11, 9, 7, 7, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11, 9, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 9, 8, 8, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 11, 11, 12, 10, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 12, 11, 10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 11, 10, 10, 10, 10, 10, 10, 11, 11, 12, 12, 12, 12};
constexpr uint32_t kCode10[169] = {34, 8, 29, 38, 95, 211, 463, 976, 983, 1005, 2032, 2038, 4093, 7, 0, 1, 9, 32, 84, 96, 213, 220, 468, 973, 990, 2023, 28, 2, 6, 12, 30, 40, 91, 205, 217, 462, 476, 985, 1009, 37, 11, 10, 13, 36, 87, 97, 204, 221, 460, 478, 979, 999, 93, 33, 31, 35, 39, 89, 100, 216, 223, 466, 482, 989, 1006, 209, 85, 41, 86, 88, 98, 206, 224, 226, 474, 980, 995, 2027, 457, 94, 90, 92, 99, 202, 218, 455, 458, 480, 987, 1000, 2028, 483, 210, 203, 208, 215, 219, 454, 469, 472, 970, 986, 2026, 2033, 481, 212, 207, 214, 222, 225, 464, 470, 977, 981, 1010, 2030, 2043, 1001, 461, 456, 459, 465, 471, 479, 975, 992, 1007, 2022, 2040, 4090, 1003, 477, 467, 473, 475, 978, 972, 988, 1002, 2029, 2035, 2041, 4089, 2034, 974, 484, 971, 984, 982, 994, 997, 2024, 2036, 2037, 2039, 4091, 2042, 1004, 991, 993, 996, 998, 1008, 2025, 2031, 4088, 4094, 4092, 4095};
constexpr uint8_t kLen11[289] = {4, 5, 6, 7, 8, 8, 9, 10, 10, 10, 11, 11, 12, 11, 12, 12, 10, 5, 4, 5, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10, 11, 8, 6, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 7, 6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 10, 8, 10, 9, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 8, 10, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 8, 11, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 10, 11, 11, 8, 11, 10, 9, 9, 10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 9, 11, 10, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 12, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 5};
constexpr uint32_t kCode11[289] = {0, 6, 25, 61, 156, 198, 423, 912, 962, 991, 2022, 2035, 4091, 2028, 4090, 4094, 910, 5, 1, 8, 20, 55, 66, 146, 175, 401, 421, 437, 926, 960, 930, 973, 2006, 174, 23, 7, 9, 24, 57, 64, 142, 163, 184, 409, 428, 449, 945, 918, 958, 970, 157, 60, 21, 22, 26, 59, 68, 145, 165, 190, 406, 430, 441, 929, 913, 933, 981, 148, 154, 54, 56, 58, 65, 140, 155, 176, 195, 414, 427, 444, 927, 911, 937, 975, 147, 191, 62, 63, 67, 69, 158, 167, 185, 404, 418, 442, 451, 934, 935, 955, 980, 159, 416, 143, 141, 144, 152, 166, 182, 196, 415, 431, 447, 921, 959, 948, 969, 999, 168, 438, 171, 164, 170, 178, 194, 197, 408, 420, 440, 908, 932, 964, 966, 989, 1000, 173, 943, 402, 189, 188, 398, 407, 410, 419, 433, 909, 920, 951, 979, 977, 987, 2013, 180, 990, 425, 411, 412, 417, 426, 429, 435, 907, 946, 952, 974, 993, 992, 2002, 2021, 183, 2019, 443, 424, 422, 432, 434, 439, 923, 922, 954, 949, 982, 2007, 996, 2008, 2026, 186, 2024, 928, 445, 436, 906, 452, 914, 938, 944, 956, 983, 2004, 2012, 2011, 2005, 2032, 193, 2043, 968, 931, 917, 925, 940, 942, 965, 984, 994, 998, 2020, 2023, 2016, 2025, 2039, 400, 2034, 915, 446, 448, 916, 919, 941, 963, 961, 978, 2010, 2009, 2015, 2027, 2036, 2042, 405, 2040, 957, 924, 939, 936, 947, 953, 976, 995, 997, 2018, 2014, 2029, 2033, 2041, 2044, 403, 4093, 988, 950, 967, 972, 971, 985, 986, 2003, 2017, 2030, 2031, 2037, 2038, 4092, 4095, 413, 450, 181, 161, 150, 151, 149, 153, 160, 162, 172, 169, 177, 179, 187, 192, 399, 4};
constexpr uint8_t kLenscl[121] = {18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 18, 19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 12, 12, 12, 11, 12, 11, 10, 10, 10, 9, 9, 8, 8, 8, 7, 6, 6, 5, 4, 3, 1, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19};
constexpr uint32_t kCodescl[121] = {262120, 262118, 262119, 262117, 524277, 524273, 524269, 524278, 524270, 524271, 524272, 524284, 524285, 524287, 524286, 524279, 524280, 524283, 524281, 262116, 524282, 262115, 131055, 131056, 65525, 131054, 65522, 65523, 65524, 65521, 32758, 32759, 16377, 16373, 16375, 16371, 16374, 16370, 8183, 8181, 4089, 4087, 4086, 2041, 4084, 2040, 1017, 1015, 1013, 504, 503, 250, 248, 246, 121, 58, 56, 26, 11, 4, 0, 10, 12, 27, 57, 59, 120, 122, 247, 249, 502, 505, 1012, 1014, 1016, 2037, 2036, 2038, 2039, 4085, 4088, 8180, 8182, 8184, 16376, 16372, 65520, 32756, 65526, 32757, 262114, 524249, 524250, 524251, 524252, 524253, 524254, 524248, 524242, 524243, 524244, 524245, 524246, 524274, 524255, 524263, 524264, 524265, 524266, 524267, 524262, 524256, 524257, 524258, 524259, 524260, 524261, 524247, 524268, 524276, 524275};


constexpr uint32_t kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
constexpr uint8_t kTnsMaxBandsLong[13] = {31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39};
constexpr uint8_t kTnsMaxBandsShort[13] = {9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14};

enum WindowSequence { OnlyLong = 0, LongStart = 1, EightShort = 2, LongStop = 3 };
enum BandType { ZeroBand = 0, NoiseBand = 13, IntensityBand2 = 14, IntensityBand = 15 };

struct Book {
  const uint8_t* lens;
  const uint32_t* codes;
  uint32_t count;
};
const Book kBooks[12] = {{kLen1, kCode1, 81}, {kLen2, kCode2, 81}, {kLen3, kCode3, 81}, {kLen4, kCode4, 81},   {kLen5, kCode5, 81},
                         {kLen6, kCode6, 81}, {kLen7, kCode7, 64}, {kLen8, kCode8, 64}, {kLen9, kCode9, 169}, {kLen10, kCode10, 169},
                         {kLen11, kCode11, 289}, {kLenscl, kCodescl, 121}};

struct Bits {
  ConstByteView d;
  uint64_t pos = 0;
  bool overrun = false;
  uint32_t get1() {
    const uint64_t byte = pos >> 3;
    if (byte >= d.size()) { overrun = true; return 0; }
    const uint32_t bit = (d[byte] >> (7 - (pos & 7))) & 1u;
    ++pos;
    return bit;
  }
  uint32_t get(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) v = (v << 1) | get1();
    return v;
  }
  void skip(uint64_t n) { pos += n; if (pos > d.size() * 8) overrun = true; }
  void align() { pos = (pos + 7) & ~uint64_t{7}; }
};

struct Tns {
  bool present = false;
  uint8_t n_filt[8] = {};
  uint8_t length[8][3] = {}, order[8][3] = {};
  bool direction[8][3] = {};
  float lpc[8][3][12] = {};
};

struct Pulse {
  bool present = false;
  uint8_t count = 0, start_sfb = 0, offset[4] = {}, amp[4] = {};
};

struct IcsInfo {
  uint8_t window_sequence = 0, window_shape = 0, max_sfb = 0, num_window_groups = 1, group_len[8] = {1};
  uint8_t num_windows = 1, num_swb = 0, tns_max_bands = 0;
  const uint8_t* widths = nullptr;  // band widths for this window type
  uint16_t swb_offset[53] = {};
  bool is_short() const { return window_sequence == EightShort; }
};

struct Channel {
  IcsInfo ics;
  uint8_t global_gain = 0;
  uint8_t band_type[8 * 51] = {};
  int sf[8 * 51] = {};          // scalefactor, noise energy, or intensity position, by band
  Tns tns;
  Pulse pulse;
  int16_t q[1024];
  float coef[1024];
};

struct ChannelState {
  float overlap[1024] = {};
  uint8_t prev_shape = 0;
};

float bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 60; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return static_cast<float>(sum);
}

}  // namespace

class Decoder {
 public:
  explicit Decoder(allocators::IAllocator& allocator) : allocator_(allocator), mdct_long_(allocator), mdct_short_(allocator) {}
  ~Decoder() { allocator_.deallocate(trees_); allocator_.deallocate(pow43_); }
  bool init(uint32_t sampling_index);
  uint32_t sample_rate() const { return kSampleRates[sampling_index_]; }
  // Decodes one raw_data_block into `pcm` (interleaved int16, 1024 frames) for `channels()` output channels. False if the block is
  // damaged or uses a tool that is not supported.
  bool decode_block(ConstByteView data, int16_t* pcm);
  uint32_t channels() const { return out_channels_; }
  void reset_state() {
    for (ChannelState& st : states_) st = ChannelState();
    random_ = 0x1F2E3D4C;
  }

 private:
  bool parse_ics_info(Bits& b, IcsInfo* ics);
  bool parse_channel(Bits& b, Channel* ch, const IcsInfo* common);
  bool parse_section_data(Bits& b, Channel* ch);
  bool parse_scalefactors(Bits& b, Channel* ch);
  bool parse_pulse(Bits& b, Channel* ch);
  bool parse_tns(Bits& b, Channel* ch);
  bool parse_spectrum(Bits& b, Channel* ch);
  int huff(Bits& b, int book);
  void dequantize(Channel* ch);
  void apply_tns(Channel* ch);
  void synthesize(Channel* ch, ChannelState* st, float* out);

  allocators::IAllocator& allocator_;
  Mdct mdct_long_, mdct_short_;
  uint32_t sampling_index_ = 4;
  int16_t* trees_ = nullptr;   // 12 binary trees, node pairs
  uint32_t tree_base_[13] = {};
  float* pow43_ = nullptr;
  float long_slope_[2][1024];
  float short_slope_[2][128];
  uint32_t random_ = 0x1F2E3D4C;
  uint32_t out_channels_ = 0;
  Channel ch_[2];
  ChannelState states_[16];
  int chosen_type_ = -1, chosen_tag_ = -1;
  bool ms_used_[8 * 51];
};

bool Decoder::init(uint32_t sampling_index) {
  if (sampling_index >= 13) return false;
  sampling_index_ = sampling_index;
  // Binary trees for the Huffman codes: node i has children at [2i], [2i+1]; a negative entry ~s is the symbol s, 0 is invalid.
  uint32_t total = 0;
  for (int i = 0; i < 12; ++i) { tree_base_[i] = total; total += kBooks[i].count * 2; }
  tree_base_[12] = total;
  trees_ = static_cast<int16_t*>(allocator_.allocate(sizeof(int16_t) * total));
  if (!trees_) return false;
  for (uint32_t i = 0; i < total; ++i) trees_[i] = 0;
  for (int bk = 0; bk < 12; ++bk) {
    int16_t* tree = trees_ + tree_base_[bk];
    uint32_t nodes = 1;
    for (uint32_t s = 0; s < kBooks[bk].count; ++s) {
      const uint32_t len = kBooks[bk].lens[s], code = kBooks[bk].codes[s];
      uint32_t node = 0;
      for (uint32_t k = 0; k < len; ++k) {
        const uint32_t bit = (code >> (len - 1 - k)) & 1;
        int16_t& child = tree[node * 2 + bit];
        if (k + 1 == len) {
          child = static_cast<int16_t>(~s);
        } else {
          if (child == 0) child = static_cast<int16_t>(nodes++);
          node = static_cast<uint32_t>(child);
        }
      }
    }
  }
  // x^(4/3) for the quantised values that can be looked up; larger ones (escapes) are computed.
  pow43_ = static_cast<float*>(allocator_.allocate(sizeof(float) * 8192));
  if (!pow43_) return false;
  for (int i = 0; i < 8192; ++i) pow43_[i] = static_cast<float>(fmath::pow(i, 4.0 / 3.0));
  // Window slopes: sine and Kaiser-Bessel-derived (alpha 4 for long, 6 for short windows).
  const double pi = 3.14159265358979323846;
  for (int i = 0; i < 1024; ++i) long_slope_[0][i] = static_cast<float>(fmath::sin(pi * (i + 0.5) / 2048.0));
  for (int i = 0; i < 128; ++i) short_slope_[0][i] = static_cast<float>(fmath::sin(pi * (i + 0.5) / 256.0));
  for (int shape = 1; shape < 2; ++shape) {
    for (int pass = 0; pass < 2; ++pass) {
      const int half = pass == 0 ? 1024 : 128;
      const double alpha = pass == 0 ? 4.0 : 6.0;
      float kaiser[1025];
      double total_sum = 0;
      for (int j = 0; j <= half; ++j) {
        const double r = 2.0 * j / half - 1.0;
        kaiser[j] = bessel_i0(pi * alpha * fmath::sqrt(1.0 - r * r));
        total_sum += kaiser[j];
      }
      double run = 0;
      for (int n = 0; n < half; ++n) {
        run += kaiser[n];
        const float w = static_cast<float>(fmath::sqrt(run / total_sum));
        if (pass == 0) long_slope_[1][n] = w; else short_slope_[1][n] = w;
      }
    }
  }
  return mdct_long_.init(2048) && mdct_short_.init(256);
}

int Decoder::huff(Bits& b, int book) {
  const int16_t* tree = trees_ + tree_base_[book];
  uint32_t node = 0;
  for (int depth = 0; depth < 20; ++depth) {
    const int16_t next = tree[node * 2 + b.get1()];
    if (b.overrun) return -1;
    if (next < 0) return ~next;
    if (next == 0) return -1;
    node = static_cast<uint32_t>(next);
  }
  return -1;
}

bool Decoder::parse_ics_info(Bits& b, IcsInfo* ics) {
  b.get1();  // reserved
  ics->window_sequence = static_cast<uint8_t>(b.get(2));
  ics->window_shape = static_cast<uint8_t>(b.get1());
  const bool is_short = ics->window_sequence == EightShort;
  const int rate = static_cast<int>(sampling_index_);
  static const uint8_t* const kLongWidths[13] = {kWidthsL96, kWidthsL96, kWidthsL64, kWidthsL48, kWidthsL48, kWidthsL32, kWidthsL24, kWidthsL24, kWidthsL16, kWidthsL16, kWidthsL16, kWidthsL8, kWidthsL8};
  static const uint8_t kLongCount[13] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40, 40};
  static const uint8_t* const kShortWidths[13] = {kWidthsS96, kWidthsS96, kWidthsS96, kWidthsS48, kWidthsS48, kWidthsS48, kWidthsS24, kWidthsS24, kWidthsS16, kWidthsS16, kWidthsS16, kWidthsS8, kWidthsS8};
  static const uint8_t kShortCount[13] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15};
  ics->widths = is_short ? kShortWidths[rate] : kLongWidths[rate];
  ics->num_swb = is_short ? kShortCount[rate] : kLongCount[rate];
  ics->tns_max_bands = is_short ? kTnsMaxBandsShort[rate] : kTnsMaxBandsLong[rate];
  uint16_t at = 0;
  for (int i = 0; i < ics->num_swb; ++i) { ics->swb_offset[i] = at; at = static_cast<uint16_t>(at + ics->widths[i]); }
  ics->swb_offset[ics->num_swb] = at;
  if (is_short) {
    ics->max_sfb = static_cast<uint8_t>(b.get(4));
    const uint32_t grouping = b.get(7);
    ics->num_windows = 8;
    ics->num_window_groups = 1;
    ics->group_len[0] = 1;
    for (int i = 0; i < 7; ++i) {
      if ((grouping >> (6 - i)) & 1) ++ics->group_len[ics->num_window_groups - 1];
      else ics->group_len[ics->num_window_groups++] = 1;
    }
  } else {
    ics->max_sfb = static_cast<uint8_t>(b.get(6));
    if (b.get1()) return false;  // predictor_data_present: Main profile prediction is not supported
    ics->num_windows = 1;
    ics->num_window_groups = 1;
    ics->group_len[0] = 1;
  }
  return !b.overrun && ics->max_sfb <= ics->num_swb;
}

bool Decoder::parse_section_data(Bits& b, Channel* ch) {
  const IcsInfo& ics = ch->ics;
  const uint32_t esc_bits = ics.is_short() ? 3 : 5, esc = (1u << esc_bits) - 1;
  for (int g = 0; g < ics.num_window_groups; ++g) {
    int k = 0;
    while (k < ics.max_sfb) {
      const uint32_t cb = b.get(4);
      if (cb == 12) return false;
      uint32_t len = 0, incr;
      do { incr = b.get(esc_bits); len += incr; } while (incr == esc && !b.overrun);
      if (b.overrun || k + len > ics.max_sfb) return false;
      for (uint32_t i = 0; i < len; ++i) ch->band_type[g * ics.max_sfb + k + i] = static_cast<uint8_t>(cb);
      k += static_cast<int>(len);
    }
  }
  return !b.overrun;
}

bool Decoder::parse_scalefactors(Bits& b, Channel* ch) {
  const IcsInfo& ics = ch->ics;
  int scale = ch->global_gain, noise = ch->global_gain - 90, intensity = 0;
  bool first_noise = true;
  for (int g = 0; g < ics.num_window_groups; ++g)
    for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
      const int idx = g * ics.max_sfb + sfb;
      const int cb = ch->band_type[idx];
      if (cb == ZeroBand) {
        ch->sf[idx] = 0;
      } else if (cb == IntensityBand || cb == IntensityBand2) {
        const int d = huff(b, 11);
        if (d < 0) return false;
        intensity += d - 60;
        ch->sf[idx] = intensity < -155 ? -155 : intensity > 100 ? 100 : intensity;
      } else if (cb == NoiseBand) {
        if (first_noise) { noise += static_cast<int>(b.get(9)) - 256; first_noise = false; }
        else {
          const int d = huff(b, 11);
          if (d < 0) return false;
          noise += d - 60;
        }
        ch->sf[idx] = noise < -100 ? -100 : noise > 155 ? 155 : noise;
      } else {
        const int d = huff(b, 11);
        if (d < 0) return false;
        scale += d - 60;
        if (scale < 0 || scale > 255) return false;
        ch->sf[idx] = scale;
      }
    }
  return !b.overrun;
}

bool Decoder::parse_pulse(Bits& b, Channel* ch) {
  Pulse& p = ch->pulse;
  p.count = static_cast<uint8_t>(b.get(2) + 1);
  p.start_sfb = static_cast<uint8_t>(b.get(6));
  if (ch->ics.is_short() || p.start_sfb >= ch->ics.num_swb) return false;
  for (int i = 0; i < p.count; ++i) {
    p.offset[i] = static_cast<uint8_t>(b.get(5));
    p.amp[i] = static_cast<uint8_t>(b.get(4));
  }
  return !b.overrun;
}

bool Decoder::parse_tns(Bits& b, Channel* ch) {
  Tns& t = ch->tns;
  const bool is8 = ch->ics.is_short();
  const int max_order = is8 ? 7 : 12;
  for (int w = 0; w < ch->ics.num_windows; ++w) {
    t.n_filt[w] = static_cast<uint8_t>(b.get(2 - (is8 ? 1 : 0)));
    if (t.n_filt[w] > 3) return false;
    if (!t.n_filt[w]) continue;
    const uint32_t coef_res = b.get1();
    for (int f = 0; f < t.n_filt[w]; ++f) {
      t.length[w][f] = static_cast<uint8_t>(b.get(6 - 2 * (is8 ? 1 : 0)));
      t.order[w][f] = static_cast<uint8_t>(b.get(5 - 2 * (is8 ? 1 : 0)));
      if (t.order[w][f] > max_order) return false;
      if (!t.order[w][f]) continue;
      t.direction[w][f] = b.get1();
      const uint32_t compress = b.get1();
      const uint32_t len = coef_res + 3 - compress, res_bits = coef_res + 3;
      float r[12];
      const double pi = 3.14159265358979323846;
      for (int i = 0; i < t.order[w][f]; ++i) {
        int q = static_cast<int>(b.get(len));
        if (q >= (1 << (len - 1))) q -= 1 << len;  // two's complement
        const double iqfac = ((1 << (res_bits - 1)) - 0.5) / (pi / 2), iqfac_m = ((1 << (res_bits - 1)) + 0.5) / (pi / 2);
        r[i] = static_cast<float>(fmath::sin(q / (q >= 0 ? iqfac : iqfac_m)));
      }
      // Reflection coefficients to direct-form prediction coefficients.
      float lpc[12] = {};
      for (int i = 0; i < t.order[w][f]; ++i) {
        const float rr = r[i];
        lpc[i] = rr;
        for (int j = 0; j < (i + 1) / 2; ++j) {
          const float a = lpc[j], c = lpc[i - 1 - j];
          lpc[j] = a + rr * c;
          lpc[i - 1 - j] = c + rr * a;
        }
      }
      for (int i = 0; i < t.order[w][f]; ++i) t.lpc[w][f][i] = lpc[i];
    }
  }
  return !b.overrun;
}

bool Decoder::parse_spectrum(Bits& b, Channel* ch) {
  const IcsInfo& ics = ch->ics;
  for (int16_t& v : ch->q) v = 0;
  int window = 0;
  for (int g = 0; g < ics.num_window_groups; ++g) {
    for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
      const int cb = ch->band_type[g * ics.max_sfb + sfb];
      if (cb == ZeroBand || cb >= NoiseBand) continue;
      const int start = ics.swb_offset[sfb], end = ics.swb_offset[sfb + 1];
      for (int w = 0; w < ics.group_len[g]; ++w) {
        int16_t* q = ch->q + (window + w) * 128 * (ics.is_short() ? 1 : 0) + start;
        if (cb <= 4) {
          for (int k = start; k < end; k += 4, q += 4) {
            const int idx = huff(b, cb - 1);
            if (idx < 0) return false;
            int v[4] = {idx / 27, idx / 9 % 3, idx / 3 % 3, idx % 3};
            if (cb <= 2) {
              for (int i = 0; i < 4; ++i) q[i] = static_cast<int16_t>(v[i] - 1);
            } else {
              for (int i = 0; i < 4; ++i) {
                q[i] = static_cast<int16_t>(v[i]);
                if (v[i] && b.get1()) q[i] = static_cast<int16_t>(-v[i]);
              }
            }
          }
        } else {
          const int modulus = cb <= 6 ? 9 : cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
          for (int k = start; k < end; k += 2, q += 2) {
            const int idx = huff(b, cb - 1);
            if (idx < 0) return false;
            int v0 = idx / modulus, v1 = idx % modulus;
            if (cb <= 6) {
              q[0] = static_cast<int16_t>(v0 - 4);
              q[1] = static_cast<int16_t>(v1 - 4);
            } else {
              const bool n0 = v0 && b.get1(), n1 = v1 && b.get1();
              if (cb == 11) {
                auto escape = [&](int v) {
                  if (v != 16) return v;
                  int ones = 0;
                  while (b.get1() && ones < 12 && !b.overrun) ++ones;
                  const uint32_t n = static_cast<uint32_t>(ones) + 4;
                  return static_cast<int>((1u << n) + b.get(n));
                };
                v0 = escape(v0);
                v1 = escape(v1);
              }
              if (v0 > 32767 || v1 > 32767) return false;
              q[0] = static_cast<int16_t>(n0 ? -v0 : v0);
              q[1] = static_cast<int16_t>(n1 ? -v1 : v1);
            }
          }
        }
        if (b.overrun) return false;
      }
    }
    window += ics.group_len[g];
  }
  return !b.overrun;
}

// ---- reconstruction ----
void Decoder::dequantize(Channel* ch) {
  const IcsInfo& ics = ch->ics;
  for (float& c : ch->coef) c = 0;
  if (ch->pulse.present) {
    int pos = ics.swb_offset[ch->pulse.start_sfb];
    for (int i = 0; i < ch->pulse.count; ++i) {
      pos += ch->pulse.offset[i];
      if (pos >= 1024) break;
      ch->q[pos] = static_cast<int16_t>(ch->q[pos] + (ch->q[pos] > 0 ? ch->pulse.amp[i] : -ch->pulse.amp[i]));
    }
  }
  int window = 0;
  for (int g = 0; g < ics.num_window_groups; ++g) {
    for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
      const int idx = g * ics.max_sfb + sfb;
      const int cb = ch->band_type[idx];
      if (cb == ZeroBand || cb == IntensityBand || cb == IntensityBand2) continue;
      const int start = ics.swb_offset[sfb], end = ics.swb_offset[sfb + 1];
      for (int w = 0; w < ics.group_len[g]; ++w) {
        const int base = ics.is_short() ? (window + w) * 128 : 0;
        float* c = ch->coef + base;
        if (cb == NoiseBand) {
          float energy = 0;
          for (int k = start; k < end; ++k) {
            random_ = random_ * 1664525u + 1013904223u;
            c[k] = static_cast<float>(static_cast<int32_t>(random_));
            energy += c[k] * c[k];
          }
          const float scale = energy > 0 ? static_cast<float>(fmath::pow(2.0, ch->sf[idx] * 0.25) / fmath::sqrt(energy)) : 0.0f;
          for (int k = start; k < end; ++k) c[k] *= scale;
        } else {
          const float gain = static_cast<float>(fmath::pow(2.0, (ch->sf[idx] - 100) * 0.25));
          const int16_t* q = ch->q + base;
          for (int k = start; k < end; ++k) {
            const int v = q[k] < 0 ? -q[k] : q[k];
            const float m = v < 8192 ? pow43_[v] : static_cast<float>(fmath::pow(v, 4.0 / 3.0));
            c[k] = (q[k] < 0 ? -m : m) * gain;
          }
        }
      }
    }
    window += ics.group_len[g];
  }
}

void Decoder::apply_tns(Channel* ch) {
  const IcsInfo& ics = ch->ics;
  const int mmm = ics.tns_max_bands < ics.max_sfb ? ics.tns_max_bands : ics.max_sfb;
  if (!mmm) return;
  for (int w = 0; w < ics.num_windows; ++w) {
    int bottom = ics.num_swb;
    for (int f = 0; f < ch->tns.n_filt[w]; ++f) {
      const int top = bottom;
      bottom = top - ch->tns.length[w][f] > 0 ? top - ch->tns.length[w][f] : 0;
      const int order = ch->tns.order[w][f];
      if (order == 0) continue;
      int start = ics.swb_offset[bottom < mmm ? bottom : mmm];
      const int end = ics.swb_offset[top < mmm ? top : mmm];
      const int size = end - start;
      if (size <= 0) continue;
      int inc = 1;
      if (ch->tns.direction[w][f]) {
        inc = -1;
        start = end - 1;
      }
      start += w * 128;
      float* coef = ch->coef;
      for (int m = 0; m < size; ++m, start += inc)
        for (int i = 1; i <= (m < order ? m : order); ++i) coef[start] -= coef[start - i * inc] * ch->tns.lpc[w][f][i - 1];
    }
  }
}

constexpr float kLongScale = 1.0f / 1024.0f, kShortScale = 1.0f / 128.0f;

void Decoder::synthesize(Channel* ch, ChannelState* st, float* out) {
  const IcsInfo& ics = ch->ics;
  const int prev_shape = st->prev_shape, shape = ics.window_shape;
  float z[2048];
  if (ics.window_sequence == EightShort) {
    for (float& f : z) f = 0;
    float y[256];
    for (int w = 0; w < 8; ++w) {
      mdct_short_.inverse(ch->coef + w * 128, y);
      const float* rise = short_slope_[w == 0 ? prev_shape : shape];
      const float* fall = short_slope_[shape];
      for (int i = 0; i < 128; ++i) z[448 + w * 128 + i] += y[i] * kShortScale * rise[i];
      for (int i = 0; i < 128; ++i) z[448 + w * 128 + 128 + i] += y[128 + i] * kShortScale * fall[127 - i];
    }
  } else {
    float y[2048];
    mdct_long_.inverse(ch->coef, y);
    const float* rise = long_slope_[prev_shape];
    const float* fall = long_slope_[shape];
    const float* srise = short_slope_[prev_shape];
    const float* sfall = short_slope_[shape];
    for (int i = 0; i < 1024; ++i) {
      float w;
      if (ics.window_sequence == LongStop) w = i < 448 ? 0.0f : i < 576 ? srise[i - 448] : 1.0f;
      else w = rise[i];
      z[i] = y[i] * kLongScale * w;
    }
    for (int i = 0; i < 1024; ++i) {
      float w;
      if (ics.window_sequence == LongStart) w = i < 448 ? 1.0f : i < 576 ? sfall[127 - (i - 448)] : 0.0f;
      else w = fall[1023 - i];
      z[1024 + i] = y[1024 + i] * kLongScale * w;
    }
  }
  for (int i = 0; i < 1024; ++i) {
    out[i] = st->overlap[i] + z[i];
    st->overlap[i] = z[1024 + i];
  }
  st->prev_shape = static_cast<uint8_t>(shape);
}

bool Decoder::parse_channel(Bits& b, Channel* ch, const IcsInfo* common) {
  ch->global_gain = static_cast<uint8_t>(b.get(8));
  if (common) ch->ics = *common;
  else if (!parse_ics_info(b, &ch->ics)) return false;
  for (uint8_t& t : ch->band_type) t = 0;
  ch->pulse = Pulse();
  ch->tns = Tns();
  if (!parse_section_data(b, ch) || !parse_scalefactors(b, ch)) return false;
  if (b.get1()) {
    ch->pulse.present = true;
    if (!parse_pulse(b, ch)) return false;
  }
  if (b.get1()) {
    ch->tns.present = true;
    if (!parse_tns(b, ch)) return false;
  }
  if (b.get1()) return false;  // gain control (SSR profile)
  if (!parse_spectrum(b, ch)) return false;
  dequantize(ch);
  return true;
}

bool Decoder::decode_block(ConstByteView data, int16_t* pcm) {
  Bits b{data};
  float out[2][1024];
  int chosen_channels = 0;  // 0: nothing chosen yet, 1: a single channel, 2: a pair
  int channel_index = 0;
  for (;;) {
    const uint32_t id = b.get(3);
    if (b.overrun) return false;
    if (id == 7) break;  // END
    if (id == 0 || id == 3) {  // SCE, LFE
      b.get(4);
      if (channel_index >= 16) return false;
      Channel& c = ch_[0];
      if (!parse_channel(b, &c, nullptr)) return false;
      if (c.tns.present) apply_tns(&c);
      float tmp[1024];
      synthesize(&c, &states_[channel_index], chosen_channels == 0 ? out[0] : tmp);
      if (chosen_channels == 0) chosen_channels = 1;
      ++channel_index;
    } else if (id == 1) {  // CPE
      b.get(4);
      if (channel_index + 1 >= 16) return false;
      const bool common = b.get1();
      int ms_present = 0;
      IcsInfo shared;
      if (common) {
        if (!parse_ics_info(b, &shared)) return false;
        ms_present = static_cast<int>(b.get(2));
        if (ms_present == 3) return false;
        const int bands = shared.num_window_groups * shared.max_sfb;
        for (int i = 0; i < bands; ++i) ms_used_[i] = ms_present == 2;
        if (ms_present == 1)
          for (int i = 0; i < bands; ++i) ms_used_[i] = b.get1();
      }
      if (!parse_channel(b, &ch_[0], common ? &shared : nullptr) || !parse_channel(b, &ch_[1], common ? &shared : nullptr)) return false;
      if (common) {
        const IcsInfo& ics = ch_[0].ics;
        int window = 0;
        for (int g = 0; g < ics.num_window_groups; ++g) {
          for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
            const int idx = g * ics.max_sfb + sfb;
            const int start = ics.swb_offset[sfb], end = ics.swb_offset[sfb + 1];
            const int b0 = ch_[0].band_type[idx], b1 = ch_[1].band_type[idx];
            for (int w = 0; w < ics.group_len[g]; ++w) {
              float* l = ch_[0].coef + (ics.is_short() ? (window + w) * 128 : 0);
              float* r = ch_[1].coef + (ics.is_short() ? (window + w) * 128 : 0);
              if (ms_used_[idx] && b0 < NoiseBand && b1 < NoiseBand) {
                for (int k = start; k < end; ++k) {
                  const float m = l[k], s = r[k];
                  l[k] = m + s;
                  r[k] = m - s;
                }
              }
              if (b1 == IntensityBand || b1 == IntensityBand2) {  // the right channel is the left one, scaled
                float scale = (b1 == IntensityBand ? 1.0f : -1.0f) * static_cast<float>(fmath::pow(0.5, ch_[1].sf[idx] * 0.25));
                if (ms_present && ms_used_[idx]) scale = -scale;
                for (int k = start; k < end; ++k) r[k] = l[k] * scale;
              }
            }
          }
          window += ics.group_len[g];
        }
      }
      for (int c = 0; c < 2; ++c) {
        if (ch_[c].tns.present) apply_tns(&ch_[c]);
      }
      float tmp[1024];
      if (chosen_channels < 2) {
        synthesize(&ch_[0], &states_[channel_index], out[0]);
        synthesize(&ch_[1], &states_[channel_index + 1], out[1]);
        chosen_channels = 2;
      } else {
        synthesize(&ch_[0], &states_[channel_index], tmp);
        synthesize(&ch_[1], &states_[channel_index + 1], tmp);
      }
      channel_index += 2;
    } else if (id == 4) {  // DSE
      b.get(4);
      const bool align = b.get1();
      uint32_t count = b.get(8);
      if (count == 255) count += b.get(8);
      if (align) b.align();
      b.skip(uint64_t{count} * 8);
    } else if (id == 6) {  // FIL
      uint32_t count = b.get(4);
      if (count == 15) count += b.get(8) - 1;
      b.skip(uint64_t{count} * 8);
    } else if (id == 5) {  // PCE
      b.get(4);
      b.get(2);
      b.get(4);
      const uint32_t front = b.get(4), side = b.get(4), back = b.get(4), lfe = b.get(2), assoc = b.get(3), cc = b.get(4);
      if (b.get1()) b.get(4);
      if (b.get1()) b.get(4);
      if (b.get1()) b.get(3);
      b.skip(uint64_t{(front + side + back + cc)} * 5 + uint64_t{lfe} * 4 + uint64_t{assoc} * 4);
      b.align();
      const uint32_t comment = b.get(8);
      b.skip(uint64_t{comment} * 8);
    } else {
      return false;  // CCE (coupling) and anything else
    }
    if (b.overrun) return false;
  }
  if (chosen_channels == 0) return false;
  out_channels_ = static_cast<uint32_t>(chosen_channels);
  for (int i = 0; i < 1024; ++i)
    for (int c = 0; c < chosen_channels; ++c) {
      float v = out[c][i];
      v = v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v;
      pcm[i * chosen_channels + c] = static_cast<int16_t>(v >= 0 ? v + 0.5f : v - 0.5f);
    }
  return true;
}

}  // namespace aac

// ---- public wrapper ----

AacDecoder::~AacDecoder() {}

bool AacDecoder::init(uint32_t sampling_index) {
  impl_ = UniquePointer<aac::Decoder>::make(allocator_, allocator_);
  return impl_ && impl_->init(sampling_index);
}
void AacDecoder::reset() {
  if (impl_) impl_->reset_state();
}
uint32_t AacDecoder::sample_rate() const { return impl_ ? impl_->sample_rate() : 0; }
uint32_t AacDecoder::channels() const { return impl_ ? impl_->channels() : 0; }
bool AacDecoder::decode_block(ConstByteView block, int16_t* pcm) { return impl_ && impl_->decode_block(block, pcm); }

bool parse_audio_specific_config(ConstByteView config, uint32_t* object_type, uint32_t* sampling_index, uint32_t* channel_config) {
  aac::Bits b{config};
  uint32_t aot = b.get(5);
  if (aot == 31) aot = 32 + b.get(6);
  uint32_t index = b.get(4);
  if (index == 15) {  // explicit frequency: match it against the table
    const uint32_t hz = b.get(24);
    index = 13;
    for (uint32_t i = 0; i < 13; ++i)
      if (aac::kSampleRates[i] == hz) index = i;
  }
  const uint32_t channels = b.get(4);
  if (b.overrun || index >= 13) return false;
  *object_type = aot;
  *sampling_index = index;
  *channel_config = channels;
  return aot == 2;  // AAC-LC
}

namespace {
// ADTS: 12 sync bits, layer 0, a valid sampling index, and a frame length that fits.
bool adts_header(ConstByteView d, uint64_t at, uint32_t* header_size, uint32_t* frame_length, uint32_t* sampling_index, uint32_t* profile) {
  if (at + 7 > d.size() || d[at] != 0xFF || (d[at + 1] & 0xF6) != 0xF0) return false;
  const uint32_t protection_absent = d[at + 1] & 1;
  *profile = d[at + 2] >> 6;
  *sampling_index = (d[at + 2] >> 2) & 15;
  *frame_length = ((d[at + 3] & 3u) << 11) | (uint32_t{d[at + 4]} << 3) | (d[at + 5] >> 5);
  *header_size = protection_absent ? 7 : 9;
  return *sampling_index < 13 && *frame_length >= *header_size && ((d[at + 6] & 3) == 0);
}
}  // namespace

bool looks_like_adts(ConstByteView d) {
  uint32_t header, length, rate, profile;
  if (!adts_header(d, 0, &header, &length, &rate, &profile)) return false;
  return length == d.size() || (length + 2 <= d.size() && adts_header(d, length, &header, &length, &rate, &profile));
}

bool decode_aac(ConstByteView data, AudioData* out) {
  out->clear();
  if (!looks_like_adts(data)) return false;
  allocators::IAllocator& allocator = out->allocator();
  AacDecoder decoder(allocator);
  ByteBuffer pcm(allocator);
  uint32_t rate = 0, channels = 0;
  int16_t block[2048];
  bool ok = true;
  for (uint64_t at = 0; ok && at + 7 <= data.size();) {
    uint32_t header, length, index, profile;
    if (!adts_header(data, at, &header, &length, &index, &profile) || at + length > data.size()) {
      ++at;  // lost sync: look for the next frame
      continue;
    }
    if (profile != 1) {  // not AAC-LC
      at += length;
      continue;
    }
    if (rate == 0) {
      if (!decoder.init(index)) return false;
      rate = decoder.sample_rate();
    } else if (aac::kSampleRates[index] != rate) {
      at += length;
      continue;
    }
    // (Several raw data blocks in one frame carry a table of their positions: only the single-block form is handled.)
    if ((data[at + 6] & 3) == 0 && decoder.decode_block(data.subview(at + header, length - header), block)) {
      if (channels == 0) channels = decoder.channels();
      if (decoder.channels() == channels) ok = pcm.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels * 2));
    }
    at += length;
  }
  if (!ok || rate == 0 || channels == 0 || pcm.size() == 0) return false;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  const uint64_t frames = pcm.size() / (channels * 2);
  if (!out->create(format, frames)) return false;
  __builtin_memcpy(out->samples().data(), pcm.view().data(), frames * channels * 2);
  return true;
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// Streaming: ADTS frames are found through a window of the file and decoded one at a time.

namespace {

class AacStream final : public StreamingPcmSource {
 public:
  static constexpr uint32_t kIndexEvery = 32, kPrimeFrames = 2;

  AacStream(IByteSource& source, allocators::IAllocator& allocator) : source_(source), allocator_(allocator), window_(source, allocator), decoder_(allocator), pcm_(allocator), index_(allocator) {}

  bool init() {
    uint64_t counted = 0, at = 0, frame_at = 0;
    uint32_t header = 0, length = 0, index = 0;
    while (next_frame(at, &frame_at, &header, &length, &index)) {
      at = frame_at + length;
      if (rate_ == 0) {
        if (!decoder_.init(index)) return false;
        rate_ = decoder_.sample_rate();
        sampling_index_ = index;
      } else if (kSampleRates()[index] != rate_) {
        continue;
      }
      if (counted % kIndexEvery == 0 && !index_.push_back(frame_at)) return false;
      ++counted;
    }
    if (rate_ == 0 || counted == 0) return false;
    // The channel count is what the first block decodes to.
    cur_at_ = index_[0];
    cur_frame_ = 0;
    int16_t block[2048];
    if (!decode_next(block)) return false;
    channels_ = decoder_.channels();
    counted_ = counted;
    frames_ = counted * 1024;
    format_.sample_rate = rate_;
    format_.channels = static_cast<uint8_t>(channels_);
    format_.type = audio::SampleType::S16;
    restart(0);
    return channels_ >= 1 && channels_ <= 2 && format_.valid();
  }

 protected:
  bool refill(uint64_t frame) override {
    const uint64_t k = frame / 1024;
    if (k < cur_frame_ || k - cur_frame_ > 256) restart(k > kPrimeFrames ? k - kPrimeFrames : 0);
    int16_t block[2048];
    while (cur_frame_ < k)
      if (!decode_next(block)) return false;
    pcm_.clear();
    uint32_t got = 0;
    for (; got < 4 && cur_frame_ < counted_; ++got) {
      if (!decode_next(block)) break;
      if (!pcm_.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels_ * 2))) return false;
    }
    if (got == 0) return false;
    set_window(k * 1024, pcm_.view());
    return true;
  }

 private:
  static const uint32_t* kSampleRates() { return aac::kSampleRates; }

  // Goes back to the remembered place at or before frame `target` with the decoder's memory cleared.
  void restart(uint64_t target) {
    const uint64_t slot = target / kIndexEvery;
    decoder_.reset();
    cur_frame_ = slot * kIndexEvery;
    cur_at_ = index_[slot];
  }

  // The next ADTS frame at or after `pos`: one the decoder can use (AAC-LC, one raw data block).
  bool next_frame(uint64_t pos, uint64_t* at, uint32_t* header, uint32_t* length, uint32_t* index) {
    for (;;) {
      const ConstByteView v = window_.from(pos);
      if (v.size() < 7) return false;
      for (uint64_t i = 0; i + 7 <= v.size(); ++i) {
        uint32_t h, len, idx, profile;
        if (!adts_header(v, i, &h, &len, &idx, &profile) || i + len > v.size()) continue;
        if (profile != 1 || (v[i + 6] & 3) != 0) {
          i += len - 1;   // (not AAC-LC, or several blocks in one frame: not played)
          continue;
        }
        *at = pos + i;
        *header = h;
        *length = len;
        *index = idx;
        return true;
      }
      if (v.size() < 131072) return false;
      pos += v.size() - 8192;
    }
  }

  // Decodes the frame at cur_at_ (of the stream's sampling rate); a frame that cannot be decoded is silence.
  bool decode_next(int16_t* block) {
    for (;;) {
      uint64_t at = 0;
      uint32_t header = 0, length = 0, index = 0;
      if (!next_frame(cur_at_, &at, &header, &length, &index)) return false;
      cur_at_ = at + length;
      if (aac::kSampleRates[index] != rate_) continue;
      const ConstByteView v = window_.from(at);
      if (v.size() < length) return false;
      if (!decoder_.decode_block(v.subview(header, length - header), block))
        for (int i = 0; i < 2048; ++i) block[i] = 0;
      ++cur_frame_;
      return true;
    }
  }

  IByteSource& source_;
  allocators::IAllocator& allocator_;
  SourceWindow window_;
  AacDecoder decoder_;
  ByteBuffer pcm_;
  List<uint64_t> index_;
  uint32_t rate_ = 0, channels_ = 0, sampling_index_ = 0;
  uint64_t counted_ = 0, cur_frame_ = 0, cur_at_ = 0;
};

}  // namespace

UniquePointer<audio::IPcmSource> open_aac_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<AacStream> stream = UniquePointer<AacStream>::make(allocator, source, allocator);
  if (!stream || !stream->init()) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

}  // namespace media
