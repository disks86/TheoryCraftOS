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

// MPEG-1/2/2.5 Audio Layer III (MP3) decoder.
//
// The Layer III decoding core (side information, scalefactors, Huffman decoding, stereo processing, reordering, alias
// reduction, IMDCT and the polyphase synthesis filterbank) and its constant tables are ported from minimp3 by Lieff,
// which is dedicated to the public domain under CC0 1.0 (https://github.com/lieff/minimp3): the scalar code paths only,
// restated in this code base's style. The file-level handling around it (ID3v2 skipping, frame sync, the Xing/Info
// and LAME tags for gapless trimming, assembling the result) is this project's own.
// Layers I and II decode too. Not handled: free-format bitrates; the CRC is skipped, not checked.
#include "Media/AudioCodecs.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

namespace {
constexpr int kMaxReservoirBytes = 511;
constexpr int kMaxFramePayload = 2304;
constexpr int kShortBlockType = 2;
constexpr int kStopBlockType = 3;

const uint8_t g_scf_long[8][23] = { { 6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54,0 }, { 12,12,12,12,12,12,16,20,24,28,32,40,48,56,64,76,90,2,2,2,2,2,0 }, { 6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54,0 }, { 6,6,6,6,6,6,8,10,12,14,16,18,22,26,32,38,46,54,62,70,76,36,0 }, { 6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54,0 }, { 4,4,4,4,4,4,6,6,8,8,10,12,16,20,24,28,34,42,50,54,76,158,0 }, { 4,4,4,4,4,4,6,6,6,8,10,12,16,18,22,28,34,40,46,54,54,192,0 }, { 4,4,4,4,4,4,6,6,8,10,12,16,20,24,30,38,46,56,68,84,102,26,0 } };
const uint8_t g_scf_short[8][40] = { { 4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 }, { 8,8,8,8,8,8,8,8,8,12,12,12,16,16,16,20,20,20,24,24,24,28,28,28,36,36,36,2,2,2,2,2,2,2,2,2,26,26,26,0 }, { 4,4,4,4,4,4,4,4,4,6,6,6,6,6,6,8,8,8,10,10,10,14,14,14,18,18,18,26,26,26,32,32,32,42,42,42,18,18,18,0 }, { 4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,32,32,32,44,44,44,12,12,12,0 }, { 4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 }, { 4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,22,22,22,30,30,30,56,56,56,0 }, { 4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,6,6,6,10,10,10,12,12,12,14,14,14,16,16,16,20,20,20,26,26,26,66,66,66,0 }, { 4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,8,8,8,12,12,12,16,16,16,20,20,20,26,26,26,34,34,34,42,42,42,12,12,12,0 } };
const uint8_t g_scf_mixed[8][40] = { { 6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 }, { 12,12,12,4,4,4,8,8,8,12,12,12,16,16,16,20,20,20,24,24,24,28,28,28,36,36,36,2,2,2,2,2,2,2,2,2,26,26,26,0 }, { 6,6,6,6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,14,14,14,18,18,18,26,26,26,32,32,32,42,42,42,18,18,18,0 }, { 6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,32,32,32,44,44,44,12,12,12,0 }, { 6,6,6,6,6,6,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,24,24,24,30,30,30,40,40,40,18,18,18,0 }, { 4,4,4,4,4,4,6,6,4,4,4,6,6,6,8,8,8,10,10,10,12,12,12,14,14,14,18,18,18,22,22,22,30,30,30,56,56,56,0 }, { 4,4,4,4,4,4,6,6,4,4,4,6,6,6,6,6,6,10,10,10,12,12,12,14,14,14,16,16,16,20,20,20,26,26,26,66,66,66,0 }, { 4,4,4,4,4,4,6,6,4,4,4,6,6,6,8,8,8,12,12,12,16,16,16,20,20,20,26,26,26,34,34,34,42,42,42,12,12,12,0 } };
const uint8_t g_scf_partitions[3][28] = { { 6,5,5, 5,6,5,5,5,6,5, 7,3,11,10,0,0, 7, 7, 7,0, 6, 6,6,3, 8, 8,5,0 }, { 8,9,6,12,6,9,9,9,6,9,12,6,15,18,0,0, 6,15,12,0, 6,12,9,6, 6,18,9,0 }, { 9,9,6,12,9,9,9,9,9,9,12,6,18,18,0,0,12,12,12,0,12, 9,9,6,15,12,9,0 } };
const uint8_t g_scfc_decode[16] = { 0,1,2,3, 12,5,6,7, 9,10,11,13, 14,15,18,19 };
const uint8_t g_mod[6*4] = { 5,5,4,4,5,5,4,1,4,3,1,1,5,6,6,1,4,4,4,1,4,3,1,1 };
const uint8_t g_preamp[10] = { 1,1,1,1,2,2,3,3,3,2 };
const float g_expfrac[4] = { 9.31322575e-10f,7.83145814e-10f,6.58544508e-10f,5.53767716e-10f };
const float g_pow43[129 + 16] = { 0,-1,-2.519842f,-4.326749f,-6.349604f,-8.549880f,-10.902724f,-13.390518f,-16.000000f,-18.720754f,-21.544347f,-24.463781f,-27.473142f,-30.567351f,-33.741992f,-36.993181f, 0,1,2.519842f,4.326749f,6.349604f,8.549880f,10.902724f,13.390518f,16.000000f,18.720754f,21.544347f,24.463781f,27.473142f,30.567351f,33.741992f,36.993181f,40.317474f,43.711787f,47.173345f,50.699631f,54.288352f,57.937408f,61.644865f,65.408941f,69.227979f,73.100443f,77.024898f,81.000000f,85.024491f,89.097188f,93.216975f,97.382800f,101.593667f,105.848633f,110.146801f,114.487321f,118.869381f,123.292209f,127.755065f,132.257246f,136.798076f,141.376907f,145.993119f,150.646117f,155.335327f,160.060199f,164.820202f,169.614826f,174.443577f,179.305980f,184.201575f,189.129918f,194.090580f,199.083145f,204.107210f,209.162385f,214.248292f,219.364564f,224.510845f,229.686789f,234.892058f,240.126328f,245.389280f,250.680604f,256.000000f,261.347174f,266.721841f,272.123723f,277.552547f,283.008049f,288.489971f,293.998060f,299.532071f,305.091761f,310.676898f,316.287249f,321.922592f,327.582707f,333.267377f,338.976394f,344.709550f,350.466646f,356.247482f,362.051866f,367.879608f,373.730522f,379.604427f,385.501143f,391.420496f,397.362314f,403.326427f,409.312672f,415.320884f,421.350905f,427.402579f,433.475750f,439.570269f,445.685987f,451.822757f,457.980436f,464.158883f,470.357960f,476.577530f,482.817459f,489.077615f,495.357868f,501.658090f,507.978156f,514.317941f,520.677324f,527.056184f,533.454404f,539.871867f,546.308458f,552.764065f,559.238575f,565.731879f,572.243870f,578.774440f,585.323483f,591.890898f,598.476581f,605.080431f,611.702349f,618.342238f,625.000000f,631.675540f,638.368763f,645.079578f };
const int16_t tabs[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 785,785,785,785,784,784,784,784,513,513,513,513,513,513,513,513,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256, -255,1313,1298,1282,785,785,785,785,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,290,288, -255,1313,1298,1282,769,769,769,769,529,529,529,529,529,529,529,529,528,528,528,528,528,528,528,528,512,512,512,512,512,512,512,512,290,288, -253,-318,-351,-367,785,785,785,785,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,819,818,547,547,275,275,275,275,561,560,515,546,289,274,288,258, -254,-287,1329,1299,1314,1312,1057,1057,1042,1042,1026,1026,784,784,784,784,529,529,529,529,529,529,529,529,769,769,769,769,768,768,768,768,563,560,306,306,291,259, -252,-413,-477,-542,1298,-575,1041,1041,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-383,-399,1107,1092,1106,1061,849,849,789,789,1104,1091,773,773,1076,1075,341,340,325,309,834,804,577,577,532,532,516,516,832,818,803,816,561,561,531,531,515,546,289,289,288,258, -252,-429,-493,-559,1057,1057,1042,1042,529,529,529,529,529,529,529,529,784,784,784,784,769,769,769,769,512,512,512,512,512,512,512,512,-382,1077,-415,1106,1061,1104,849,849,789,789,1091,1076,1029,1075,834,834,597,581,340,340,339,324,804,833,532,532,832,772,818,803,817,787,816,771,290,290,290,290,288,258, -253,-349,-414,-447,-463,1329,1299,-479,1314,1312,1057,1057,1042,1042,1026,1026,785,785,785,785,784,784,784,784,769,769,769,769,768,768,768,768,-319,851,821,-335,836,850,805,849,341,340,325,336,533,533,579,579,564,564,773,832,578,548,563,516,321,276,306,291,304,259, -251,-572,-733,-830,-863,-879,1041,1041,784,784,784,784,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-511,-527,-543,1396,1351,1381,1366,1395,1335,1380,-559,1334,1138,1138,1063,1063,1350,1392,1031,1031,1062,1062,1364,1363,1120,1120,1333,1348,881,881,881,881,375,374,359,373,343,358,341,325,791,791,1123,1122,-703,1105,1045,-719,865,865,790,790,774,774,1104,1029,338,293,323,308,-799,-815,833,788,772,818,803,816,322,292,307,320,561,531,515,546,289,274,288,258, -251,-525,-605,-685,-765,-831,-846,1298,1057,1057,1312,1282,785,785,785,785,784,784,784,784,769,769,769,769,512,512,512,512,512,512,512,512,1399,1398,1383,1367,1382,1396,1351,-511,1381,1366,1139,1139,1079,1079,1124,1124,1364,1349,1363,1333,882,882,882,882,807,807,807,807,1094,1094,1136,1136,373,341,535,535,881,775,867,822,774,-591,324,338,-671,849,550,550,866,864,609,609,293,336,534,534,789,835,773,-751,834,804,308,307,833,788,832,772,562,562,547,547,305,275,560,515,290,290, -252,-397,-477,-557,-622,-653,-719,-735,-750,1329,1299,1314,1057,1057,1042,1042,1312,1282,1024,1024,785,785,785,785,784,784,784,784,769,769,769,769,-383,1127,1141,1111,1126,1140,1095,1110,869,869,883,883,1079,1109,882,882,375,374,807,868,838,881,791,-463,867,822,368,263,852,837,836,-543,610,610,550,550,352,336,534,534,865,774,851,821,850,805,593,533,579,564,773,832,578,578,548,548,577,577,307,276,306,291,516,560,259,259, -250,-2107,-2507,-2764,-2909,-2974,-3007,-3023,1041,1041,1040,1040,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-767,-1052,-1213,-1277,-1358,-1405,-1469,-1535,-1550,-1582,-1614,-1647,-1662,-1694,-1726,-1759,-1774,-1807,-1822,-1854,-1886,1565,-1919,-1935,-1951,-1967,1731,1730,1580,1717,-1983,1729,1564,-1999,1548,-2015,-2031,1715,1595,-2047,1714,-2063,1610,-2079,1609,-2095,1323,1323,1457,1457,1307,1307,1712,1547,1641,1700,1699,1594,1685,1625,1442,1442,1322,1322,-780,-973,-910,1279,1278,1277,1262,1276,1261,1275,1215,1260,1229,-959,974,974,989,989,-943,735,478,478,495,463,506,414,-1039,1003,958,1017,927,942,987,957,431,476,1272,1167,1228,-1183,1256,-1199,895,895,941,941,1242,1227,1212,1135,1014,1014,490,489,503,487,910,1013,985,925,863,894,970,955,1012,847,-1343,831,755,755,984,909,428,366,754,559,-1391,752,486,457,924,997,698,698,983,893,740,740,908,877,739,739,667,667,953,938,497,287,271,271,683,606,590,712,726,574,302,302,738,736,481,286,526,725,605,711,636,724,696,651,589,681,666,710,364,467,573,695,466,466,301,465,379,379,709,604,665,679,316,316,634,633,436,436,464,269,424,394,452,332,438,363,347,408,393,448,331,422,362,407,392,421,346,406,391,376,375,359,1441,1306,-2367,1290,-2383,1337,-2399,-2415,1426,1321,-2431,1411,1336,-2447,-2463,-2479,1169,1169,1049,1049,1424,1289,1412,1352,1319,-2495,1154,1154,1064,1064,1153,1153,416,390,360,404,403,389,344,374,373,343,358,372,327,357,342,311,356,326,1395,1394,1137,1137,1047,1047,1365,1392,1287,1379,1334,1364,1349,1378,1318,1363,792,792,792,792,1152,1152,1032,1032,1121,1121,1046,1046,1120,1120,1030,1030,-2895,1106,1061,1104,849,849,789,789,1091,1076,1029,1090,1060,1075,833,833,309,324,532,532,832,772,818,803,561,561,531,560,515,546,289,274,288,258, -250,-1179,-1579,-1836,-1996,-2124,-2253,-2333,-2413,-2477,-2542,-2574,-2607,-2622,-2655,1314,1313,1298,1312,1282,785,785,785,785,1040,1040,1025,1025,768,768,768,768,-766,-798,-830,-862,-895,-911,-927,-943,-959,-975,-991,-1007,-1023,-1039,-1055,-1070,1724,1647,-1103,-1119,1631,1767,1662,1738,1708,1723,-1135,1780,1615,1779,1599,1677,1646,1778,1583,-1151,1777,1567,1737,1692,1765,1722,1707,1630,1751,1661,1764,1614,1736,1676,1763,1750,1645,1598,1721,1691,1762,1706,1582,1761,1566,-1167,1749,1629,767,766,751,765,494,494,735,764,719,749,734,763,447,447,748,718,477,506,431,491,446,476,461,505,415,430,475,445,504,399,460,489,414,503,383,474,429,459,502,502,746,752,488,398,501,473,413,472,486,271,480,270,-1439,-1455,1357,-1471,-1487,-1503,1341,1325,-1519,1489,1463,1403,1309,-1535,1372,1448,1418,1476,1356,1462,1387,-1551,1475,1340,1447,1402,1386,-1567,1068,1068,1474,1461,455,380,468,440,395,425,410,454,364,467,466,464,453,269,409,448,268,432,1371,1473,1432,1417,1308,1460,1355,1446,1459,1431,1083,1083,1401,1416,1458,1445,1067,1067,1370,1457,1051,1051,1291,1430,1385,1444,1354,1415,1400,1443,1082,1082,1173,1113,1186,1066,1185,1050,-1967,1158,1128,1172,1097,1171,1081,-1983,1157,1112,416,266,375,400,1170,1142,1127,1065,793,793,1169,1033,1156,1096,1141,1111,1155,1080,1126,1140,898,898,808,808,897,897,792,792,1095,1152,1032,1125,1110,1139,1079,1124,882,807,838,881,853,791,-2319,867,368,263,822,852,837,866,806,865,-2399,851,352,262,534,534,821,836,594,594,549,549,593,593,533,533,848,773,579,579,564,578,548,563,276,276,577,576,306,291,516,560,305,305,275,259, -251,-892,-2058,-2620,-2828,-2957,-3023,-3039,1041,1041,1040,1040,769,769,769,769,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,-511,-527,-543,-559,1530,-575,-591,1528,1527,1407,1526,1391,1023,1023,1023,1023,1525,1375,1268,1268,1103,1103,1087,1087,1039,1039,1523,-604,815,815,815,815,510,495,509,479,508,463,507,447,431,505,415,399,-734,-782,1262,-815,1259,1244,-831,1258,1228,-847,-863,1196,-879,1253,987,987,748,-767,493,493,462,477,414,414,686,669,478,446,461,445,474,429,487,458,412,471,1266,1264,1009,1009,799,799,-1019,-1276,-1452,-1581,-1677,-1757,-1821,-1886,-1933,-1997,1257,1257,1483,1468,1512,1422,1497,1406,1467,1496,1421,1510,1134,1134,1225,1225,1466,1451,1374,1405,1252,1252,1358,1480,1164,1164,1251,1251,1238,1238,1389,1465,-1407,1054,1101,-1423,1207,-1439,830,830,1248,1038,1237,1117,1223,1148,1236,1208,411,426,395,410,379,269,1193,1222,1132,1235,1221,1116,976,976,1192,1162,1177,1220,1131,1191,963,963,-1647,961,780,-1663,558,558,994,993,437,408,393,407,829,978,813,797,947,-1743,721,721,377,392,844,950,828,890,706,706,812,859,796,960,948,843,934,874,571,571,-1919,690,555,689,421,346,539,539,944,779,918,873,932,842,903,888,570,570,931,917,674,674,-2575,1562,-2591,1609,-2607,1654,1322,1322,1441,1441,1696,1546,1683,1593,1669,1624,1426,1426,1321,1321,1639,1680,1425,1425,1305,1305,1545,1668,1608,1623,1667,1592,1638,1666,1320,1320,1652,1607,1409,1409,1304,1304,1288,1288,1664,1637,1395,1395,1335,1335,1622,1636,1394,1394,1319,1319,1606,1621,1392,1392,1137,1137,1137,1137,345,390,360,375,404,373,1047,-2751,-2767,-2783,1062,1121,1046,-2799,1077,-2815,1106,1061,789,789,1105,1104,263,355,310,340,325,354,352,262,339,324,1091,1076,1029,1090,1060,1075,833,833,788,788,1088,1028,818,818,803,803,561,561,531,531,816,771,546,546,289,274,288,258, -253,-317,-381,-446,-478,-509,1279,1279,-811,-1179,-1451,-1756,-1900,-2028,-2189,-2253,-2333,-2414,-2445,-2511,-2526,1313,1298,-2559,1041,1041,1040,1040,1025,1025,1024,1024,1022,1007,1021,991,1020,975,1019,959,687,687,1018,1017,671,671,655,655,1016,1015,639,639,758,758,623,623,757,607,756,591,755,575,754,559,543,543,1009,783,-575,-621,-685,-749,496,-590,750,749,734,748,974,989,1003,958,988,973,1002,942,987,957,972,1001,926,986,941,971,956,1000,910,985,925,999,894,970,-1071,-1087,-1102,1390,-1135,1436,1509,1451,1374,-1151,1405,1358,1480,1420,-1167,1507,1494,1389,1342,1465,1435,1450,1326,1505,1310,1493,1373,1479,1404,1492,1464,1419,428,443,472,397,736,526,464,464,486,457,442,471,484,482,1357,1449,1434,1478,1388,1491,1341,1490,1325,1489,1463,1403,1309,1477,1372,1448,1418,1433,1476,1356,1462,1387,-1439,1475,1340,1447,1402,1474,1324,1461,1371,1473,269,448,1432,1417,1308,1460,-1711,1459,-1727,1441,1099,1099,1446,1386,1431,1401,-1743,1289,1083,1083,1160,1160,1458,1445,1067,1067,1370,1457,1307,1430,1129,1129,1098,1098,268,432,267,416,266,400,-1887,1144,1187,1082,1173,1113,1186,1066,1050,1158,1128,1143,1172,1097,1171,1081,420,391,1157,1112,1170,1142,1127,1065,1169,1049,1156,1096,1141,1111,1155,1080,1126,1154,1064,1153,1140,1095,1048,-2159,1125,1110,1137,-2175,823,823,1139,1138,807,807,384,264,368,263,868,838,853,791,867,822,852,837,866,806,865,790,-2319,851,821,836,352,262,850,805,849,-2399,533,533,835,820,336,261,578,548,563,577,532,532,832,772,562,562,547,547,305,275,560,515,290,290,288,258 };
const uint8_t tab32[] = { 130,162,193,209,44,28,76,140,9,9,9,9,9,9,9,9,190,254,222,238,126,94,157,157,109,61,173,205 };
const uint8_t tab33[] = { 252,236,220,204,188,172,156,140,124,108,92,76,60,44,28,12 };
const int16_t tabindex[2*16] = { 0,32,64,98,0,132,180,218,292,364,426,538,648,746,0,1126,1460,1460,1460,1460,1460,1460,1460,1460,1842,1842,1842,1842,1842,1842,1842,1842 };
const uint8_t g_linbits[] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,6,8,10,13,4,5,6,7,8,9,11,13 };
const float g_pan[7*2] = { 0,1,0.21132487f,0.78867513f,0.36602540f,0.63397460f,0.5f,0.5f,0.63397460f,0.36602540f,0.78867513f,0.21132487f,1,0 };
const float g_aa[2][8] = { {0.85749293f,0.88174200f,0.94962865f,0.98331459f,0.99551782f,0.99916056f,0.99989920f,0.99999316f}, {0.51449576f,0.47173197f,0.31337745f,0.18191320f,0.09457419f,0.04096558f,0.01419856f,0.00369997f} };
const float g_twid9[18] = { 0.73727734f,0.79335334f,0.84339145f,0.88701083f,0.92387953f,0.95371695f,0.97629601f,0.99144486f,0.99904822f,0.67559021f,0.60876143f,0.53729961f,0.46174861f,0.38268343f,0.30070580f,0.21643961f,0.13052619f,0.04361938f };
const float g_twid3[6] = { 0.79335334f,0.92387953f,0.99144486f, 0.60876143f,0.38268343f,0.13052619f };
const float g_mdct_window[2][18] = { { 0.99904822f,0.99144486f,0.97629601f,0.95371695f,0.92387953f,0.88701083f,0.84339145f,0.79335334f,0.73727734f,0.04361938f,0.13052619f,0.21643961f,0.30070580f,0.38268343f,0.46174861f,0.53729961f,0.60876143f,0.67559021f }, { 1,1,1,1,1,1,0.99144486f,0.92387953f,0.79335334f,0,0,0,0,0,0,0.13052619f,0.38268343f,0.60876143f } };
const float g_sec[24] = { 10.19000816f,0.50060302f,0.50241929f,3.40760851f,0.50547093f,0.52249861f,2.05778098f,0.51544732f,0.56694406f,1.48416460f,0.53104258f,0.64682180f,1.16943991f,0.55310392f,0.78815460f,0.97256821f,0.58293498f,1.06067765f,0.83934963f,0.62250412f,1.72244716f,0.74453628f,0.67480832f,5.10114861f };
const float g_win[] = { -1,26,-31,208,218,401,-519,2063,2000,4788,-5517,7134,5959,35640,-39336,74992, -1,24,-35,202,222,347,-581,2080,1952,4425,-5879,7640,5288,33791,-41176,74856, -1,21,-38,196,225,294,-645,2087,1893,4063,-6237,8092,4561,31947,-43006,74630, -1,19,-41,190,227,244,-711,2085,1822,3705,-6589,8492,3776,30112,-44821,74313, -1,17,-45,183,228,197,-779,2075,1739,3351,-6935,8840,2935,28289,-46617,73908, -1,16,-49,176,228,153,-848,2057,1644,3004,-7271,9139,2037,26482,-48390,73415, -2,14,-53,169,227,111,-919,2032,1535,2663,-7597,9389,1082,24694,-50137,72835, -2,13,-58,161,224,72,-991,2001,1414,2330,-7910,9592,70,22929,-51853,72169, -2,11,-63,154,221,36,-1064,1962,1280,2006,-8209,9750,-998,21189,-53534,71420, -2,10,-68,147,215,2,-1137,1919,1131,1692,-8491,9863,-2122,19478,-55178,70590, -3,9,-73,139,208,-29,-1210,1870,970,1388,-8755,9935,-3300,17799,-56778,69679, -3,8,-79,132,200,-57,-1283,1817,794,1095,-8998,9966,-4533,16155,-58333,68692, -4,7,-85,125,189,-83,-1356,1759,605,814,-9219,9959,-5818,14548,-59838,67629, -4,7,-91,117,177,-106,-1428,1698,402,545,-9416,9916,-7154,12980,-61289,66494, -5,6,-97,111,163,-127,-1498,1634,185,288,-9585,9838,-8540,11455,-62684,65290 };

