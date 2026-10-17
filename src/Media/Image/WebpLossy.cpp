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

// WebP lossy: a VP8 key frame (RFC 6386) decoded to RGB(A), with the optional ALPH alpha plane.
//
// The probability and quantiser tables below are the ones in RFC 6386, taken from libwebp (Copyright 2010 Google Inc.,
// BSD 3-Clause license; https://chromium.googlesource.com/webm/libwebp). The decoder around them is written for this code
// base from the format description, following libwebp's behaviour where the specification leaves room - notably the "fancy"
// chroma upsampling and the fixed-point YUV to RGB conversion, so that the pictures match what libwebp produces.
//
// Not handled: animated lossy files; VP8 inter frames (a WebP holds one key frame); the colour-space and clamping hints.
#include "Media/Image/WebpInternal.hpp"

namespace media {
namespace webp_internal {

namespace {
const uint8_t CoeffsProba0[4][8][3][11] = { {{{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}, {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}, {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}, {{253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128}, {189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128}, {106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128}}, { {1, 98, 248, 255, 236, 226, 255, 255, 128, 128, 128}, {181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128}, {78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128}, }, { {1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128}, {184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128}, {77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128}, }, {{1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128}, {170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128}, {37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128}}, {{1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128}, {207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128}, {102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128}}, {{1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128}, {177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128}, {80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128}}, {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}, {246, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}, {255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}}, {{{198, 35, 237, 223, 193, 187, 162, 160, 145, 155, 62}, {131, 45, 198, 221, 172, 176, 220, 157, 252, 221, 1}, {68, 47, 146, 208, 149, 167, 221, 162, 255, 223, 128}}, {{1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128}, {184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128}, {81, 99, 181, 242, 176, 190, 249, 202, 255, 255, 128}}, {{1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128}, {99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128}, {23, 91, 163, 242, 170, 187, 247, 210, 255, 255, 128}}, {{1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128}, {109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128}, {44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128}}, {{1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128}, {94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128}, {22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128}}, {{1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128}, {124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128}, {35, 77, 181, 251, 193, 211, 255, 205, 128, 128, 128}}, {{1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128}, {121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128}, {45, 99, 188, 251, 195, 217, 255, 224, 128, 128, 128}}, {{1, 1, 251, 255, 213, 255, 128, 128, 128, 128, 128}, {203, 1, 248, 255, 255, 128, 128, 128, 128, 128, 128}, {137, 1, 177, 255, 224, 255, 128, 128, 128, 128, 128}}}, {{{253, 9, 248, 251, 207, 208, 255, 192, 128, 128, 128}, {175, 13, 224, 243, 193, 185, 249, 198, 255, 255, 128}, {73, 17, 171, 221, 161, 179, 236, 167, 255, 234, 128}}, {{1, 95, 247, 253, 212, 183, 255, 255, 128, 128, 128}, {239, 90, 244, 250, 211, 209, 255, 255, 128, 128, 128}, {155, 77, 195, 248, 188, 195, 255, 255, 128, 128, 128}}, {{1, 24, 239, 251, 218, 219, 255, 205, 128, 128, 128}, {201, 51, 219, 255, 196, 186, 128, 128, 128, 128, 128}, {69, 46, 190, 239, 201, 218, 255, 228, 128, 128, 128}}, {{1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128}, {223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128}, {141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128}}, {{1, 16, 248, 255, 255, 128, 128, 128, 128, 128, 128}, {190, 36, 230, 255, 236, 255, 128, 128, 128, 128, 128}, {149, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}}, {{1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128}, {247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128}, {240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128}}, {{1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128}, {213, 62, 250, 255, 255, 128, 128, 128, 128, 128, 128}, {55, 93, 255, 128, 128, 128, 128, 128, 128, 128, 128}}, {{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}, {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}, {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}}, {{{202, 24, 213, 235, 186, 191, 220, 160, 240, 175, 255}, {126, 38, 182, 232, 169, 184, 228, 174, 255, 187, 128}, {61, 46, 138, 219, 151, 178, 240, 170, 255, 216, 128}}, {{1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128}, {166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128}, {39, 77, 162, 232, 172, 180, 245, 178, 255, 255, 128}}, {{1, 52, 220, 246, 198, 199, 249, 220, 255, 255, 128}, {124, 74, 191, 243, 183, 193, 250, 221, 255, 255, 128}, {24, 71, 130, 219, 154, 170, 243, 182, 255, 255, 128}}, {{1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128}, {149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128}, {28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128}}, {{1, 81, 230, 252, 204, 203, 255, 192, 128, 128, 128}, {123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128}, {20, 95, 153, 243, 164, 173, 255, 203, 128, 128, 128}}, {{1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128}, {168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128}, {47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128}}, {{1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128}, {141, 84, 213, 252, 201, 202, 255, 219, 128, 128, 128}, {42, 80, 160, 240, 162, 185, 255, 205, 128, 128, 128}}, {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}, {244, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}, {238, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}}} };
const uint8_t kBModesProba[10][10][9] = { {{231, 120, 48, 89, 115, 113, 120, 152, 112}, {152, 179, 64, 126, 170, 118, 46, 70, 95}, {175, 69, 143, 80, 85, 82, 72, 155, 103}, {56, 58, 10, 171, 218, 189, 17, 13, 152}, {114, 26, 17, 163, 44, 195, 21, 10, 173}, {121, 24, 80, 195, 26, 62, 44, 64, 85}, {144, 71, 10, 38, 171, 213, 144, 34, 26}, {170, 46, 55, 19, 136, 160, 33, 206, 71}, {63, 20, 8, 114, 114, 208, 12, 9, 226}, {81, 40, 11, 96, 182, 84, 29, 16, 36}}, {{134, 183, 89, 137, 98, 101, 106, 165, 148}, {72, 187, 100, 130, 157, 111, 32, 75, 80}, {66, 102, 167, 99, 74, 62, 40, 234, 128}, {41, 53, 9, 178, 241, 141, 26, 8, 107}, {74, 43, 26, 146, 73, 166, 49, 23, 157}, {65, 38, 105, 160, 51, 52, 31, 115, 128}, {104, 79, 12, 27, 217, 255, 87, 17, 7}, {87, 68, 71, 44, 114, 51, 15, 186, 23}, {47, 41, 14, 110, 182, 183, 21, 17, 194}, {66, 45, 25, 102, 197, 189, 23, 18, 22}}, {{88, 88, 147, 150, 42, 46, 45, 196, 205}, {43, 97, 183, 117, 85, 38, 35, 179, 61}, {39, 53, 200, 87, 26, 21, 43, 232, 171}, {56, 34, 51, 104, 114, 102, 29, 93, 77}, {39, 28, 85, 171, 58, 165, 90, 98, 64}, {34, 22, 116, 206, 23, 34, 43, 166, 73}, {107, 54, 32, 26, 51, 1, 81, 43, 31}, {68, 25, 106, 22, 64, 171, 36, 225, 114}, {34, 19, 21, 102, 132, 188, 16, 76, 124}, {62, 18, 78, 95, 85, 57, 50, 48, 51}}, {{193, 101, 35, 159, 215, 111, 89, 46, 111}, {60, 148, 31, 172, 219, 228, 21, 18, 111}, {112, 113, 77, 85, 179, 255, 38, 120, 114}, {40, 42, 1, 196, 245, 209, 10, 25, 109}, {88, 43, 29, 140, 166, 213, 37, 43, 154}, {61, 63, 30, 155, 67, 45, 68, 1, 209}, {100, 80, 8, 43, 154, 1, 51, 26, 71}, {142, 78, 78, 16, 255, 128, 34, 197, 171}, {41, 40, 5, 102, 211, 183, 4, 1, 221}, {51, 50, 17, 168, 209, 192, 23, 25, 82}}, {{138, 31, 36, 171, 27, 166, 38, 44, 229}, {67, 87, 58, 169, 82, 115, 26, 59, 179}, {63, 59, 90, 180, 59, 166, 93, 73, 154}, {40, 40, 21, 116, 143, 209, 34, 39, 175}, {47, 15, 16, 183, 34, 223, 49, 45, 183}, {46, 17, 33, 183, 6, 98, 15, 32, 183}, {57, 46, 22, 24, 128, 1, 54, 17, 37}, {65, 32, 73, 115, 28, 128, 23, 128, 205}, {40, 3, 9, 115, 51, 192, 18, 6, 223}, {87, 37, 9, 115, 59, 77, 64, 21, 47}}, {{104, 55, 44, 218, 9, 54, 53, 130, 226}, {64, 90, 70, 205, 40, 41, 23, 26, 57}, {54, 57, 112, 184, 5, 41, 38, 166, 213}, {30, 34, 26, 133, 152, 116, 10, 32, 134}, {39, 19, 53, 221, 26, 114, 32, 73, 255}, {31, 9, 65, 234, 2, 15, 1, 118, 73}, {75, 32, 12, 51, 192, 255, 160, 43, 51}, {88, 31, 35, 67, 102, 85, 55, 186, 85}, {56, 21, 23, 111, 59, 205, 45, 37, 192}, {55, 38, 70, 124, 73, 102, 1, 34, 98}}, {{125, 98, 42, 88, 104, 85, 117, 175, 82}, {95, 84, 53, 89, 128, 100, 113, 101, 45}, {75, 79, 123, 47, 51, 128, 81, 171, 1}, {57, 17, 5, 71, 102, 57, 53, 41, 49}, {38, 33, 13, 121, 57, 73, 26, 1, 85}, {41, 10, 67, 138, 77, 110, 90, 47, 114}, {115, 21, 2, 10, 102, 255, 166, 23, 6}, {101, 29, 16, 10, 85, 128, 101, 196, 26}, {57, 18, 10, 102, 102, 213, 34, 20, 43}, {117, 20, 15, 36, 163, 128, 68, 1, 26}}, {{102, 61, 71, 37, 34, 53, 31, 243, 192}, {69, 60, 71, 38, 73, 119, 28, 222, 37}, {68, 45, 128, 34, 1, 47, 11, 245, 171}, {62, 17, 19, 70, 146, 85, 55, 62, 70}, {37, 43, 37, 154, 100, 163, 85, 160, 1}, {63, 9, 92, 136, 28, 64, 32, 201, 85}, {75, 15, 9, 9, 64, 255, 184, 119, 16}, {86, 6, 28, 5, 64, 255, 25, 248, 1}, {56, 8, 17, 132, 137, 255, 55, 116, 128}, {58, 15, 20, 82, 135, 57, 26, 121, 40}}, {{164, 50, 31, 137, 154, 133, 25, 35, 218}, {51, 103, 44, 131, 131, 123, 31, 6, 158}, {86, 40, 64, 135, 148, 224, 45, 183, 128}, {22, 26, 17, 131, 240, 154, 14, 1, 209}, {45, 16, 21, 91, 64, 222, 7, 1, 197}, {56, 21, 39, 155, 60, 138, 23, 102, 213}, {83, 12, 13, 54, 192, 255, 68, 47, 28}, {85, 26, 85, 85, 128, 128, 32, 146, 171}, {18, 11, 7, 63, 144, 171, 4, 4, 246}, {35, 27, 10, 146, 174, 171, 12, 26, 128}}, {{190, 80, 35, 99, 180, 80, 126, 54, 45}, {85, 126, 47, 87, 176, 51, 41, 20, 32}, {101, 75, 128, 139, 118, 146, 116, 128, 85}, {56, 41, 15, 176, 236, 85, 37, 9, 62}, {71, 30, 17, 119, 118, 255, 17, 18, 138}, {101, 38, 60, 138, 55, 70, 43, 26, 142}, {146, 36, 19, 30, 171, 255, 97, 27, 20}, {138, 45, 61, 62, 219, 1, 81, 188, 64}, {32, 41, 20, 117, 151, 142, 20, 21, 163}, {112, 19, 12, 61, 195, 128, 48, 4, 24}} };
const uint8_t CoeffsUpdateProba[4][8][3][11] = { {{{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255}, {249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255}, {234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255}, {250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255}, {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}}, {{{217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255}, {234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255}}, {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255}}, {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255}, {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}}, {{{186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255}, {234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255}, {251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255}}, {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255}}, {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}}, {{{248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255}, {248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255}}, {{255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255}, {246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255}, {252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255}}, {{255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255}, {248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255}, {253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255}}, {{255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255}, {252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255}, {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}, {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}} };
const uint8_t kDcTable[128] = { 4, 5, 6, 7, 8, 9, 10, 10, 11, 12, 13, 14, 15, 16, 17, 17, 18, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 25, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 91, 93, 95, 96, 98, 100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157 };
const uint16_t kAcTable[128] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284 };

constexpr uint8_t kBands[16 + 1] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
constexpr uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr uint8_t kCat3[] = {173, 148, 140, 0};
constexpr uint8_t kCat4[] = {176, 155, 140, 135, 0};
constexpr uint8_t kCat5[] = {180, 157, 141, 134, 130, 0};
constexpr uint8_t kCat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
const uint8_t* const kCat3456[] = {kCat3, kCat4, kCat5, kCat6};

// Prediction modes. The 16x16 and chroma modes reuse the 4x4 numbering: DC, TM, V (VE) and H (HE).
enum { B_DC_PRED = 0, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_RD_PRED, B_VR_PRED, B_LD_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED };
constexpr int BPS = 32;  // stride of the macroblock work area
constexpr int kYOff = BPS * 1 + 8, kUOff = kYOff + BPS * 16 + BPS, kVOff = kUOff + 16;
constexpr int kWorkSize = BPS * 17 + BPS * 9;

inline uint8_t clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : static_cast<uint8_t>(v); }

// The VP8 boolean decoder, as in the specification's reference.
struct BoolDecoder {
  const uint8_t* p = nullptr;
  const uint8_t* end = nullptr;
  uint32_t value = 0, range = 255;
  int bit_count = 0;
  int overrun = 0;  // bytes asked for beyond the end of the data

