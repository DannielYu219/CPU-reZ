#pragma once
// Reference table 1:1 from cpuz_arm64.exe static array @0x1402FA4F0 (file 0x2F8AF0).
// Entry: {name(utf16), id, single, multi}, stride 0x18, terminated by id=-1.
// Scoreboard (0x140005DAC+): display = measured*100.0f/ref (s8=100.0 @0x140006298), fcvtzs.
typedef struct { const char *name; int id; float single_ref; float multi_ref; } cpuz_ref_t;
static const cpuz_ref_t CPUZ_REFS[] = {
  {"NXP i.MX8MP", 4, 60.0f, 240.0f},
  {"Broadcom BCM2711", 4, 93.0f, 364.0f},
  {"Qualcomm Snapdragon(R) 810", 8, 69.0f, 415.0f},
  {"Qualcomm Snapdragon(R) 7c", 8, 177.0f, 621.0f},
  {"Broadcom BCM2712", 4, 260.0f, 960.0f},
  {"Qualcomm Snapdragon(R) 845", 8, 220.0f, 1120.0f},
  {"Rockchip RK3588S", 8, 224.0f, 1230.0f},
  {"Qualcomm Snapdragon(R) 860", 8, 268.0f, 1385.0f},
  {"Microsoft SQ2", 8, 345.0f, 1519.0f},
  {"NXP Layerscape LX2160A", 16, 133.0f, 2131.0f},
  {"Qualcomm Snapdragon(R) 8cx Gen3", 8, 563.0f, 3610.0f},
  {"Qualcomm Snapdragon(R) X Elite - X1E001DE", 12, 820.0f, 9165.0f},
  {"Ampere(R) Altra(R) Max M128-30", 128, 315.0f, 40640.0f},
};
#define CPUZ_REF_COUNT 13
#define CPUZ_REF_DEFAULT 11 // X Elite flagship