struct BitStream {
  const uint8_t* buf;
  int pos, limit;
  void init(const uint8_t* data, int bytes) {
    buf = data;
    pos = 0;
    limit = bytes * 8;
  }
  uint32_t get(int n) {
    uint32_t next, cache = 0, s = pos & 7;
    int shl = n + static_cast<int>(s);
    const uint8_t* p = buf + (pos >> 3);
    if ((pos += n) > limit) return 0;
    next = *p++ & (255 >> s);
    while ((shl -= 8) > 0) {
      cache |= next << shl;
      next = *p++;
    }
    return cache | (next >> -shl);
  }
};

struct GranuleInfo {
  const uint8_t* sfbtab;
  uint16_t part_23_length, big_values, scalefac_compress;
  uint8_t global_gain, block_type, mixed_block_flag, n_long_sfb, n_short_sfb;
  uint8_t table_select[3], region_count[3], subblock_gain[3];
  uint8_t preflag, scalefac_scale, count1_table, scfsi;
};

// ---- header helpers (h points at the four header bytes) ----
inline bool hdr_is_mono(const uint8_t* h) { return (h[3] & 0xC0) == 0xC0; }
inline bool hdr_is_ms_stereo(const uint8_t* h) { return (h[3] & 0xE0) == 0x60; }
inline bool hdr_is_free_format(const uint8_t* h) { return (h[2] & 0xF0) == 0; }
inline bool hdr_is_crc(const uint8_t* h) { return !(h[1] & 1); }
inline bool hdr_padding_bit(const uint8_t* h) { return h[2] & 2; }
inline bool hdr_is_mpeg1(const uint8_t* h) { return h[1] & 8; }
inline bool hdr_not_mpeg25(const uint8_t* h) { return h[1] & 0x10; }
inline bool hdr_i_stereo(const uint8_t* h) { return h[3] & 0x10; }
inline bool hdr_ms_stereo(const uint8_t* h) { return h[3] & 0x20; }
inline int hdr_layer(const uint8_t* h) { return (h[1] >> 1) & 3; }  // 1 = Layer III, 2 = Layer II, 3 = Layer I
inline int hdr_bitrate_index(const uint8_t* h) { return h[2] >> 4; }
inline int hdr_sample_rate_index(const uint8_t* h) { return (h[2] >> 2) & 3; }
inline int hdr_my_sample_rate(const uint8_t* h) { return hdr_sample_rate_index(h) + (((h[1] >> 3) & 1) + ((h[1] >> 4) & 1)) * 3; }
inline bool hdr_is_frame_576(const uint8_t* h) { return (h[1] & 14) == 2; }
inline bool hdr_is_layer_1(const uint8_t* h) { return (h[1] & 6) == 6; }