  void init(const uint8_t* data, uint64_t size) {
    p = data;
    end = data + size;
    overrun = 0;
    value = (next() << 8);
    value |= next();
    range = 255;
    bit_count = 0;
  }
  uint32_t next() {
    if (p < end) return *p++;
    ++overrun;
    return 0;
  }
  bool eof() const { return overrun > 2; }
  int get(int prob) {
    const uint32_t split = 1 + (((range - 1) * static_cast<uint32_t>(prob)) >> 8);
    const uint32_t big = split << 8;
    int bit;
    if (value >= big) {
      bit = 1;
      range -= split;
      value -= big;
    } else {
      bit = 0;
      range = split;
    }
    while (range < 128) {
      value <<= 1;
      range <<= 1;
      if (++bit_count == 8) {
        bit_count = 0;
        value |= next();
      }
    }
    return bit;
  }
  int bit() { return get(0x80); }
  uint32_t literal(int n) {
    uint32_t v = 0;
    while (n-- > 0) v = (v << 1) | static_cast<uint32_t>(bit());
    return v;
  }
  int signed_literal(int n) {
    const int v = static_cast<int>(literal(n));
    return bit() ? -v : v;
  }
};

struct SegmentHeader {
  bool use_segment = false, update_map = false, absolute_delta = true;
  int8_t quantizer[4] = {}, filter_strength[4] = {};
};

struct FilterHeader {
  bool simple = false;
  int level = 0, sharpness = 0;
  bool use_lf_delta = false;
  int ref_lf_delta[4] = {}, mode_lf_delta[4] = {};
};

struct Quant {
  int y1[2], y2[2], uv[2];  // DC and AC factors
};

struct FilterInfo {
  uint8_t limit = 0, ilevel = 0, hev_thresh = 0;
};

// What the loop filter needs to know about each macroblock once it is decoded.
struct MbFilter {
  uint8_t segment = 0;
  bool i4x4 = false, inner = false;
};

struct Probas {
  uint8_t bands[4][8][3][11];
  uint8_t segments[3];
};

struct NzContext {
  uint8_t y[4], u[2], v[2], dc;
};

// ---- transforms and predictions on the work area ----
inline int mul1(int a) { return ((a * 20091) >> 16) + a; }
inline int mul2(int a) { return (a * 35468) >> 16; }

void transform_one(const int16_t* in, uint8_t* dst) {
  int tmp[16];
  int* t = tmp;
  for (int i = 0; i < 4; ++i, ++in, t += 4) {  // vertical pass
    const int a = in[0] + in[8];
    const int b = in[0] - in[8];
    const int c = mul2(in[4]) - mul1(in[12]);
    const int d = mul1(in[4]) + mul2(in[12]);
    t[0] = a + d;
    t[1] = b + c;
    t[2] = b - c;
    t[3] = a - d;
  }
  t = tmp;
  for (int i = 0; i < 4; ++i, ++t, dst += BPS) {  // horizontal pass
    const int dc = t[0] + 4;
    const int a = dc + t[8];
    const int b = dc - t[8];
    const int c = mul2(t[4]) - mul1(t[12]);
    const int d = mul1(t[4]) + mul2(t[12]);
    dst[0] = clip8(dst[0] + ((a + d) >> 3));
    dst[1] = clip8(dst[1] + ((b + c) >> 3));
    dst[2] = clip8(dst[2] + ((b - c) >> 3));
    dst[3] = clip8(dst[3] + ((a - d) >> 3));
  }
}

void transform_wht(const int16_t* in, int16_t* out) {
  int tmp[16];
  for (int i = 0; i < 4; ++i) {
    const int a0 = in[0 + i] + in[12 + i];
    const int a1 = in[4 + i] + in[8 + i];
    const int a2 = in[4 + i] - in[8 + i];
    const int a3 = in[0 + i] - in[12 + i];
    tmp[0 + i] = a0 + a1;
    tmp[8 + i] = a0 - a1;
    tmp[4 + i] = a3 + a2;
    tmp[12 + i] = a3 - a2;
  }
  for (int i = 0; i < 4; ++i) {
    const int dc = tmp[0 + i * 4] + 3;
    const int a0 = dc + tmp[3 + i * 4];
    const int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
    const int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
    const int a3 = dc - tmp[3 + i * 4];
    out[0] = static_cast<int16_t>((a0 + a1) >> 3);
    out[16] = static_cast<int16_t>((a3 + a2) >> 3);
    out[32] = static_cast<int16_t>((a0 - a1) >> 3);
    out[48] = static_cast<int16_t>((a3 - a2) >> 3);
    out += 64;
  }
}

inline int avg3(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }
inline int avg2(int a, int b) { return (a + b + 1) >> 1; }

void predict_tm(uint8_t* dst, int size) {
  const uint8_t* top = dst - BPS;
  for (int y = 0; y < size; ++y, dst += BPS)
    for (int x = 0; x < size; ++x) dst[x] = clip8(top[x] + dst[-1] - top[-1]);
}

void predict_v(uint8_t* dst, int size) {
  for (int j = 0; j < size; ++j)
    for (int i = 0; i < size; ++i) dst[j * BPS + i] = dst[i - BPS];
}

void predict_h(uint8_t* dst, int size) {
  for (int j = 0; j < size; ++j)
    for (int i = 0; i < size; ++i) dst[j * BPS + i] = dst[j * BPS - 1];
}

// DC over `size`, using the top row and/or the left column if available.
void predict_dc(uint8_t* dst, int size, bool have_top, bool have_left) {
  const int shift = size == 16 ? 3 : 2;  // log2(size) - 1
  int dc = 0, count = 0;
  if (have_top) { for (int i = 0; i < size; ++i) dc += dst[i - BPS]; ++count; }
  if (have_left) { for (int i = 0; i < size; ++i) dc += dst[-1 + i * BPS]; ++count; }
  int value;
  if (count == 2) value = (dc + size) >> (shift + 2);
  else if (count == 1) value = (dc + size / 2) >> (shift + 1);
  else value = 0x80;
  for (int j = 0; j < size; ++j)
    for (int i = 0; i < size; ++i) dst[j * BPS + i] = static_cast<uint8_t>(value);
}

void predict_4x4(int mode, uint8_t* dst) {
#define DST(x, y) dst[(x) + (y) * BPS]
  const uint8_t* top = dst - BPS;
  switch (mode) {
    case B_DC_PRED: {
      int dc = 4;
      for (int i = 0; i < 4; ++i) dc += dst[i - BPS] + dst[-1 + i * BPS];
      dc >>= 3;
      for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) DST(i, j) = static_cast<uint8_t>(dc);
      break;
    }
    case B_TM_PRED: predict_tm(dst, 4); break;
    case B_VE_PRED: {
      const uint8_t vals[4] = {static_cast<uint8_t>(avg3(top[-1], top[0], top[1])), static_cast<uint8_t>(avg3(top[0], top[1], top[2])),
                               static_cast<uint8_t>(avg3(top[1], top[2], top[3])), static_cast<uint8_t>(avg3(top[2], top[3], top[4]))};
      for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) DST(i, j) = vals[i];
      break;
    }
    case B_HE_PRED: {
      const int A = dst[-1 - BPS], B = dst[-1], C = dst[-1 + BPS], D = dst[-1 + 2 * BPS], E = dst[-1 + 3 * BPS];
      const uint8_t rows[4] = {static_cast<uint8_t>(avg3(A, B, C)), static_cast<uint8_t>(avg3(B, C, D)), static_cast<uint8_t>(avg3(C, D, E)),
                               static_cast<uint8_t>(avg3(D, E, E))};
      for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) DST(i, j) = rows[j];
      break;
    }
    case B_RD_PRED: {
      const int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], L = dst[-1 + 3 * BPS], X = dst[-1 - BPS];
      const int A = top[0], B = top[1], C = top[2], D = top[3];
      DST(0, 3) = static_cast<uint8_t>(avg3(J, K, L));
      DST(1, 3) = DST(0, 2) = static_cast<uint8_t>(avg3(I, J, K));
      DST(2, 3) = DST(1, 2) = DST(0, 1) = static_cast<uint8_t>(avg3(X, I, J));
      DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = static_cast<uint8_t>(avg3(A, X, I));
      DST(3, 2) = DST(2, 1) = DST(1, 0) = static_cast<uint8_t>(avg3(B, A, X));
      DST(3, 1) = DST(2, 0) = static_cast<uint8_t>(avg3(C, B, A));
      DST(3, 0) = static_cast<uint8_t>(avg3(D, C, B));
      break;
    }
    case B_VR_PRED: {
      const int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], X = dst[-1 - BPS];
      const int A = top[0], B = top[1], C = top[2], D = top[3];
      DST(0, 0) = DST(1, 2) = static_cast<uint8_t>(avg2(X, A));
      DST(1, 0) = DST(2, 2) = static_cast<uint8_t>(avg2(A, B));
      DST(2, 0) = DST(3, 2) = static_cast<uint8_t>(avg2(B, C));
      DST(3, 0) = static_cast<uint8_t>(avg2(C, D));
      DST(0, 3) = static_cast<uint8_t>(avg3(K, J, I));
      DST(0, 2) = static_cast<uint8_t>(avg3(J, I, X));
      DST(0, 1) = DST(1, 3) = static_cast<uint8_t>(avg3(I, X, A));
      DST(1, 1) = DST(2, 3) = static_cast<uint8_t>(avg3(X, A, B));
      DST(2, 1) = DST(3, 3) = static_cast<uint8_t>(avg3(A, B, C));
      DST(3, 1) = static_cast<uint8_t>(avg3(B, C, D));
      break;
    }
    case B_LD_PRED: {
      const int A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], H = top[7];
      DST(0, 0) = static_cast<uint8_t>(avg3(A, B, C));
      DST(1, 0) = DST(0, 1) = static_cast<uint8_t>(avg3(B, C, D));
      DST(2, 0) = DST(1, 1) = DST(0, 2) = static_cast<uint8_t>(avg3(C, D, E));
      DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = static_cast<uint8_t>(avg3(D, E, F));
      DST(3, 1) = DST(2, 2) = DST(1, 3) = static_cast<uint8_t>(avg3(E, F, G));
      DST(3, 2) = DST(2, 3) = static_cast<uint8_t>(avg3(F, G, H));
      DST(3, 3) = static_cast<uint8_t>(avg3(G, H, H));
      break;
    }
    case B_VL_PRED: {
      const int A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], H = top[7];
      DST(0, 0) = static_cast<uint8_t>(avg2(A, B));
      DST(1, 0) = DST(0, 2) = static_cast<uint8_t>(avg2(B, C));
      DST(2, 0) = DST(1, 2) = static_cast<uint8_t>(avg2(C, D));
      DST(3, 0) = DST(2, 2) = static_cast<uint8_t>(avg2(D, E));
      DST(0, 1) = static_cast<uint8_t>(avg3(A, B, C));
      DST(1, 1) = DST(0, 3) = static_cast<uint8_t>(avg3(B, C, D));
      DST(2, 1) = DST(1, 3) = static_cast<uint8_t>(avg3(C, D, E));
      DST(3, 1) = DST(2, 3) = static_cast<uint8_t>(avg3(D, E, F));
      DST(3, 2) = static_cast<uint8_t>(avg3(E, F, G));
      DST(3, 3) = static_cast<uint8_t>(avg3(F, G, H));
      break;
    }
    case B_HD_PRED: {
      const int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], L = dst[-1 + 3 * BPS], X = dst[-1 - BPS];
      const int A = top[0], B = top[1], C = top[2];
      DST(0, 0) = DST(2, 1) = static_cast<uint8_t>(avg2(I, X));
      DST(0, 1) = DST(2, 2) = static_cast<uint8_t>(avg2(J, I));
      DST(0, 2) = DST(2, 3) = static_cast<uint8_t>(avg2(K, J));
      DST(0, 3) = static_cast<uint8_t>(avg2(L, K));
      DST(3, 0) = static_cast<uint8_t>(avg3(A, B, C));
      DST(2, 0) = static_cast<uint8_t>(avg3(X, A, B));
      DST(1, 0) = DST(3, 1) = static_cast<uint8_t>(avg3(I, X, A));
      DST(1, 1) = DST(3, 2) = static_cast<uint8_t>(avg3(J, I, X));
      DST(1, 2) = DST(3, 3) = static_cast<uint8_t>(avg3(K, J, I));
      DST(1, 3) = static_cast<uint8_t>(avg3(L, K, J));
      break;
    }
    default: {  // B_HU_PRED
      const int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], L = dst[-1 + 3 * BPS];
      DST(0, 0) = static_cast<uint8_t>(avg2(I, J));
      DST(2, 0) = DST(0, 1) = static_cast<uint8_t>(avg2(J, K));
      DST(2, 1) = DST(0, 2) = static_cast<uint8_t>(avg2(K, L));
      DST(1, 0) = static_cast<uint8_t>(avg3(I, J, K));
      DST(3, 0) = DST(1, 1) = static_cast<uint8_t>(avg3(J, K, L));
      DST(3, 1) = DST(1, 2) = static_cast<uint8_t>(avg3(K, L, L));
      DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = static_cast<uint8_t>(L);
      break;
    }
  }
