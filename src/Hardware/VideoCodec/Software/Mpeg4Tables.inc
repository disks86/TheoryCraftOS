// The variable length codes of MPEG-4 Part 2 (ISO/IEC 14496-2 Annex B) and H.263 (ITU-T H.263 clause 5): a code is given as its value and its length in bits.
// Included by Mpeg4Syntax.cpp inside an anonymous namespace.

// B.1.1 / H.263 Table 7: mcbpc of I planes. Symbol: (dquant << 2) | cbpc; 8 is stuffing.
constexpr uint16_t kIntraMcbpcCode[9] = {1, 1, 2, 3, 1, 1, 2, 3, 1};
constexpr uint8_t kIntraMcbpcBits[9] = {1, 3, 3, 3, 4, 6, 6, 6, 9};

// B.1.1 / H.263 Table 8: mcbpc of P planes. Symbol: type * 4 + cbpc with the types inter, intra, inter+q, intra+q, inter4v; 20 is stuffing, 24..27 the inter4v+q of
// H.263 version 2 (no length: not decoded).
constexpr uint16_t kInterMcbpcCode[28] = {1, 3, 2, 5, 3, 4, 3, 3, 3, 7, 6, 5, 4, 4, 3, 2, 2, 5, 4, 5, 1, 0, 0, 0, 2, 12, 14, 15};
constexpr uint8_t kInterMcbpcBits[28] = {1, 4, 4, 6, 5, 8, 8, 7, 3, 7, 7, 9, 6, 9, 9, 9, 3, 7, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0};

// B.1.2 / H.263 Table 13: cbpy, the coded pattern of the luma blocks of an intra macroblock (inter macroblocks: the complement).
constexpr uint16_t kCbpyCode[16] = {3, 5, 4, 9, 3, 7, 2, 11, 2, 3, 5, 10, 4, 8, 6, 3};
constexpr uint8_t kCbpyBits[16] = {4, 5, 5, 4, 5, 4, 6, 4, 5, 6, 4, 4, 4, 4, 4, 2};

// B.1.3 / H.263 Table 14: the magnitude of a motion vector difference (a sign bit follows a non-zero one).
constexpr uint16_t kMvCode[33] = {1, 1, 1, 1, 3, 5, 4, 3, 11, 10, 9, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 7, 6, 5, 4, 3, 2, 3, 2};
constexpr uint8_t kMvBits[33] = {1, 2, 3, 4, 6, 7, 7, 7, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12};