bool hdr_valid(const uint8_t* h) {
  return h[0] == 0xFF && ((h[1] & 0xF0) == 0xF0 || (h[1] & 0xFE) == 0xE2) && hdr_layer(h) != 0 && hdr_bitrate_index(h) != 15 && hdr_sample_rate_index(h) != 3;
}

bool hdr_compare(const uint8_t* a, const uint8_t* b) {
  return hdr_valid(b) && ((a[1] ^ b[1]) & 0xFE) == 0 && ((a[2] ^ b[2]) & 0x0C) == 0 && !(hdr_is_free_format(a) ^ hdr_is_free_format(b));
}

unsigned hdr_bitrate_kbps(const uint8_t* h) {
  static const uint8_t halfrate[2][3][15] = {
      {{0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80}, {0, 16, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128}},
      {{0, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160}, {0, 16, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192}, {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224}},
  };
  return 2u * halfrate[hdr_is_mpeg1(h) ? 1 : 0][hdr_layer(h) - 1][hdr_bitrate_index(h)];
}

unsigned hdr_sample_rate_hz(const uint8_t* h) {
  static const unsigned hz[3] = {44100, 48000, 32000};
  return hz[hdr_sample_rate_index(h)] >> (hdr_is_mpeg1(h) ? 0 : 1) >> (hdr_not_mpeg25(h) ? 0 : 1);
}

unsigned hdr_frame_samples(const uint8_t* h) { return hdr_is_layer_1(h) ? 384 : (1152u >> (hdr_is_frame_576(h) ? 1 : 0)); }

// Bytes in the frame without padding; 0 for free format (not handled).
unsigned hdr_frame_bytes(const uint8_t* h) {
  unsigned bytes = hdr_frame_samples(h) * hdr_bitrate_kbps(h) * 125 / hdr_sample_rate_hz(h);
  if (hdr_is_layer_1(h)) bytes &= ~3u;
  return bytes;
}

unsigned hdr_padding(const uint8_t* h) { return hdr_padding_bit(h) ? (hdr_is_layer_1(h) ? 4 : 1) : 0; }