#undef DST
}

// ---- in-loop filters (RFC 6386 section 15) ----
inline int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }  // clips [-1020, 1020] to [-128, 127]
inline int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }      // clips [-112, 112] to [-16, 15]
inline int iabs(int v) { return v < 0 ? -v : v; }

void do_filter2(uint8_t* p, int step) {
  const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  const int a = 3 * (q0 - p0) + sclip1(p1 - q1);
  const int a1 = sclip2((a + 4) >> 3);
  const int a2 = sclip2((a + 3) >> 3);
  p[-step] = clip8(p0 + a2);
  p[0] = clip8(q0 - a1);
}

void do_filter4(uint8_t* p, int step) {
  const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  const int a = 3 * (q0 - p0);
  const int a1 = sclip2((a + 4) >> 3);
  const int a2 = sclip2((a + 3) >> 3);
  const int a3 = (a1 + 1) >> 1;
  p[-2 * step] = clip8(p1 + a3);
  p[-step] = clip8(p0 + a2);
  p[0] = clip8(q0 - a1);
  p[step] = clip8(q1 - a3);
}

void do_filter6(uint8_t* p, int step) {
  const int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
  const int q0 = p[0], q1 = p[step], q2 = p[2 * step];
  const int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
  const int a1 = (27 * a + 63) >> 7;
  const int a2 = (18 * a + 63) >> 7;
  const int a3 = (9 * a + 63) >> 7;
  p[-3 * step] = clip8(p2 + a3);
  p[-2 * step] = clip8(p1 + a2);
  p[-step] = clip8(p0 + a1);
  p[0] = clip8(q0 - a1);
  p[step] = clip8(q1 - a2);
  p[2 * step] = clip8(q2 - a3);
}