// B.1.4 Tables B-13 and B-14: dct_dc_size of luma and chroma blocks.
constexpr uint16_t kDcLumaCode[13] = {3, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
constexpr uint8_t kDcLumaBits[13] = {3, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint16_t kDcChromaCode[13] = {3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
constexpr uint8_t kDcChromaBits[13] = {2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

// B.1.5: the modb-coded mb_type of B planes: direct, interpolate, backward, forward.
constexpr uint16_t kBTypeCode[4] = {1, 1, 1, 1};
constexpr uint8_t kBTypeBits[4] = {1, 2, 3, 4};

// The transform coefficient tables: 102 (run, level, last) events and the escape (the 103rd code). Events [0, last_start) have last = 0, the others last = 1.
struct CoefficientTable {
  uint16_t code[103];
  uint8_t bits[103];
  uint8_t run[102];
  uint8_t level[102];
  int last_start;
};

// B.1.6 Table B-16: intra macroblocks of MPEG-4.
constexpr CoefficientTable kIntraCoefficients = {
    {0x2,  0x6,  0xf,  0xd,  0xc,  0x15, 0x13, 0x12, 0x17, 0x1f, 0x1e, 0x1d, 0x25, 0x24, 0x23, 0x21, 0x21, 0x20, 0xf,  0xe,  0x7,  0x6,  0x20, 0x21, 0x50, 0x51,
     0x52, 0xe,  0x14, 0x16, 0x1c, 0x20, 0x1f, 0xd,  0x22, 0x53, 0x55, 0xb,  0x15, 0x1e, 0xc,  0x56, 0x11, 0x1b, 0x1d, 0xb,  0x10, 0x22, 0xa,  0xd,  0x1c, 0x8,
     0x12, 0x1b, 0x54, 0x14, 0x1a, 0x57, 0x19, 0x9,  0x18, 0x23, 0x17, 0x19, 0x18, 0x7,  0x58, 0x7,  0xc,  0x16, 0x17, 0x6,  0x5,  0x4,  0x59, 0xf,  0x16, 0x5,
     0xe,  0x4,  0x11, 0x24, 0x10, 0x25, 0x13, 0x5a, 0x15, 0x5b, 0x14, 0x13, 0x1a, 0x15, 0x14, 0x13, 0x12, 0x11, 0x26, 0x27, 0x5c, 0x5d, 0x5e, 0x5f, 0x3},
    {2,  3,  4,  5,  5,  6,  6,  6,  7,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 4,  6,  7,  8,  9,  9,  10, 11, 12, 12,
     5,  7,  9,  10, 12, 6,  8,  9,  10, 6,  9,  10, 6,  9,  10, 7,  9,  12, 7,  9,  12, 8,  10, 8,  11, 8,  9,  9,  10, 12, 4,  6,  8,  9,  10, 11, 11,
     12, 6,  9,  10, 6,  10, 7,  11, 7,  11, 7,  12, 8,  12, 8,  8,  8,  9,  9,  9,  9,  9,  11, 11, 12, 12, 12, 12, 7},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4,
     4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9, 10, 11, 12, 13, 14, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12,
     13, 14, 15, 16, 17, 18, 19, 20},
    {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1, 2, 3, 4, 5, 1,
     2, 3, 4, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1,
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    67};

// B.1.6 Table B-17 / H.263 Table 16: inter macroblocks of MPEG-4, and every block of H.263.
constexpr CoefficientTable kInterCoefficients = {
    {0x2,  0xf,  0x15, 0x17, 0x1f, 0x25, 0x24, 0x21, 0x20, 0x7,  0x6,  0x20, 0x6,  0x14, 0x1e, 0xf,  0x21, 0x50, 0xe,  0x1d, 0xe,  0x51, 0xd,  0x23, 0xd,  0xc,
     0x22, 0x52, 0xb,  0xc,  0x53, 0x13, 0xb,  0x54, 0x12, 0xa,  0x11, 0x9,  0x10, 0x8,  0x16, 0x55, 0x15, 0x14, 0x1c, 0x1b, 0x21, 0x20, 0x1f, 0x1e, 0x1d, 0x1c,
     0x1b, 0x1a, 0x22, 0x23, 0x56, 0x57, 0x7,  0x19, 0x5,  0xf,  0x4,  0xe,  0xd,  0xc,  0x13, 0x12, 0x11, 0x10, 0x1a, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13,
     0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x7,  0x6,  0x5,  0x4,  0x24, 0x25, 0x26, 0x27, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x3},
    {2,  4,  6,  7,  8,  9,  9,  10, 10, 11, 11, 11, 3,  6,  8,  10, 11, 12, 4,  8,  10, 12, 5,  9,  10, 5,  9,  12, 5,  10, 12, 6,  10, 12, 6,  10, 6,
     10, 6,  10, 7,  12, 7,  7,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  11, 11, 12, 12, 4,  9,  11, 6,  11, 6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  8,
     8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 7},
    {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  4,  4,  4,  5,  5,  5,  6,  6,  6,  7,  7,  8,
     8,  9,  9,  10, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 0,  0,  0,  1,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
     13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40},
    {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1,
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
     1, 1, 1, 1},
    58};

// 6.3.6: intra_dc_vlc_thr - the running quantiser below which the DC coefficient of an intra block is coded on its own (99: always; 0: never).
constexpr uint8_t kIntraDcThreshold[8] = {99, 13, 15, 17, 19, 21, 23, 0};

// 7.4.5 Figure 7-4: the alternate vertical scan (the alternate horizontal scan is its transpose).
constexpr uint8_t kAlternateVertical[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
                                             4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
                                             52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