// ---- Layer III side information ----
// Returns main_data_begin, or -1 for a frame that cannot be right.
int read_side_info(BitStream* bs, GranuleInfo* gr, const uint8_t* hdr) {
  unsigned tables, scfsi = 0;
  int main_data_begin, part_23_sum = 0;
  int sr_idx = hdr_my_sample_rate(hdr);
  sr_idx -= (sr_idx != 0);
  int gr_count = hdr_is_mono(hdr) ? 1 : 2;
  if (hdr_is_mpeg1(hdr)) {
    gr_count *= 2;
    main_data_begin = static_cast<int>(bs->get(9));
    scfsi = bs->get(7 + gr_count);
  } else {
    main_data_begin = static_cast<int>(bs->get(8 + gr_count) >> gr_count);
  }
  do {
    if (hdr_is_mono(hdr)) scfsi <<= 4;
    gr->part_23_length = static_cast<uint16_t>(bs->get(12));
    part_23_sum += gr->part_23_length;
    gr->big_values = static_cast<uint16_t>(bs->get(9));
    if (gr->big_values > 288) return -1;
    gr->global_gain = static_cast<uint8_t>(bs->get(8));
    gr->scalefac_compress = static_cast<uint16_t>(bs->get(hdr_is_mpeg1(hdr) ? 4 : 9));
    gr->sfbtab = g_scf_long[sr_idx];
    gr->n_long_sfb = 22;
    gr->n_short_sfb = 0;
    if (bs->get(1)) {
      gr->block_type = static_cast<uint8_t>(bs->get(2));
      if (!gr->block_type) return -1;
      gr->mixed_block_flag = static_cast<uint8_t>(bs->get(1));
      gr->region_count[0] = 7;
      gr->region_count[1] = 255;
      if (gr->block_type == kShortBlockType) {
        scfsi &= 0x0F0F;
        if (!gr->mixed_block_flag) {
          gr->region_count[0] = 8;
          gr->sfbtab = g_scf_short[sr_idx];
          gr->n_long_sfb = 0;
          gr->n_short_sfb = 39;
        } else {
          gr->sfbtab = g_scf_mixed[sr_idx];
          gr->n_long_sfb = hdr_is_mpeg1(hdr) ? 8 : 6;
          gr->n_short_sfb = 30;
        }
      }
      tables = bs->get(10);
      tables <<= 5;
      gr->subblock_gain[0] = static_cast<uint8_t>(bs->get(3));
      gr->subblock_gain[1] = static_cast<uint8_t>(bs->get(3));
      gr->subblock_gain[2] = static_cast<uint8_t>(bs->get(3));
    } else {
      gr->block_type = 0;
      gr->mixed_block_flag = 0;
      tables = bs->get(15);
      gr->region_count[0] = static_cast<uint8_t>(bs->get(4));
      gr->region_count[1] = static_cast<uint8_t>(bs->get(3));
      gr->region_count[2] = 255;
    }
    gr->table_select[0] = static_cast<uint8_t>(tables >> 10);
    gr->table_select[1] = static_cast<uint8_t>((tables >> 5) & 31);
    gr->table_select[2] = static_cast<uint8_t>(tables & 31);
    gr->preflag = hdr_is_mpeg1(hdr) ? static_cast<uint8_t>(bs->get(1)) : (gr->scalefac_compress >= 500);
    gr->scalefac_scale = static_cast<uint8_t>(bs->get(1));
    gr->count1_table = static_cast<uint8_t>(bs->get(1));
    gr->scfsi = static_cast<uint8_t>((scfsi >> 12) & 15);
    scfsi <<= 4;
    ++gr;
  } while (--gr_count);
  if (part_23_sum + bs->pos > bs->limit + main_data_begin * 8) return -1;
  return main_data_begin;
}

void read_scalefactors(uint8_t* scf, uint8_t* ist_pos, const uint8_t* scf_size, const uint8_t* scf_count, BitStream* bitbuf, int scfsi) {
  int i, k;
  for (i = 0; i < 4 && scf_count[i]; i++, scfsi *= 2) {
    int cnt = scf_count[i];
    if (scfsi & 8) {
      for (int n = 0; n < cnt; ++n) scf[n] = ist_pos[n];
    } else {
      int bits = scf_size[i];
      if (!bits) {
        for (int n = 0; n < cnt; ++n) scf[n] = ist_pos[n] = 0;
      } else {
        int max_scf = (scfsi < 0) ? (1 << bits) - 1 : -1;
        for (k = 0; k < cnt; k++) {
          int s = static_cast<int>(bitbuf->get(bits));
          ist_pos[k] = static_cast<uint8_t>(s == max_scf ? -1 : s);
          scf[k] = static_cast<uint8_t>(s);
        }
      }
    }
    ist_pos += cnt;
    scf += cnt;
  }
  scf[0] = scf[1] = scf[2] = 0;
}

float ldexp_q2(float y, int exp_q2) {
  int e;
  do {
    e = exp_q2 < 30 * 4 ? exp_q2 : 30 * 4;
    y *= g_expfrac[e & 3] * static_cast<float>(1 << 30 >> (e >> 2));
  } while ((exp_q2 -= e) > 0);
  return y;
}

constexpr int kBitsDequantizerOut = -1;
constexpr int kMaxScf = 255 + kBitsDequantizerOut * 4 - 210;
constexpr int kMaxScfi = (kMaxScf + 3) & ~3;

void decode_scalefactors(const uint8_t* hdr, uint8_t* ist_pos, BitStream* bs, const GranuleInfo* gr, float* scf, int ch) {
  const uint8_t* scf_partition = g_scf_partitions[(gr->n_short_sfb ? 1 : 0) + (gr->n_long_sfb ? 0 : 1)];
  uint8_t scf_size[4], iscf[40];
  int i, scf_shift = gr->scalefac_scale + 1, gain_exp, scfsi = gr->scfsi;
  float gain;
  if (hdr_is_mpeg1(hdr)) {
    int part = g_scfc_decode[gr->scalefac_compress];
    scf_size[1] = scf_size[0] = static_cast<uint8_t>(part >> 2);
    scf_size[3] = scf_size[2] = static_cast<uint8_t>(part & 3);
  } else {
    int k, modprod, sfc, ist = hdr_i_stereo(hdr) && ch;
    sfc = gr->scalefac_compress >> ist;
    for (k = ist * 3 * 4; sfc >= 0; sfc -= modprod, k += 4) {
      for (modprod = 1, i = 3; i >= 0; i--) {
        scf_size[i] = static_cast<uint8_t>(sfc / modprod % g_mod[k + i]);
        modprod *= g_mod[k + i];
      }
    }
    scf_partition += k;
    scfsi = -16;
  }
  read_scalefactors(iscf, ist_pos, scf_size, scf_partition, bs, scfsi);
  if (gr->n_short_sfb) {
    int sh = 3 - scf_shift;
    for (i = 0; i < gr->n_short_sfb; i += 3) {
      iscf[gr->n_long_sfb + i + 0] = static_cast<uint8_t>(iscf[gr->n_long_sfb + i + 0] + (gr->subblock_gain[0] << sh));
      iscf[gr->n_long_sfb + i + 1] = static_cast<uint8_t>(iscf[gr->n_long_sfb + i + 1] + (gr->subblock_gain[1] << sh));
      iscf[gr->n_long_sfb + i + 2] = static_cast<uint8_t>(iscf[gr->n_long_sfb + i + 2] + (gr->subblock_gain[2] << sh));
    }
  } else if (gr->preflag) {
    for (i = 0; i < 10; i++) iscf[11 + i] = static_cast<uint8_t>(iscf[11 + i] + g_preamp[i]);
  }
  gain_exp = gr->global_gain + kBitsDequantizerOut * 4 - 210 - (hdr_is_ms_stereo(hdr) ? 2 : 0);
  gain = ldexp_q2(static_cast<float>(1 << (kMaxScfi / 4)), kMaxScfi - gain_exp);
  for (i = 0; i < gr->n_long_sfb + gr->n_short_sfb; i++) scf[i] = ldexp_q2(gain, iscf[i] << scf_shift);
}

float pow_43(int x) {
  float frac;
  int sign, mult = 256;
  if (x < 129) return g_pow43[16 + x];
  if (x < 1024) {
    mult = 16;
    x <<= 3;
  }
  sign = 2 * x & 64;
  frac = static_cast<float>((x & 63) - sign) / static_cast<float>((x & ~63) + sign);
  return g_pow43[16 + ((x + sign) >> 6)] * (1.f + frac * ((4.f / 3) + frac * (2.f / 9))) * static_cast<float>(mult);
}