inline bool hev(const uint8_t* p, int step, int thresh) {
  const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  return iabs(p1 - p0) > thresh || iabs(q1 - q0) > thresh;
}

inline bool needs_filter(const uint8_t* p, int step, int t) {
  const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  return 4 * iabs(p0 - q0) + iabs(p1 - q1) <= t;
}

inline bool needs_filter2(const uint8_t* p, int step, int t, int it) {
  const int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
  const int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
  if (4 * iabs(p0 - q0) + iabs(p1 - q1) > t) return false;
  return iabs(p3 - p2) <= it && iabs(p2 - p1) <= it && iabs(p1 - p0) <= it && iabs(q3 - q2) <= it && iabs(q2 - q1) <= it && iabs(q1 - q0) <= it;
}

// `hstride` steps across the edge, `vstride` along it.
void simple_filter(uint8_t* p, int hstride, int vstride, int count, int thresh) {
  const int thresh2 = 2 * thresh + 1;
  for (int i = 0; i < count; ++i, p += vstride)
    if (needs_filter(p, hstride, thresh2)) do_filter2(p, hstride);
}

void filter_loop26(uint8_t* p, int hstride, int vstride, int size, int thresh, int ithresh, int hev_thresh) {
  const int thresh2 = 2 * thresh + 1;
  for (; size > 0; --size, p += vstride) {
    if (needs_filter2(p, hstride, thresh2, ithresh)) {
      if (hev(p, hstride, hev_thresh)) do_filter2(p, hstride);
      else do_filter6(p, hstride);
    }
  }
}

void filter_loop24(uint8_t* p, int hstride, int vstride, int size, int thresh, int ithresh, int hev_thresh) {
  const int thresh2 = 2 * thresh + 1;
  for (; size > 0; --size, p += vstride) {
    if (needs_filter2(p, hstride, thresh2, ithresh)) {
      if (hev(p, hstride, hev_thresh)) do_filter2(p, hstride);
      else do_filter4(p, hstride);
    }
  }
}

// ---- YUV to RGB, with libwebp's fixed-point constants ----
inline int mult_hi(int v, int coeff) { return (v * coeff) >> 8; }
inline int clip_yuv(int v) { return (v & ~((256 << 6) - 1)) == 0 ? (v >> 6) : (v < 0 ? 0 : 255); }
inline uint32_t yuv_to_argb(int y, int u, int v) {
  const int r = clip_yuv(mult_hi(y, 19077) + mult_hi(v, 26149) - 14234);
  const int g = clip_yuv(mult_hi(y, 19077) - mult_hi(u, 6419) - mult_hi(v, 13320) + 8708);
  const int b = clip_yuv(mult_hi(y, 19077) + mult_hi(u, 33050) - 17685);
  return 0xFF000000u | static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b);
}

