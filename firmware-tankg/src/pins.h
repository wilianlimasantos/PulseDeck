// Tank-G v1 pin map, derived OFFLINE from m571_v1_app_inner (stock firmware). NOT tested on hardware.
// Port register block: base 0x50000 + port*0x40; +0 OUT +4 IN +8 DIR(1=in) +12 DIE +16 PU +20 PD.
// Confidence: [V]=read directly from stock init code/tables, [I]=inferred/by analogy.
#pragma once
enum { PORT_A = 0, PORT_B = 1, PORT_C = 2, PORT_D = 3 };
// Footswitches: active-low, input + DIE + pull-up (setup loop at +0x15844, tables via r15)  [V]
//   FS1..FS4 = port base 0x501C0, bits 6,7,8,9   (0x501C0 = 0x50000 + 7*0x40 -> port index 7, "PH" in felucca naming)
#define TG_FS_PORT 7
static const unsigned char TG_FS_BIT[4] = {6, 7, 8, 9};
// 5th input: port 0 (PA) bit 10, same setup loop  [V]; role unknown. NOTE it is also ALNK0 pin group-0 entry [3].
#define TG_IN5_PORT PORT_A
#define TG_IN5_BIT 10
// Knob scan (stock fn +0xBD8E, +0xBC4E ADC read, +0xBBEE mux select)  [V code]: mux address = PC6..PC8 (output); per address read SARADC ch3 (PB1, start delay 15)
//   -> value[addr] and SARADC ch8 (start delay 2) -> value[addr+8], the latter only after the LED SPI/DMA finished. ch8 = PC9 is INFERRED (not read from a pin table).
//   Firmware: tg_knobs_scan() / serial KNOBS [ch]. UNTESTED ON HARDWARE. Which of the 16 slots is which knob is not known yet.
//   Footswitches (measured): FS1=PA10 FS2=PH9 FS3=PH8 FS4=PH6, active low. RGB LEDs: PC10 + SPI/DMA @0x11D00 (see ws_enc.h, rgb_map.h).
// Audio: ALNK0 @0x12E00, same register sequence as felucca-bsp Galaxia (gx_audio_init)  [V: +0x4C16 writes 0x12E00, IOMAP_CON5 0x51030 bits6-7]
//   pin table @0x02035360 (7-byte rows: DAT0,DAT1,DAT2,DAT3,MCLK,SCLK,LRCK)  [V]
//   group0 (IOMAP_CON5[7:6]=1): PA07 PA08 PA09 PA10 | PA04 PA05 PA06   (== Galaxia, hw-verified there)
//   group1: PC03 PC04 PC05 PC06 | PC00 PC01 PC02      group2: PG11..PG14 | PG08 PG09 PG10
//   which group the Tank-G selects = runtime config byte [0x01C268F0+1750]: UNKNOWN until read in a dump of a live unit.
// PD0 (0x500C0 bit 0) = chip-select of an SPI0 (0x11C00) slave [V]. NOT an LED (corrected after analysing the real M5.48 dump).
// Real Tank-G dump (M5.48): footswitch + ALNK tables identical; ALNK group row = RAM byte 0x01C22FFE, value unknown. PA10 is an input+pull-up in the stock setup loop [V]; it is ALNK DAT3 only if row 0 AND lane 3 enabled (unknown), so NOT a proven conflict. Also:
// ALNK DAT3 (row 0) and the 5th input: conflict, unresolved. LEDs: driver NOT identified. See analysis/HW_MAP_TANKG_M548.md.