// Huffman-decodes the granule's spectrum into `dst` (576 values), applying the scalefactors.
void huffman(float* dst, BitStream* bs, const GranuleInfo* gr_info, const float* scf, int layer3gr_limit) {
  float one = 0.0f;
  int ireg = 0, big_val_cnt = gr_info->big_values;
  const uint8_t* sfb = gr_info->sfbtab;
  const uint8_t* bs_next_ptr = bs->buf + bs->pos / 8;
  uint32_t bs_cache = (((bs_next_ptr[0] * 256u + bs_next_ptr[1]) * 256u + bs_next_ptr[2]) * 256u + bs_next_ptr[3]) << (bs->pos & 7);
  int pairs_to_decode, np, bs_sh = (bs->pos & 7) - 8;
  bs_next_ptr += 4;
  auto peek = [&](int n) { return bs_cache >> (32 - n); };
  auto flush = [&](int n) {
    bs_cache <<= n;
    bs_sh += n;
  };
  auto check = [&]() {
    while (bs_sh >= 0) {
      bs_cache |= static_cast<uint32_t>(*bs_next_ptr++) << bs_sh;
      bs_sh -= 8;
    }
  };
  while (big_val_cnt > 0) {
    int tab_num = gr_info->table_select[ireg];
    int sfb_cnt = gr_info->region_count[ireg++];
    const int16_t* codebook = tabs + tabindex[tab_num];
    int linbits = g_linbits[tab_num];
    do {
      np = *sfb++ / 2;
      pairs_to_decode = big_val_cnt < np ? big_val_cnt : np;
      one = *scf++;
      do {
        int j, w = 5;
        int leaf = codebook[peek(w)];
        while (leaf < 0) {
          flush(w);
          w = leaf & 7;
          leaf = codebook[peek(w) - (leaf >> 3)];
        }
        flush(leaf >> 8);
        for (j = 0; j < 2; j++, dst++, leaf >>= 4) {
          int lsb = leaf & 0x0F;
          if (linbits && lsb == 15) {
            lsb += static_cast<int>(peek(linbits));
            flush(linbits);
            check();
            *dst = one * pow_43(lsb) * (static_cast<int32_t>(bs_cache) < 0 ? -1.f : 1.f);
          } else {
            *dst = g_pow43[16 + lsb - 16 * static_cast<int>(bs_cache >> 31)] * one;
          }
          flush(lsb ? 1 : 0);
        }
        check();
      } while (--pairs_to_decode);
    } while ((big_val_cnt -= np) > 0 && --sfb_cnt >= 0);
  }
  for (np = 1 - big_val_cnt;; dst += 4) {
    const uint8_t* codebook_count1 = gr_info->count1_table ? tab33 : tab32;
    int leaf = codebook_count1[peek(4)];
    if (!(leaf & 8)) leaf = codebook_count1[(leaf >> 3) + (bs_cache << 4 >> (32 - (leaf & 3)))];
    flush(leaf & 7);
    if (((bs_next_ptr - bs->buf) * 8 - 24 + bs_sh) > layer3gr_limit) break;
    auto reload = [&]() {
      if (!--np) {
        np = *sfb++ / 2;
        if (!np) return false;
        one = *scf++;
      }
      return true;
    };
    auto deq = [&](int s) {
      if (leaf & (128 >> s)) {
        dst[s] = (static_cast<int32_t>(bs_cache) < 0) ? -one : one;
        flush(1);
      }
    };
    if (!reload()) break;
    deq(0);
    deq(1);
    if (!reload()) break;
    deq(2);
    deq(3);
    check();
  }
  bs->pos = layer3gr_limit;
}

void midside_stereo(float* left, int n) {
  float* right = left + 576;
  for (int i = 0; i < n; i++) {
    float a = left[i], b = right[i];
    left[i] = a + b;
    right[i] = a - b;
  }
}

void intensity_stereo_band(float* left, int n, float kl, float kr) {
  for (int i = 0; i < n; i++) {
    left[i + 576] = left[i] * kr;
    left[i] = left[i] * kl;
  }
}

void stereo_top_band(const float* right, const uint8_t* sfb, int nbands, int max_band[3]) {
  max_band[0] = max_band[1] = max_band[2] = -1;
  for (int i = 0; i < nbands; i++) {
    for (int k = 0; k < sfb[i]; k += 2) {
      if (right[k] != 0 || right[k + 1] != 0) {
        max_band[i % 3] = i;
        break;
      }
    }
    right += sfb[i];
  }
}

void stereo_process(float* left, const uint8_t* ist_pos, const uint8_t* sfb, const uint8_t* hdr, int max_band[3], int mpeg2_sh) {
  unsigned max_pos = hdr_is_mpeg1(hdr) ? 7 : 64;
  for (unsigned i = 0; sfb[i]; i++) {
    unsigned ipos = ist_pos[i];
    if (static_cast<int>(i) > max_band[i % 3] && ipos < max_pos) {
      float kl, kr, s = hdr_ms_stereo(hdr) ? 1.41421356f : 1;
      if (hdr_is_mpeg1(hdr)) {
        kl = g_pan[2 * ipos];
        kr = g_pan[2 * ipos + 1];
      } else {
        kl = 1;
        kr = ldexp_q2(1, static_cast<int>((ipos + 1) >> 1 << mpeg2_sh));
        if (ipos & 1) {
          kl = kr;
          kr = 1;
        }
      }
      intensity_stereo_band(left, sfb[i], kl * s, kr * s);
    } else if (hdr_ms_stereo(hdr)) {
      midside_stereo(left, sfb[i]);
    }
    left += sfb[i];
  }
}

void intensity_stereo(float* left, uint8_t* ist_pos, const GranuleInfo* gr, const uint8_t* hdr) {
  int max_band[3], n_sfb = gr->n_long_sfb + gr->n_short_sfb;
  int max_blocks = gr->n_short_sfb ? 3 : 1;
  stereo_top_band(left + 576, gr->sfbtab, n_sfb, max_band);
  if (gr->n_long_sfb) {
    int m = max_band[0] > max_band[1] ? max_band[0] : max_band[1];
    m = m > max_band[2] ? m : max_band[2];
    max_band[0] = max_band[1] = max_band[2] = m;
  }
  for (int i = 0; i < max_blocks; i++) {
    int default_pos = hdr_is_mpeg1(hdr) ? 3 : 0;
    int itop = n_sfb - max_blocks + i;
    int prev = itop - max_blocks;
    ist_pos[itop] = static_cast<uint8_t>(max_band[i] >= prev ? default_pos : ist_pos[prev]);
  }
  stereo_process(left, ist_pos, gr->sfbtab, hdr, max_band, gr[1].scalefac_compress & 1);
}

void reorder(float* grbuf, float* scratch, const uint8_t* sfb) {
  int i, len;
  float *src = grbuf, *dst = scratch;
  for (; 0 != (len = *sfb); sfb += 3, src += 2 * len) {
    for (i = 0; i < len; i++, src++) {
      *dst++ = src[0 * len];
      *dst++ = src[1 * len];
      *dst++ = src[2 * len];
    }
  }
  for (float* p = scratch; p < dst; ++p) grbuf[p - scratch] = *p;
}

void antialias(float* grbuf, int nbands) {
  for (; nbands > 0; nbands--, grbuf += 18) {
    for (int i = 0; i < 8; i++) {
      float u = grbuf[18 + i];
      float d = grbuf[17 - i];
      grbuf[18 + i] = u * g_aa[0][i] - d * g_aa[1][i];
      grbuf[17 - i] = u * g_aa[1][i] + d * g_aa[0][i];
    }
  }
}

void dct3_9(float* y) {
  float s0, s1, s2, s3, s4, s5, s6, s7, s8, t0, t2, t4;
  s0 = y[0]; s2 = y[2]; s4 = y[4]; s6 = y[6]; s8 = y[8];
  t0 = s0 + s6 * 0.5f;
  s0 -= s6;
  t4 = (s4 + s2) * 0.93969262f;
  t2 = (s8 + s2) * 0.76604444f;
  s6 = (s4 - s8) * 0.17364818f;
  s4 += s8 - s2;
  s2 = s0 - s4 * 0.5f;
  y[4] = s4 + s0;
  s8 = t0 - t2 + s6;
  s0 = t0 - t4 + t2;
  s4 = t0 + t4 - s6;
  s1 = y[1]; s3 = y[3]; s5 = y[5]; s7 = y[7];
  s3 *= 0.86602540f;
  t0 = (s5 + s1) * 0.98480775f;
  t4 = (s5 - s7) * 0.34202014f;
  t2 = (s1 + s7) * 0.64278761f;
  s1 = (s1 - s5 - s7) * 0.86602540f;
  s5 = t0 - s3 - t2;
  s7 = t4 - s3 - t0;
  s3 = t4 + s3 - t2;
  y[0] = s4 - s7;
  y[1] = s2 + s1;
  y[2] = s0 - s3;
  y[3] = s8 + s5;
  y[5] = s8 - s5;
  y[6] = s0 + s3;
  y[7] = s2 - s1;
  y[8] = s4 + s7;
}

void imdct36(float* grbuf, float* overlap, const float* window, int nbands) {
  for (int j = 0; j < nbands; j++, grbuf += 18, overlap += 9) {
    float co[9], si[9];
    co[0] = -grbuf[0];
    si[0] = grbuf[17];
    for (int i = 0; i < 4; i++) {
      si[8 - 2 * i] = grbuf[4 * i + 1] - grbuf[4 * i + 2];
      co[1 + 2 * i] = grbuf[4 * i + 1] + grbuf[4 * i + 2];
      si[7 - 2 * i] = grbuf[4 * i + 4] - grbuf[4 * i + 3];
      co[2 + 2 * i] = -(grbuf[4 * i + 3] + grbuf[4 * i + 4]);
    }
    dct3_9(co);
    dct3_9(si);
    si[1] = -si[1];
    si[3] = -si[3];
    si[5] = -si[5];
    si[7] = -si[7];
    for (int i = 0; i < 9; i++) {
      float ovl = overlap[i];
      float sum = co[i] * g_twid9[9 + i] + si[i] * g_twid9[0 + i];
      overlap[i] = co[i] * g_twid9[0 + i] - si[i] * g_twid9[9 + i];
      grbuf[i] = ovl * window[0 + i] - sum * window[9 + i];
      grbuf[17 - i] = ovl * window[9 + i] + sum * window[0 + i];
    }
  }
}

void idct3(float x0, float x1, float x2, float* dst) {
  float m1 = x1 * 0.86602540f;
  float a1 = x0 - x2 * 0.5f;
  dst[1] = x0 + x2;
  dst[0] = a1 + m1;
  dst[2] = a1 - m1;
}