// Converts two rows of luma (the second may be missing) using chroma interpolated from the four nearest samples: the rows'
// chroma is `top_*` and `cur_*` (see EmitFancyRGB in libwebp: for the first and last row of an even-height picture both are the same row).
void upsample_pair(const uint8_t* top_y, const uint8_t* bottom_y, const uint8_t* top_u, const uint8_t* top_v, const uint8_t* cur_u,
                   const uint8_t* cur_v, uint32_t* top_dst, uint32_t* bottom_dst, int len) {
  const int last_pair = (len - 1) >> 1;
  int tl_u = top_u[0], tl_v = top_v[0], l_u = cur_u[0], l_v = cur_v[0];
  top_dst[0] = yuv_to_argb(top_y[0], (3 * tl_u + l_u + 2) >> 2, (3 * tl_v + l_v + 2) >> 2);
  if (bottom_y) bottom_dst[0] = yuv_to_argb(bottom_y[0], (3 * l_u + tl_u + 2) >> 2, (3 * l_v + tl_v + 2) >> 2);
  for (int x = 1; x <= last_pair; ++x) {
    const int t_u = top_u[x], t_v = top_v[x], c_u = cur_u[x], c_v = cur_v[x];
    const int avg_u = tl_u + t_u + l_u + c_u + 8, avg_v = tl_v + t_v + l_v + c_v + 8;
    const int diag12_u = (avg_u + 2 * (t_u + l_u)) >> 3, diag12_v = (avg_v + 2 * (t_v + l_v)) >> 3;
    const int diag03_u = (avg_u + 2 * (tl_u + c_u)) >> 3, diag03_v = (avg_v + 2 * (tl_v + c_v)) >> 3;
    top_dst[2 * x - 1] = yuv_to_argb(top_y[2 * x - 1], (diag12_u + tl_u) >> 1, (diag12_v + tl_v) >> 1);
    top_dst[2 * x] = yuv_to_argb(top_y[2 * x], (diag03_u + t_u) >> 1, (diag03_v + t_v) >> 1);
    if (bottom_y) {
      bottom_dst[2 * x - 1] = yuv_to_argb(bottom_y[2 * x - 1], (diag03_u + l_u) >> 1, (diag03_v + l_v) >> 1);
      bottom_dst[2 * x] = yuv_to_argb(bottom_y[2 * x], (diag12_u + c_u) >> 1, (diag12_v + c_v) >> 1);
    }
    tl_u = t_u; tl_v = t_v; l_u = c_u; l_v = c_v;
  }
  if (!(len & 1)) {
    top_dst[len - 1] = yuv_to_argb(top_y[len - 1], (3 * tl_u + l_u + 2) >> 2, (3 * tl_v + l_v + 2) >> 2);
    if (bottom_y) bottom_dst[len - 1] = yuv_to_argb(bottom_y[len - 1], (3 * l_u + tl_u + 2) >> 2, (3 * l_v + tl_v + 2) >> 2);
  }
}

class Vp8Decoder {
 public:
  explicit Vp8Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Vp8Decoder() {
    release_view(allocator_, y_);
    release_view(allocator_, u_);
    release_view(allocator_, v_);
    allocator_.deallocate(finfo_);
    allocator_.deallocate(top_nz_);
    allocator_.deallocate(top_modes_);
    allocator_.deallocate(alpha_);
  }
  bool decode(ConstByteView frame, ConstByteView alph, Image* out);

 private:
  bool parse_headers(ConstByteView frame);
  void parse_proba();
  void parse_quant();
  void parse_intra_mode(int mb_x);
  int get_coeffs(BoolDecoder& br, const uint8_t (*prob)[3][11], int ctx, const int* dq, int n, int16_t* out);
  bool parse_residuals(BoolDecoder& br, int mb_x);
  void reconstruct(int mb_x, int mb_y);
  void filter_frame();
  void emit(Image* out);

  allocators::IAllocator& allocator_;
  BoolDecoder br_;
  BoolDecoder parts_[8];
  int num_parts_ = 1;
  uint32_t width_ = 0, height_ = 0, mb_w_ = 0, mb_h_ = 0;
  SegmentHeader seg_;
  FilterHeader filter_;
  int filter_type_ = 0;
  Quant dqm_[4];
  Probas proba_;
  bool use_skip_proba_ = false;
  int skip_p_ = 0;
  // Current macroblock.
  struct MbData {
    int segment = 0;
    bool skip = false, is_i4x4 = false;
    uint8_t imodes[16] = {};
    int uvmode = 0;
    int16_t coeffs[25 * 16];
    bool need[24];  // does block n (16 luma, 4 U, 4 V) need an inverse transform
    bool any_coeffs = false;
  } mb_;
  uint8_t left_modes_[4] = {};
  NzContext left_nz_ = {};
  NzContext* top_nz_ = nullptr;
  uint8_t* top_modes_ = nullptr;  // 4 per macroblock column
  MbFilter* finfo_ = nullptr;
  FilterInfo fstrengths_[4][2];
  ArrayView<uint8_t> y_, u_, v_;
  uint32_t y_stride_ = 0, uv_stride_ = 0;
  uint8_t* alpha_ = nullptr;
  uint8_t work_[kWorkSize];
};

bool Vp8Decoder::parse_headers(ConstByteView frame) {
  if (frame.size() < 10) return false;
  const uint32_t bits = frame[0] | (frame[1] << 8) | (frame[2] << 16);
  const bool key_frame = !(bits & 1);
  const uint32_t profile = (bits >> 1) & 7, show = (bits >> 4) & 1, partition_length = bits >> 5;
  if (!key_frame || profile > 3 || !show) return false;
  if (frame[3] != 0x9D || frame[4] != 0x01 || frame[5] != 0x2A) return false;
  width_ = (frame[6] | (frame[7] << 8)) & 0x3FFF;
  height_ = (frame[8] | (frame[9] << 8)) & 0x3FFF;
  if (width_ == 0 || height_ == 0 || uint64_t{width_} * height_ > Image::kMaxPixels) return false;
  mb_w_ = (width_ + 15) >> 4;
  mb_h_ = (height_ + 15) >> 4;
  const uint8_t* buf = frame.data() + 10;
  uint64_t left = frame.size() - 10;
  if (partition_length > left) return false;
  br_.init(buf, partition_length);
  buf += partition_length;
  left -= partition_length;
  for (uint8_t& s : proba_.segments) s = 255;
  br_.bit();  // colour space
  br_.bit();  // clamping type
  seg_.use_segment = br_.bit();
  if (seg_.use_segment) {
    seg_.update_map = br_.bit();
    if (br_.bit()) {  // update the segment data
      seg_.absolute_delta = br_.bit();
      for (int8_t& q : seg_.quantizer) q = static_cast<int8_t>(br_.bit() ? br_.signed_literal(7) : 0);
      for (int8_t& f : seg_.filter_strength) f = static_cast<int8_t>(br_.bit() ? br_.signed_literal(6) : 0);
    }
    if (seg_.update_map)
      for (uint8_t& p : proba_.segments) p = static_cast<uint8_t>(br_.bit() ? br_.literal(8) : 255);
  }
  filter_.simple = br_.bit();
  filter_.level = static_cast<int>(br_.literal(6));
  filter_.sharpness = static_cast<int>(br_.literal(3));
  filter_.use_lf_delta = br_.bit();
  if (filter_.use_lf_delta && br_.bit()) {
    for (int& d : filter_.ref_lf_delta)
      if (br_.bit()) d = br_.signed_literal(6);
    for (int& d : filter_.mode_lf_delta)
      if (br_.bit()) d = br_.signed_literal(6);
  }
  filter_type_ = filter_.level == 0 ? 0 : filter_.simple ? 1 : 2;
  if (br_.eof()) return false;
  // The residual partitions: sizes of all but the last, then the data.
  num_parts_ = 1 << br_.literal(2);
  const int last = num_parts_ - 1;
  if (left < 3u * static_cast<uint64_t>(last)) return false;
  const uint8_t* sizes = buf;
  const uint8_t* part = buf + 3 * last;
  uint64_t remaining = left - 3 * static_cast<uint64_t>(last);
  for (int p = 0; p < last; ++p) {
    uint64_t psize = sizes[0] | (sizes[1] << 8) | (sizes[2] << 16);
    if (psize > remaining) psize = remaining;
    parts_[p].init(part, psize);
    part += psize;
    remaining -= psize;
    sizes += 3;
  }
  parts_[last].init(part, remaining);
  if (remaining == 0) return false;  // no residual data at all
  parse_quant();
  br_.bit();  // refresh_entropy_probs: irrelevant for a single frame
  parse_proba();
  return !br_.eof();
}