void imdct12(float* x, float* dst, float* overlap) {
  float co[3], si[3];
  idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
  idct3(x[15], x[12] - x[9], x[6] - x[3], si);
  si[1] = -si[1];
  for (int i = 0; i < 3; i++) {
    float ovl = overlap[i];
    float sum = co[i] * g_twid3[3 + i] + si[i] * g_twid3[0 + i];
    overlap[i] = co[i] * g_twid3[0 + i] - si[i] * g_twid3[3 + i];
    dst[i] = ovl * g_twid3[2 - i] - sum * g_twid3[5 - i];
    dst[5 - i] = ovl * g_twid3[5 - i] + sum * g_twid3[2 - i];
  }
}

void imdct_short(float* grbuf, float* overlap, int nbands) {
  for (; nbands > 0; nbands--, overlap += 9, grbuf += 18) {
    float tmp[18];
    for (int i = 0; i < 18; ++i) tmp[i] = grbuf[i];
    for (int i = 0; i < 6; ++i) grbuf[i] = overlap[i];
    imdct12(tmp, grbuf + 6, overlap + 6);
    imdct12(tmp + 1, grbuf + 12, overlap + 6);
    imdct12(tmp + 2, overlap, overlap + 6);
  }
}

void change_sign(float* grbuf) {
  int b, i;
  for (b = 0, grbuf += 18; b < 32; b += 2, grbuf += 36)
    for (i = 1; i < 18; i += 2) grbuf[i] = -grbuf[i];
}

void imdct_gr(float* grbuf, float* overlap, unsigned block_type, unsigned n_long_bands) {
  if (n_long_bands) {
    imdct36(grbuf, overlap, g_mdct_window[0], static_cast<int>(n_long_bands));
    grbuf += 18 * n_long_bands;
    overlap += 9 * n_long_bands;
  }
  if (block_type == kShortBlockType) imdct_short(grbuf, overlap, 32 - static_cast<int>(n_long_bands));
  else imdct36(grbuf, overlap, g_mdct_window[block_type == kStopBlockType], 32 - static_cast<int>(n_long_bands));
}

// ---- synthesis filterbank ----
void dct_ii(float* grbuf, int n) {
  for (int k = 0; k < n; k++) {
    float t[4][8], *x, *y = grbuf + k;
    int i;
    for (x = t[0], i = 0; i < 8; i++, x++) {
      float x0 = y[i * 18];
      float x1 = y[(15 - i) * 18];
      float x2 = y[(16 + i) * 18];
      float x3 = y[(31 - i) * 18];
      float t0 = x0 + x3;
      float t1 = x1 + x2;
      float t2 = (x1 - x2) * g_sec[3 * i + 0];
      float t3 = (x0 - x3) * g_sec[3 * i + 1];
      x[0] = t0 + t1;
      x[8] = (t0 - t1) * g_sec[3 * i + 2];
      x[16] = t3 + t2;
      x[24] = (t3 - t2) * g_sec[3 * i + 2];
    }
    for (x = t[0], i = 0; i < 4; i++, x += 8) {
      float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7], xt;
      xt = x0 - x7; x0 += x7;
      x7 = x1 - x6; x1 += x6;
      x6 = x2 - x5; x2 += x5;
      x5 = x3 - x4; x3 += x4;
      x4 = x0 - x3; x0 += x3;
      x3 = x1 - x2; x1 += x2;
      x[0] = x0 + x1;
      x[4] = (x0 - x1) * 0.70710677f;
      x5 = x5 + x6;
      x6 = (x6 + x7) * 0.70710677f;
      x7 = x7 + xt;
      x3 = (x3 + x4) * 0.70710677f;
      x5 -= x7 * 0.198912367f;
      x7 += x5 * 0.382683432f;
      x5 -= x7 * 0.198912367f;
      x0 = xt - x6; xt += x6;
      x[1] = (xt + x7) * 0.50979561f;
      x[2] = (x4 + x3) * 0.54119611f;
      x[3] = (x0 - x5) * 0.60134488f;
      x[5] = (x0 + x5) * 0.89997619f;
      x[6] = (x4 - x3) * 1.30656302f;
      x[7] = (xt - x7) * 2.56291556f;
    }
    for (i = 0; i < 7; i++, y += 4 * 18) {
      y[0 * 18] = t[0][i];
      y[1 * 18] = t[2][i] + t[3][i] + t[3][i + 1];
      y[2 * 18] = t[1][i] + t[1][i + 1];
      y[3 * 18] = t[2][i + 1] + t[3][i] + t[3][i + 1];
    }
    y[0 * 18] = t[0][7];
    y[1 * 18] = t[2][7] + t[3][7];
    y[2 * 18] = t[1][7];
    y[3 * 18] = t[3][7];
  }
}

int16_t scale_pcm(float sample) {
  if (sample >= 32766.5f) return 32767;
  if (sample <= -32767.5f) return -32768;
  int16_t s = static_cast<int16_t>(sample + .5f);
  s -= (s < 0);  // away from zero, to be compliant
  return s;
}

void synth_pair(int16_t* pcm, int nch, const float* z) {
  float a;
  a = (z[14 * 64] - z[0]) * 29;
  a += (z[1 * 64] + z[13 * 64]) * 213;
  a += (z[12 * 64] - z[2 * 64]) * 459;
  a += (z[3 * 64] + z[11 * 64]) * 2037;
  a += (z[10 * 64] - z[4 * 64]) * 5153;
  a += (z[5 * 64] + z[9 * 64]) * 6574;
  a += (z[8 * 64] - z[6 * 64]) * 37489;
  a += z[7 * 64] * 75038;
  pcm[0] = scale_pcm(a);
  z += 2;
  a = z[14 * 64] * 104;
  a += z[12 * 64] * 1567;
  a += z[10 * 64] * 9727;
  a += z[8 * 64] * 64019;
  a += z[6 * 64] * -9975;
  a += z[4 * 64] * -45;
  a += z[2 * 64] * 146;
  a += z[0 * 64] * -5;
  pcm[16 * nch] = scale_pcm(a);
}

void synth(float* xl, int16_t* dstl, int nch, float* lins) {
  float* xr = xl + 576 * (nch - 1);
  int16_t* dstr = dstl + (nch - 1);
  float* zlin = lins + 15 * 64;
  const float* w = g_win;
  zlin[4 * 15] = xl[18 * 16];
  zlin[4 * 15 + 1] = xr[18 * 16];
  zlin[4 * 15 + 2] = xl[0];
  zlin[4 * 15 + 3] = xr[0];
  zlin[4 * 31] = xl[1 + 18 * 16];
  zlin[4 * 31 + 1] = xr[1 + 18 * 16];
  zlin[4 * 31 + 2] = xl[1];
  zlin[4 * 31 + 3] = xr[1];
  synth_pair(dstr, nch, lins + 4 * 15 + 1);
  synth_pair(dstr + 32 * nch, nch, lins + 4 * 15 + 64 + 1);
  synth_pair(dstl, nch, lins + 4 * 15);
  synth_pair(dstl + 32 * nch, nch, lins + 4 * 15 + 64);
  for (int i = 14; i >= 0; i--) {
    float a[4], b[4];
    zlin[4 * i] = xl[18 * (31 - i)];
    zlin[4 * i + 1] = xr[18 * (31 - i)];
    zlin[4 * i + 2] = xl[1 + 18 * (31 - i)];
    zlin[4 * i + 3] = xr[1 + 18 * (31 - i)];
    zlin[4 * (i + 16)] = xl[1 + 18 * (1 + i)];
    zlin[4 * (i + 16) + 1] = xr[1 + 18 * (1 + i)];
    zlin[4 * (i - 16) + 2] = xl[18 * (1 + i)];
    zlin[4 * (i - 16) + 3] = xr[18 * (1 + i)];
    for (int k = 0; k < 8; ++k) {
      const float w0 = *w++, w1 = *w++;
      const float* vz = &zlin[4 * i - k * 64];
      const float* vy = &zlin[4 * i - (15 - k) * 64];
      for (int j = 0; j < 4; j++) {
        if (k == 0) {
          b[j] = vz[j] * w1 + vy[j] * w0;
          a[j] = vz[j] * w0 - vy[j] * w1;
        } else if (k & 1) {  // S2
          b[j] += vz[j] * w1 + vy[j] * w0;
          a[j] += vy[j] * w1 - vz[j] * w0;
        } else {  // S1
          b[j] += vz[j] * w1 + vy[j] * w0;
          a[j] += vz[j] * w0 - vy[j] * w1;
        }
      }
    }
    dstr[(15 - i) * nch] = scale_pcm(a[1]);
    dstr[(17 + i) * nch] = scale_pcm(b[1]);
    dstl[(15 - i) * nch] = scale_pcm(a[0]);
    dstl[(17 + i) * nch] = scale_pcm(b[0]);
    dstr[(47 - i) * nch] = scale_pcm(a[3]);
    dstr[(49 + i) * nch] = scale_pcm(b[3]);
    dstl[(47 - i) * nch] = scale_pcm(a[2]);
    dstl[(49 + i) * nch] = scale_pcm(b[2]);
  }
}


// ---- Layers I and II ----
struct SubbandAlloc {
  uint8_t tab_offset, code_tab_width, band_count;
};

struct ScaleInfo {
  float scf[3 * 64];
  uint8_t total_bands, stereo_bands, bitalloc[64], scfcod[64];
};

inline int hdr_stereo_mode(const uint8_t* h) { return (h[3] >> 6) & 3; }
inline int hdr_stereo_mode_ext(const uint8_t* h) { return (h[3] >> 4) & 3; }

const SubbandAlloc* subband_alloc_table(const uint8_t* hdr, ScaleInfo* sci) {
  const SubbandAlloc* alloc;
  const int mode = hdr_stereo_mode(hdr);
  int nbands, stereo_bands = (mode == 3) ? 0 : (mode == 1) ? (hdr_stereo_mode_ext(hdr) << 2) + 4 : 32;
  if (hdr_is_layer_1(hdr)) {
    static const SubbandAlloc alloc_l1[] = {{76, 4, 32}};
    alloc = alloc_l1;
    nbands = 32;
  } else if (!hdr_is_mpeg1(hdr)) {
    static const SubbandAlloc alloc_l2m2[] = {{60, 4, 4}, {44, 3, 7}, {44, 2, 19}};
    alloc = alloc_l2m2;
    nbands = 30;
  } else {
    static const SubbandAlloc alloc_l2m1[] = {{0, 4, 3}, {16, 4, 8}, {32, 3, 12}, {40, 2, 7}};
    const int sample_rate_idx = hdr_sample_rate_index(hdr);
    unsigned kbps = hdr_bitrate_kbps(hdr) >> (mode != 3 ? 1 : 0);
    if (!kbps) kbps = 192;
    alloc = alloc_l2m1;
    nbands = 27;
    if (kbps < 56) {
      static const SubbandAlloc alloc_l2m1_lowrate[] = {{44, 4, 2}, {44, 3, 10}};
      alloc = alloc_l2m1_lowrate;
      nbands = sample_rate_idx == 2 ? 12 : 8;
    } else if (kbps >= 96 && sample_rate_idx != 1) {
      nbands = 30;
    }
  }
  sci->total_bands = static_cast<uint8_t>(nbands);
  sci->stereo_bands = static_cast<uint8_t>(stereo_bands < nbands ? stereo_bands : nbands);
  return alloc;
}

void l12_read_scalefactors(BitStream* bs, uint8_t* pba, uint8_t* scfcod, int bands, float* scf) {
  static const float deq_l12[18 * 3] = {
#define DQ(x) 9.53674316e-07f / x, 7.56931807e-07f / x, 6.00777173e-07f / x
      DQ(3), DQ(7), DQ(15), DQ(31), DQ(63), DQ(127), DQ(255), DQ(511), DQ(1023), DQ(2047), DQ(4095), DQ(8191), DQ(16383), DQ(32767), DQ(65535), DQ(3), DQ(5), DQ(9)
#undef DQ
  };
  for (int i = 0; i < bands; i++) {
    float s = 0;
    const int ba = *pba++;
    const int mask = ba ? 4 + ((19 >> scfcod[i]) & 3) : 0;
    for (int m = 4; m; m >>= 1) {
      if (mask & m) {
        const int b = static_cast<int>(bs->get(6));
        s = deq_l12[ba * 3 - 6 + b % 3] * static_cast<float>(1 << 21 >> b / 3);
      }
      *scf++ = s;
    }
  }
}

void l12_read_scale_info(const uint8_t* hdr, BitStream* bs, ScaleInfo* sci) {
  static const uint8_t bitalloc_code_tab[] = {0, 17, 3,  4,  5, 6, 7, 8,  9,  10, 11, 12, 13, 14, 15, 16, 0, 17, 18, 3,  19, 4, 5, 6,  7,  8,  9,  10, 11, 12, 13, 16,
                                              0, 17, 18, 3,  19, 4, 5, 16, 0,  17, 18, 16, 0,  17, 18, 19, 4, 5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 0,  17, 18, 3,
                                              19, 4, 5,  6,  7,  8, 9, 10, 11, 12, 13, 14, 0,  2,  3,  4,  5, 6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16};
  const SubbandAlloc* subband_alloc = subband_alloc_table(hdr, sci);
  int k = 0, ba_bits = 0;
  const uint8_t* ba_code_tab = bitalloc_code_tab;
  for (int i = 0; i < sci->total_bands; i++) {
    if (i == k) {
      k += subband_alloc->band_count;
      ba_bits = subband_alloc->code_tab_width;
      ba_code_tab = bitalloc_code_tab + subband_alloc->tab_offset;
      subband_alloc++;
    }
    uint8_t ba = ba_code_tab[bs->get(ba_bits)];
    sci->bitalloc[2 * i] = ba;
    if (i < sci->stereo_bands) ba = ba_code_tab[bs->get(ba_bits)];
    sci->bitalloc[2 * i + 1] = sci->stereo_bands ? ba : 0;
  }
  for (int i = 0; i < 2 * sci->total_bands; i++)
    sci->scfcod[i] = sci->bitalloc[i] ? (hdr_is_layer_1(hdr) ? 2 : static_cast<uint8_t>(bs->get(2))) : 6;
  l12_read_scalefactors(bs, sci->bitalloc, sci->scfcod, sci->total_bands * 2, sci->scf);
  for (int i = sci->stereo_bands; i < sci->total_bands; i++) sci->bitalloc[2 * i + 1] = 0;
}

int l12_dequantize_granule(float* grbuf, BitStream* bs, ScaleInfo* sci, int group_size) {
  int choff = 576;
  for (int j = 0; j < 4; j++) {
    float* dst = grbuf + group_size * j;
    for (int i = 0; i < 2 * sci->total_bands; i++) {
      const int ba = sci->bitalloc[i];
      if (ba != 0) {
        if (ba < 17) {
          const int half = (1 << (ba - 1)) - 1;
          for (int k = 0; k < group_size; k++) dst[k] = static_cast<float>(static_cast<int>(bs->get(ba)) - half);
        } else {
          const unsigned mod = (2u << (ba - 17)) + 1;  // 3, 5, 9
          unsigned code = bs->get(static_cast<int>(mod + 2 - (mod >> 3)));  // 5, 7, 10 bits
          for (int k = 0; k < group_size; k++, code /= mod) dst[k] = static_cast<float>(static_cast<int>(code % mod) - static_cast<int>(mod / 2));
        }
      }
      dst += choff;
      choff = 18 - choff;
    }
  }
  return group_size * 4;
}

void l12_apply_scf_384(ScaleInfo* sci, const float* scf, float* dst) {
  for (int i = 0; i < (sci->total_bands - sci->stereo_bands) * 18; ++i) dst[576 + sci->stereo_bands * 18 + i] = dst[sci->stereo_bands * 18 + i];
  for (int i = 0; i < sci->total_bands; i++, dst += 18, scf += 6) {
    for (int k = 0; k < 12; k++) {
      dst[k + 0] *= scf[0];
      dst[k + 576] *= scf[3];
    }
  }
}

class Mp3Decoder {
 public:
  Mp3Decoder() { reset(); }
  void reset() {
    reserv_ = 0;
    for (float& f : mdct_overlap_[0]) f = 0;
    for (float& f : mdct_overlap_[1]) f = 0;
    for (float& f : qmf_state_) f = 0;
  }
  // Decodes one Layer III frame (`frame` points at its header, `size` bytes with padding) into `pcm` (1152 or 576 frames
  // of `channels` samples). Returns the sample frames produced, 0 if the frame could not be decoded.
  int decode_frame(const uint8_t* frame, int size, int16_t* pcm);

 private:
  struct Scratch {
    BitStream bs;
    uint8_t maindata[kMaxReservoirBytes + kMaxFramePayload + 16];
    GranuleInfo gr_info[4];
    float grbuf[2][576], scf[40], syn[18 + 15][2 * 32];
    uint8_t ist_pos[2][39];
  };
  bool restore_reservoir(BitStream* bs, Scratch* s, int main_data_begin);
  void save_reservoir(Scratch* s);
  void decode_granule(Scratch* s, GranuleInfo* gr_info, int nch, const uint8_t* hdr);
  void synth_granule(float* grbuf, int nbands, int nch, int16_t* pcm, float* lins);

  uint8_t reserv_buf_[kMaxReservoirBytes + 16] = {};
  int reserv_ = 0;
  float mdct_overlap_[2][9 * 32];
  float qmf_state_[15 * 2 * 32];
  Scratch scratch_;
};

bool Mp3Decoder::restore_reservoir(BitStream* bs, Scratch* s, int main_data_begin) {
  int frame_bytes = (bs->limit - bs->pos) / 8;
  int bytes_have = reserv_ < main_data_begin ? reserv_ : main_data_begin;
  const int from = reserv_ - main_data_begin > 0 ? reserv_ - main_data_begin : 0;
  for (int i = 0; i < bytes_have; ++i) s->maindata[i] = reserv_buf_[from + i];
  for (int i = 0; i < frame_bytes; ++i) s->maindata[bytes_have + i] = bs->buf[bs->pos / 8 + i];
  for (int i = 0; i < 16; ++i) s->maindata[bytes_have + frame_bytes + i] = 0;  // the Huffman reader looks a few bytes ahead
  s->bs.init(s->maindata, bytes_have + frame_bytes);
  return reserv_ >= main_data_begin;
}

void Mp3Decoder::save_reservoir(Scratch* s) {
  int pos = (s->bs.pos + 7) / 8;
  int remains = s->bs.limit / 8 - pos;
  if (remains > kMaxReservoirBytes) {
    pos += remains - kMaxReservoirBytes;
    remains = kMaxReservoirBytes;
  }
  if (remains > 0)
    for (int i = 0; i < remains; ++i) reserv_buf_[i] = s->maindata[pos + i];  // (moving down within different buffers)
  reserv_ = remains > 0 ? remains : 0;
}