void Vp8Decoder::parse_quant() {
  const int base_q0 = static_cast<int>(br_.literal(7));
  const int dqy1_dc = br_.bit() ? br_.signed_literal(4) : 0;
  const int dqy2_dc = br_.bit() ? br_.signed_literal(4) : 0;
  const int dqy2_ac = br_.bit() ? br_.signed_literal(4) : 0;
  const int dquv_dc = br_.bit() ? br_.signed_literal(4) : 0;
  const int dquv_ac = br_.bit() ? br_.signed_literal(4) : 0;
  auto clip = [](int v, int m) { return v < 0 ? 0 : v > m ? m : v; };
  for (int i = 0; i < 4; ++i) {
    int q;
    if (seg_.use_segment) {
      q = seg_.quantizer[i];
      if (!seg_.absolute_delta) q += base_q0;
    } else {
      if (i > 0) {
        dqm_[i] = dqm_[0];
        continue;
      }
      q = base_q0;
    }
    Quant& m = dqm_[i];
    m.y1[0] = kDcTable[clip(q + dqy1_dc, 127)];
    m.y1[1] = kAcTable[clip(q + 0, 127)];
    m.y2[0] = kDcTable[clip(q + dqy2_dc, 127)] * 2;
    m.y2[1] = (kAcTable[clip(q + dqy2_ac, 127)] * 101581) >> 16;
    if (m.y2[1] < 8) m.y2[1] = 8;
    m.uv[0] = kDcTable[clip(q + dquv_dc, 117)];
    m.uv[1] = kAcTable[clip(q + dquv_ac, 127)];
  }
}

void Vp8Decoder::parse_proba() {
  for (int t = 0; t < 4; ++t)
    for (int b = 0; b < 8; ++b)
      for (int c = 0; c < 3; ++c)
        for (int p = 0; p < 11; ++p) proba_.bands[t][b][c][p] = br_.get(CoeffsUpdateProba[t][b][c][p]) ? static_cast<uint8_t>(br_.literal(8)) : CoeffsProba0[t][b][c][p];
  use_skip_proba_ = br_.bit();
  if (use_skip_proba_) skip_p_ = static_cast<int>(br_.literal(8));
}

void Vp8Decoder::parse_intra_mode(int mb_x) {
  uint8_t* top = top_modes_ + 4 * mb_x;
  uint8_t* left = left_modes_;
  MbData& b = mb_;
  if (seg_.update_map) b.segment = !br_.get(proba_.segments[0]) ? br_.get(proba_.segments[1]) : br_.get(proba_.segments[2]) + 2;
  else b.segment = 0;
  b.skip = use_skip_proba_ ? br_.get(skip_p_) : false;
  b.is_i4x4 = !br_.get(145);
  if (!b.is_i4x4) {
    const int ymode = br_.get(156) ? (br_.get(128) ? B_TM_PRED : B_HE_PRED) : (br_.get(163) ? B_VE_PRED : B_DC_PRED);
    b.imodes[0] = static_cast<uint8_t>(ymode);
    for (int i = 0; i < 4; ++i) top[i] = left[i] = static_cast<uint8_t>(ymode);
  } else {
    uint8_t* modes = b.imodes;
    for (int y = 0; y < 4; ++y) {
      int ymode = left[y];
      for (int x = 0; x < 4; ++x) {
        const uint8_t* prob = kBModesProba[top[x]][ymode];
        ymode = !br_.get(prob[0])   ? B_DC_PRED
                : !br_.get(prob[1]) ? B_TM_PRED
                : !br_.get(prob[2]) ? B_VE_PRED
                : !br_.get(prob[3]) ? (!br_.get(prob[4]) ? B_HE_PRED : (!br_.get(prob[5]) ? B_RD_PRED : B_VR_PRED))
                                    : (!br_.get(prob[6]) ? B_LD_PRED : (!br_.get(prob[7]) ? B_VL_PRED : (!br_.get(prob[8]) ? B_HD_PRED : B_HU_PRED)));
        top[x] = static_cast<uint8_t>(ymode);
      }
      for (int i = 0; i < 4; ++i) modes[i] = top[i];
      modes += 4;
      left[y] = static_cast<uint8_t>(ymode);
    }
  }
  b.uvmode = !br_.get(142) ? B_DC_PRED : !br_.get(114) ? B_VE_PRED : br_.get(183) ? B_TM_PRED : B_HE_PRED;
}

int get_large_value(BoolDecoder& br, const uint8_t* p) {
  int v;
  if (!br.get(p[3])) {
    if (!br.get(p[4])) v = 2;
    else v = 3 + br.get(p[5]);
  } else {
    if (!br.get(p[6])) {
      if (!br.get(p[7])) {
        v = 5 + br.get(159);
      } else {
        v = 7 + 2 * br.get(165);
        v += br.get(145);
      }
    } else {
      const int bit1 = br.get(p[8]);
      const int bit0 = br.get(p[9 + bit1]);
      const int cat = 2 * bit1 + bit0;
      v = 0;
      for (const uint8_t* tab = kCat3456[cat]; *tab; ++tab) v += v + br.get(*tab);
      v += 3 + (8 << cat);
    }
  }
  return v;
}

// Reads the coefficients of one block into `out` (dequantised, in raster order). `prob` is the 8-band array of one block type.
// Returns the position of the last non-zero coefficient plus one.
int Vp8Decoder::get_coeffs(BoolDecoder& br, const uint8_t (*prob)[3][11], int ctx, const int* dq, int n, int16_t* out) {
  const uint8_t* p = prob[kBands[n]][ctx];
  for (; n < 16; ++n) {
    if (!br.get(p[0])) return n;  // the previous coefficient was the last non-zero one
    while (!br.get(p[1])) {       // a run of zeros
      p = prob[kBands[++n]][0];
      if (n == 16) return 16;
    }
    const uint8_t(*next)[11] = prob[kBands[n + 1]];
    int v;
    if (!br.get(p[2])) {
      v = 1;
      p = next[1];
    } else {
      v = get_large_value(br, p);
      p = next[2];
    }
    const int signed_v = br.bit() ? -v : v;
    out[kZigzag[n]] = static_cast<int16_t>(signed_v * dq[n > 0]);
  }
  return 16;
}

// Parses a macroblock's coefficients; returns true if there are none at all.
bool Vp8Decoder::parse_residuals(BoolDecoder& br, int mb_x) {
  MbData& b = mb_;
  NzContext& top = top_nz_[mb_x];
  NzContext& left = left_nz_;
  const Quant& q = dqm_[b.segment];
  for (int16_t& c : b.coeffs) c = 0;
  for (bool& n : b.need) n = false;
  int16_t* dst = b.coeffs;
  int first;
  const uint8_t(*ac_proba)[3][11];
  if (!b.is_i4x4) {  // the Y2 block holds the 16 luma DC values
    int16_t dc[16] = {};
    const int ctx = top.dc + left.dc;
    const int nz = get_coeffs(br, proba_.bands[1], ctx, q.y2, 0, dc);
    top.dc = left.dc = nz > 0;
    if (nz > 1) {
      transform_wht(dc, dst);
    } else {
      const int dc0 = (dc[0] + 3) >> 3;
      for (int i = 0; i < 16 * 16; i += 16) dst[i] = static_cast<int16_t>(dc0);
    }
    first = 1;
    ac_proba = proba_.bands[0];
  } else {
    first = 0;
    ac_proba = proba_.bands[3];
  }
  bool any = false;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      const int ctx = left.y[y] + top.y[x];
      const int nz = get_coeffs(br, ac_proba, ctx, q.y1, first, dst);
      top.y[x] = left.y[y] = nz > first;
      const bool need = nz > 1 || dst[0] != 0;
      b.need[y * 4 + x] = need;
      any = any || need;
      dst += 16;
    }
  for (int ch = 0; ch < 2; ++ch) {
    uint8_t* tn = ch == 0 ? top.u : top.v;
    uint8_t* ln = ch == 0 ? left.u : left.v;
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        const int ctx = ln[y] + tn[x];
        const int nz = get_coeffs(br, proba_.bands[2], ctx, q.uv, 0, dst);
        tn[x] = ln[y] = nz > 0;
        const bool need = nz > 1 || dst[0] != 0;
        b.need[16 + ch * 4 + y * 2 + x] = need;
        any = any || need;
        dst += 16;
      }
  }
  b.any_coeffs = any;
  return !any;
}