void Mp3Decoder::decode_granule(Scratch* s, GranuleInfo* gr_info, int nch, const uint8_t* hdr) {
  for (int ch = 0; ch < nch; ch++) {
    int layer3gr_limit = s->bs.pos + gr_info[ch].part_23_length;
    decode_scalefactors(hdr, s->ist_pos[ch], &s->bs, gr_info + ch, s->scf, ch);
    huffman(s->grbuf[ch], &s->bs, gr_info + ch, s->scf, layer3gr_limit);
  }
  if (hdr_i_stereo(hdr)) intensity_stereo(s->grbuf[0], s->ist_pos[1], gr_info, hdr);
  else if (hdr_is_ms_stereo(hdr)) midside_stereo(s->grbuf[0], 576);
  for (int ch = 0; ch < nch; ch++, gr_info++) {
    int aa_bands = 31;
    int n_long_bands = (gr_info->mixed_block_flag ? 2 : 0) << (hdr_my_sample_rate(hdr) == 2 ? 1 : 0);
    if (gr_info->n_short_sfb) {
      aa_bands = n_long_bands - 1;
      reorder(s->grbuf[ch] + n_long_bands * 18, s->syn[0], gr_info->sfbtab + gr_info->n_long_sfb);
    }
    antialias(s->grbuf[ch], aa_bands);
    imdct_gr(s->grbuf[ch], mdct_overlap_[ch], gr_info->block_type, static_cast<unsigned>(n_long_bands));
    change_sign(s->grbuf[ch]);
  }
}

void Mp3Decoder::synth_granule(float* grbuf, int nbands, int nch, int16_t* pcm, float* lins) {
  for (int i = 0; i < nch; i++) dct_ii(grbuf + 576 * i, nbands);
  for (int i = 0; i < 15 * 64; ++i) lins[i] = qmf_state_[i];
  for (int i = 0; i < nbands; i += 2) synth(grbuf + i, pcm + 32 * nch * i, nch, lins + i * 64);
  if (nch == 1) {
    for (int i = 0; i < 15 * 64; i += 2) qmf_state_[i] = lins[nbands * 64 + i];
  } else {
    for (int i = 0; i < 15 * 64; ++i) qmf_state_[i] = lins[nbands * 64 + i];
  }
}

int Mp3Decoder::decode_frame(const uint8_t* hdr, int frame_size, int16_t* pcm) {
  const int channels = hdr_is_mono(hdr) ? 1 : 2;
  const int layer = 4 - hdr_layer(hdr);
  Scratch* scratch = &scratch_;
  BitStream bs_frame;
  bs_frame.init(hdr + 4, frame_size - 4);
  if (hdr_is_crc(hdr)) bs_frame.get(16);
  if (layer == 3) {
    const int main_data_begin = read_side_info(&bs_frame, scratch->gr_info, hdr);
    if (main_data_begin < 0 || bs_frame.pos > bs_frame.limit) {
      reset();
      return 0;
    }
    const bool success = restore_reservoir(&bs_frame, scratch, main_data_begin);
    if (success) {
      for (int igr = 0; igr < (hdr_is_mpeg1(hdr) ? 2 : 1); igr++, pcm += 576 * channels) {
        for (float& f : scratch->grbuf[0]) f = 0;
        for (float& f : scratch->grbuf[1]) f = 0;
        decode_granule(scratch, scratch->gr_info + igr * channels, channels, hdr);
        synth_granule(scratch->grbuf[0], 18, channels, pcm, scratch->syn[0]);
      }
    }
    save_reservoir(scratch);
    return success ? static_cast<int>(hdr_frame_samples(hdr)) : 0;
  }
  // Layers I and II: scale information, then three granules (Layer II) of 12 samples per sub-band, or one (Layer I).
  ScaleInfo sci;
  l12_read_scale_info(hdr, &bs_frame, &sci);
  for (float& f : scratch->grbuf[0]) f = 0;
  for (float& f : scratch->grbuf[1]) f = 0;
  int i = 0;
  for (int igr = 0; igr < 3; igr++) {
    if (12 == (i += l12_dequantize_granule(scratch->grbuf[0] + i, &bs_frame, &sci, layer | 1))) {
      i = 0;
      l12_apply_scf_384(&sci, sci.scf + igr, scratch->grbuf[0]);
      synth_granule(scratch->grbuf[0], 12, channels, pcm, scratch->syn[0]);
      for (float& f : scratch->grbuf[0]) f = 0;
      for (float& f : scratch->grbuf[1]) f = 0;
      pcm += 384 * channels;
    }
    if (bs_frame.pos > bs_frame.limit) {
      reset();
      return 0;
    }
  }
  return static_cast<int>(hdr_frame_samples(hdr));
}

// Finds the next frame at or after `from`: a valid Layer III header whose frame fits and is followed by another matching
// header (or by the end of the data or a short tail, such as an ID3v1 tag).
bool find_frame(ConstByteView d, uint64_t from, uint64_t* at, uint32_t* size) {
  for (uint64_t i = from; i + 4 <= d.size(); ++i) {
    const uint8_t* h = d.data() + i;
    if (!hdr_valid(h)) continue;
    const unsigned frame_bytes = hdr_frame_bytes(h);
    if (frame_bytes < 24) continue;  // free format or implausibly small
    const uint64_t total = frame_bytes + hdr_padding(h);
    if (i + total > d.size()) continue;
    const uint64_t next = i + total;
    const bool ok = next + 4 > d.size() || d.size() - next <= 512 || hdr_compare(h, d.data() + next);
    if (!ok) continue;
    *at = i;
    *size = static_cast<uint32_t>(total);
    return true;
  }
  return false;
}

uint32_t be32(ConstByteView d, uint64_t at) { return (uint32_t{d[at]} << 24) | (uint32_t{d[at + 1]} << 16) | (uint32_t{d[at + 2]} << 8) | d[at + 3]; }
}  // namespace

bool decode_mp3(ConstByteView data, AudioData* out) {
  out->clear();
  if (detect_audio_format(data) != AudioFileFormat::Mp3) return false;
  allocators::IAllocator& allocator = out->allocator();
  uint64_t at = 0;
  if (data.size() >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {  // skip an ID3v2 tag
    at = 10 + ((uint64_t{data[6] & 0x7Fu} << 21) | (uint64_t{data[7] & 0x7Fu} << 14) | (uint64_t{data[8] & 0x7Fu} << 7) | (data[9] & 0x7Fu));
    if (data[5] & 0x10) at += 10;
  }
  UniquePointer<Mp3Decoder> decoder = UniquePointer<Mp3Decoder>::make(allocator);
  if (!decoder) return false;
  ByteBuffer pcm(allocator);
  uint32_t rate = 0, channels = 0, samples_per_frame = 0;
  bool first = true, ok = true, lame = false;
  uint32_t encoder_delay = 0, encoder_padding = 0, tag_frames = 0;
  int16_t frame_pcm[1152 * 2];
  uint64_t frame_at;
  uint32_t frame_size;
  while (ok && find_frame(data, at, &frame_at, &frame_size)) {
    const uint8_t* h = data.data() + frame_at;
    at = frame_at + frame_size;
    const uint32_t hz = hdr_sample_rate_hz(h), ch = hdr_is_mono(h) ? 1 : 2;
    if (first) {
      rate = hz;
      channels = ch;
      samples_per_frame = hdr_frame_samples(h);
      // A leading Xing/Info frame carries no sound: its tag may hold the frame count and the LAME encoder delay and padding.
      const uint64_t side = hdr_is_mpeg1(h) ? (ch == 1 ? 17 : 32) : (ch == 1 ? 9 : 17);
      const uint64_t tag = frame_at + 4 + (hdr_is_crc(h) ? 2 : 0) + side;
      if (tag + 8 <= frame_at + frame_size && ((data[tag] == 'X' && data[tag + 1] == 'i' && data[tag + 2] == 'n' && data[tag + 3] == 'g') ||
                                               (data[tag] == 'I' && data[tag + 1] == 'n' && data[tag + 2] == 'f' && data[tag + 3] == 'o'))) {
        const uint32_t flags = be32(data, tag + 4);
        uint64_t p = tag + 8;
        if (flags & 1) { tag_frames = p + 4 <= data.size() ? be32(data, p) : 0; p += 4; }
        if (flags & 2) p += 4;
        if (flags & 4) p += 100;
        if (flags & 8) p += 4;
        // The LAME extension (written by LAME and by ffmpeg's wrapper of it): a 9-byte encoder string, then (21 bytes in) the encoder delay and padding as two 12-bit numbers.
        if (p + 24 <= frame_at + frame_size && data[p] == 'L' && ((data[p + 1] == 'A' && data[p + 2] == 'M' && data[p + 3] == 'E') || (data[p + 1] == 'a' && data[p + 2] == 'v' && (data[p + 3] == 'c' || data[p + 3] == 'f')))) {
          encoder_delay = (uint32_t{data[p + 21]} << 4) | (data[p + 22] >> 4);
          encoder_padding = (uint32_t{data[p + 22] & 0x0Fu} << 8) | data[p + 23];
          lame = true;
        }
        first = false;
        continue;
      }
      first = false;
    }
    if (hz != rate || ch != channels) continue;  // a stream that changes format mid-way: the odd frames are dropped
    const int n = decoder->decode_frame(h, static_cast<int>(frame_size), frame_pcm);
    if (n > 0) ok = pcm.append(ConstByteView(reinterpret_cast<const uint8_t*>(frame_pcm), uint64_t{static_cast<uint32_t>(n)} * channels * 2));
  }
  decoder.reset();
  if (!ok || rate == 0) return false;
  uint64_t frames = pcm.size() / (channels * 2), skip = 0;
  if (lame && tag_frames > 0) {  // gapless playback: drop the encoder's delay at the start and padding at the end
    skip = uint64_t{encoder_delay} + 529;
    uint64_t end = uint64_t{tag_frames} * samples_per_frame + 529;
    end = end > encoder_padding ? end - encoder_padding : 0;
    if (end < frames) frames = end;
    if (skip >= frames) skip = 0;
  }
  frames -= skip;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  if (frames == 0 || !out->create(format, frames)) return false;
  __builtin_memcpy(out->samples().data(), pcm.view().data() + skip * channels * 2, frames * channels * 2);
  return true;
}

}  // namespace media