void Vp8Decoder::reconstruct(int mb_x, int mb_y) {
  uint8_t* const wy = work_ + kYOff;
  uint8_t* const wu = work_ + kUOff;
  uint8_t* const wv = work_ + kVOff;
  const MbData& b = mb_;
  // Neighbouring samples: from the (not yet loop-filtered) picture, or the fixed border values at its edges.
  const uint8_t* py = y_.data() + uint64_t{static_cast<uint32_t>(mb_y) * 16} * y_stride_ + static_cast<uint32_t>(mb_x) * 16;
  const uint8_t* pu = u_.data() + uint64_t{static_cast<uint32_t>(mb_y) * 8} * uv_stride_ + static_cast<uint32_t>(mb_x) * 8;
  const uint8_t* pv = v_.data() + uint64_t{static_cast<uint32_t>(mb_y) * 8} * uv_stride_ + static_cast<uint32_t>(mb_x) * 8;
  const int64_t ys = y_stride_, uvs = uv_stride_;  // (signed: the neighbours are above and to the left)
  for (int j = 0; j < 16; ++j) wy[j * BPS - 1] = mb_x > 0 ? py[j * ys - 1] : 129;
  for (int j = 0; j < 8; ++j) {
    wu[j * BPS - 1] = mb_x > 0 ? pu[j * uvs - 1] : 129;
    wv[j * BPS - 1] = mb_x > 0 ? pv[j * uvs - 1] : 129;
  }
  if (mb_y > 0) {
    for (int i = 0; i < 16; ++i) wy[i - BPS] = py[i - ys];
    for (int i = 0; i < 8; ++i) {
      wu[i - BPS] = pu[i - uvs];
      wv[i - BPS] = pv[i - uvs];
    }
    const uint8_t tl = mb_x > 0 ? py[-ys - 1] : 129;
    const uint8_t tlu = mb_x > 0 ? pu[-uvs - 1] : 129;
    const uint8_t tlv = mb_x > 0 ? pv[-uvs - 1] : 129;
    wy[-1 - BPS] = tl;
    wu[-1 - BPS] = tlu;
    wv[-1 - BPS] = tlv;
  } else {
    for (int i = -1; i < 16 + 4; ++i) wy[i - BPS] = 127;
    for (int i = -1; i < 8; ++i) wu[i - BPS] = wv[i - BPS] = 127;
  }
  const int16_t* coeffs = b.coeffs;
  static const uint16_t kScan[16] = {0 + 0 * BPS, 4 + 0 * BPS, 8 + 0 * BPS, 12 + 0 * BPS, 0 + 4 * BPS, 4 + 4 * BPS, 8 + 4 * BPS, 12 + 4 * BPS,
                                     0 + 8 * BPS, 4 + 8 * BPS, 8 + 8 * BPS, 12 + 8 * BPS, 0 + 12 * BPS, 4 + 12 * BPS, 8 + 12 * BPS, 12 + 12 * BPS};
  if (b.is_i4x4) {
    uint8_t* top_right = wy - BPS + 16;
    if (mb_y > 0) {
      if (mb_x >= static_cast<int>(mb_w_) - 1) {
        for (int i = 0; i < 4; ++i) top_right[i] = wy[15 - BPS];
      } else {
        for (int i = 0; i < 4; ++i) top_right[i] = py[16 + i - ys];
      }
    }
    for (int r = 1; r <= 3; ++r)
      for (int i = 0; i < 4; ++i) top_right[r * 4 * BPS + i] = top_right[i];  // the sub-blocks in the last column reuse them
    for (int n = 0; n < 16; ++n) {
      uint8_t* dst = wy + kScan[n];
      predict_4x4(b.imodes[n], dst);
      if (b.need[n]) transform_one(coeffs + n * 16, dst);
    }
  } else {
    const int mode = b.imodes[0];
    if (mode == B_DC_PRED) predict_dc(wy, 16, mb_y > 0, mb_x > 0);
    else if (mode == B_TM_PRED) predict_tm(wy, 16);
    else if (mode == B_VE_PRED) predict_v(wy, 16);
    else predict_h(wy, 16);
    for (int n = 0; n < 16; ++n)
      if (b.need[n]) transform_one(coeffs + n * 16, wy + kScan[n]);
  }
  const int uvmode = b.uvmode;
  uint8_t* const chroma[2] = {wu, wv};
  for (uint8_t* w : chroma) {
    if (uvmode == B_DC_PRED) predict_dc(w, 8, mb_y > 0, mb_x > 0);
    else if (uvmode == B_TM_PRED) predict_tm(w, 8);
    else if (uvmode == B_VE_PRED) predict_v(w, 8);
    else predict_h(w, 8);
  }
  for (int n = 0; n < 4; ++n) {
    const int off = (n & 1) * 4 + (n >> 1) * 4 * BPS;
    if (b.need[16 + n]) transform_one(coeffs + (16 + n) * 16, wu + off);
    if (b.need[20 + n]) transform_one(coeffs + (20 + n) * 16, wv + off);
  }
  // Store the macroblock into the picture.
  uint8_t* oy = y_.data() + uint64_t{static_cast<uint32_t>(mb_y) * 16} * y_stride_ + static_cast<uint32_t>(mb_x) * 16;
  uint8_t* ou = u_.data() + uint64_t{static_cast<uint32_t>(mb_y) * 8} * uv_stride_ + static_cast<uint32_t>(mb_x) * 8;
  uint8_t* ov = v_.data() + uint64_t{static_cast<uint32_t>(mb_y) * 8} * uv_stride_ + static_cast<uint32_t>(mb_x) * 8;
  for (int j = 0; j < 16; ++j)
    for (int i = 0; i < 16; ++i) oy[j * y_stride_ + i] = wy[j * BPS + i];
  for (int j = 0; j < 8; ++j)
    for (int i = 0; i < 8; ++i) {
      ou[j * uv_stride_ + i] = wu[j * BPS + i];
      ov[j * uv_stride_ + i] = wv[j * BPS + i];
    }
}

void Vp8Decoder::filter_frame() {
  if (filter_type_ == 0) return;
  // The strength of each segment's 16x16 and 4x4 macroblocks.
  for (int s = 0; s < 4; ++s) {
    int base_level;
    if (seg_.use_segment) {
      base_level = seg_.filter_strength[s];
      if (!seg_.absolute_delta) base_level += filter_.level;
    } else {
      base_level = filter_.level;
    }
    for (int i4x4 = 0; i4x4 <= 1; ++i4x4) {
      FilterInfo& info = fstrengths_[s][i4x4];
      int level = base_level;
      if (filter_.use_lf_delta) {
        level += filter_.ref_lf_delta[0];
        if (i4x4) level += filter_.mode_lf_delta[0];
      }
      level = level < 0 ? 0 : level > 63 ? 63 : level;
      if (level > 0) {
        int ilevel = level;
        if (filter_.sharpness > 0) {
          ilevel >>= filter_.sharpness > 4 ? 2 : 1;
          if (ilevel > 9 - filter_.sharpness) ilevel = 9 - filter_.sharpness;
        }
        if (ilevel < 1) ilevel = 1;
        info.ilevel = static_cast<uint8_t>(ilevel);
        info.limit = static_cast<uint8_t>(2 * level + ilevel);
        info.hev_thresh = level >= 40 ? 2 : level >= 15 ? 1 : 0;
      } else {
        info.limit = 0;
      }
    }
  }
  const int ys = static_cast<int>(y_stride_), uvs = static_cast<int>(uv_stride_);
  for (uint32_t mb_y = 0; mb_y < mb_h_; ++mb_y)
    for (uint32_t mb_x = 0; mb_x < mb_w_; ++mb_x) {
      const MbFilter& mf = finfo_[mb_y * mb_w_ + mb_x];
      const FilterInfo& f = fstrengths_[mf.segment][mf.i4x4 ? 1 : 0];
      const int limit = f.limit, ilevel = f.ilevel, hev_thresh = f.hev_thresh;
      if (limit == 0) continue;
      uint8_t* y = y_.data() + uint64_t{mb_y * 16} * y_stride_ + mb_x * 16;
      uint8_t* u = u_.data() + uint64_t{mb_y * 8} * uv_stride_ + mb_x * 8;
      uint8_t* v = v_.data() + uint64_t{mb_y * 8} * uv_stride_ + mb_x * 8;
      if (filter_type_ == 1) {  // simple: luma only
        if (mb_x > 0) simple_filter(y, 1, ys, 16, limit + 4);
        if (mf.inner)
          for (int k = 1; k <= 3; ++k) simple_filter(y + 4 * k, 1, ys, 16, limit);
        if (mb_y > 0) simple_filter(y, ys, 1, 16, limit + 4);
        if (mf.inner)
          for (int k = 1; k <= 3; ++k) simple_filter(y + 4 * k * ys, ys, 1, 16, limit);
      } else {
        if (mb_x > 0) {
          filter_loop26(y, 1, ys, 16, limit + 4, ilevel, hev_thresh);
          filter_loop26(u, 1, uvs, 8, limit + 4, ilevel, hev_thresh);
          filter_loop26(v, 1, uvs, 8, limit + 4, ilevel, hev_thresh);
        }
        if (mf.inner) {
          for (int k = 1; k <= 3; ++k) filter_loop24(y + 4 * k, 1, ys, 16, limit, ilevel, hev_thresh);
          filter_loop24(u + 4, 1, uvs, 8, limit, ilevel, hev_thresh);
          filter_loop24(v + 4, 1, uvs, 8, limit, ilevel, hev_thresh);
        }
        if (mb_y > 0) {
          filter_loop26(y, ys, 1, 16, limit + 4, ilevel, hev_thresh);
          filter_loop26(u, uvs, 1, 8, limit + 4, ilevel, hev_thresh);
          filter_loop26(v, uvs, 1, 8, limit + 4, ilevel, hev_thresh);
        }
        if (mf.inner) {
          for (int k = 1; k <= 3; ++k) filter_loop24(y + 4 * k * ys, ys, 1, 16, limit, ilevel, hev_thresh);
          filter_loop24(u + 4 * uvs, uvs, 1, 8, limit, ilevel, hev_thresh);
          filter_loop24(v + 4 * uvs, uvs, 1, 8, limit, ilevel, hev_thresh);
        }
      }
    }
}

// Converts the picture to ARGB, interpolating the chroma the way libwebp does, and applies the alpha plane.
void Vp8Decoder::emit(Image* out) {
  ArrayView<uint32_t> dst = out->frame(0);
  const int w = static_cast<int>(width_), h = static_cast<int>(height_);
  const uint8_t* yp = y_.data();
  const uint8_t* up = u_.data();
  const uint8_t* vp = v_.data();
  auto yrow = [&](int r) { return yp + uint64_t{static_cast<uint32_t>(r)} * y_stride_; };
  auto urow = [&](int r) { return up + uint64_t{static_cast<uint32_t>(r)} * uv_stride_; };
  auto vrow = [&](int r) { return vp + uint64_t{static_cast<uint32_t>(r)} * uv_stride_; };
  auto drow = [&](int r) { return dst.data() + uint64_t{static_cast<uint32_t>(r)} * static_cast<uint32_t>(w); };
  const int uv_rows = (h + 1) / 2;
  upsample_pair(yrow(0), nullptr, urow(0), vrow(0), urow(0), vrow(0), drow(0), nullptr, w);
  // Then pairs of luma rows (2k-1, 2k), each between chroma rows k-1 and k; an even-height picture ends on a single row, which
  // takes the last chroma row for both.
  for (int k = 1; 2 * k - 1 < h; ++k) {
    const bool has_bottom = 2 * k < h;
    const int cur = k < uv_rows ? k : uv_rows - 1;
    upsample_pair(yrow(2 * k - 1), has_bottom ? yrow(2 * k) : nullptr, urow(k - 1), vrow(k - 1), urow(cur), vrow(cur), drow(2 * k - 1),
                  has_bottom ? drow(2 * k) : nullptr, w);
  }
  if (alpha_) {
    bool has_alpha = false;
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) {
        const uint32_t a = alpha_[uint64_t{static_cast<uint32_t>(j)} * static_cast<uint32_t>(w) + static_cast<uint32_t>(i)];
        uint32_t& p = dst[uint64_t{static_cast<uint32_t>(j)} * static_cast<uint32_t>(w) + static_cast<uint32_t>(i)];
        p = (p & 0x00FFFFFFu) | (a << 24);
        has_alpha = has_alpha || a != 255;
      }
    out->set_has_alpha(has_alpha);
  }
}

bool Vp8Decoder::decode(ConstByteView frame, ConstByteView alph, Image* out) {
  if (!parse_headers(frame)) return false;
  y_stride_ = mb_w_ * 16;
  uv_stride_ = mb_w_ * 8;
  y_ = allocate_view<uint8_t>(allocator_, uint64_t{y_stride_} * mb_h_ * 16);
  u_ = allocate_view<uint8_t>(allocator_, uint64_t{uv_stride_} * mb_h_ * 8);
  v_ = allocate_view<uint8_t>(allocator_, uint64_t{uv_stride_} * mb_h_ * 8);
  finfo_ = static_cast<MbFilter*>(allocator_.allocate(sizeof(MbFilter) * mb_w_ * mb_h_));
  top_nz_ = static_cast<NzContext*>(allocator_.allocate(sizeof(NzContext) * mb_w_));
  top_modes_ = static_cast<uint8_t*>(allocator_.allocate(uint64_t{4} * mb_w_));
  if (y_.empty() || u_.empty() || v_.empty() || !finfo_ || !top_nz_ || !top_modes_) return false;
  for (uint32_t i = 0; i < mb_w_; ++i) top_nz_[i] = NzContext{};
  for (uint32_t i = 0; i < 4 * mb_w_; ++i) top_modes_[i] = B_DC_PRED;
  y_.fill(0);
  u_.fill(0);
  v_.fill(0);
  for (uint32_t mb_y = 0; mb_y < mb_h_; ++mb_y) {
    BoolDecoder& tokens = parts_[mb_y & (num_parts_ - 1)];
    left_nz_ = NzContext{};
    for (uint8_t& m : left_modes_) m = B_DC_PRED;
    for (uint32_t mb_x = 0; mb_x < mb_w_; ++mb_x) {
      parse_intra_mode(static_cast<int>(mb_x));
      if (br_.eof()) return false;
      bool skip = use_skip_proba_ ? mb_.skip : false;
      if (!skip) {
        skip = parse_residuals(tokens, static_cast<int>(mb_x));
      } else {
        NzContext& top = top_nz_[mb_x];
        const uint8_t dc_top = top.dc, dc_left = left_nz_.dc;
        top = NzContext{};
        left_nz_ = NzContext{};
        if (mb_.is_i4x4) {  // no Y2 block: its context is left alone
          top.dc = dc_top;
          left_nz_.dc = dc_left;
        }
        for (bool& n : mb_.need) n = false;
        mb_.any_coeffs = false;
      }
      if (tokens.eof()) return false;
      MbFilter& f = finfo_[mb_y * mb_w_ + mb_x];
      f.segment = static_cast<uint8_t>(mb_.segment);
      f.i4x4 = mb_.is_i4x4;
      f.inner = mb_.is_i4x4 || !skip;
      reconstruct(static_cast<int>(mb_x), static_cast<int>(mb_y));
    }
  }
  if (!alph.empty()) {
    alpha_ = static_cast<uint8_t*>(allocator_.allocate(uint64_t{width_} * height_));
    if (!alpha_ || !decode_alpha_plane(allocator_, alph, width_, height_, alpha_)) return false;
  }
  filter_frame();
  if (!out->create(width_, height_, 1)) return false;
  emit(out);
  return true;
}

}  // namespace

bool decode_vp8(ConstByteView frame, ConstByteView alph, Image* out) {
  out->clear();
  Vp8Decoder decoder(out->allocator());
  if (decoder.decode(frame, alph, out)) return true;
  out->clear();
  return false;
}

}  // namespace webp_internal
}  // namespace media
