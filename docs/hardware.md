# Hardware: M-VAVE Tank-G v1

SoC: JieLi **AC791N** (WL82 family, `pi32v2` cores, two CPUs, 360 MHz under the stock boot loader), 1 MiB SPI NOR flash,
SRAM 0x01C00000..0x01C7FD50. Port registers: base `0x50000 + port*0x40`; `+0 OUT +4 IN +8 DIR(1=in) +12 DIE +16 PU +20 PD`.

## Panel (LEDs + 7-segment) — 29-bit shift register [measured]
| Signal | Pin |
|---|---|
| LATCH | PA0 (must be pulsed on **every** word) |
| /OE | PA1 |
| CLK | PA2 |
| DATA (inverted) | PA3 |

| Bits | Meaning |
|---|---|
| 0 | MASTER knob LED (also the base bit) |
| 1..14 | knob LEDs: IR CAB, RVB MIX, RVB DECAY, DLY TIME, DLY MIX, MOD SPEED, MOD FX, VOLUME, BASS, MIDDLE, TREBLE, GAIN, TYPE, NOISE GATE |
| 16..22 | 7-segment A..G |
| 23 | decimal point |
| 25 | blue/green BT LED |
| 28 | red channel of the MASTER LED |
| 15, 24, 26, 27 | no visible effect |

Driver: `firmware-tankg/src/led_sr.h`. Diagnostics: `LEDBIT n`, `LEDRAW hex`.

## Footswitches [measured]
Active low, input + DIE + pull-up: **FS1 (A) = PA10, FS2 (B) = PH9, FS3 (C) = PH8, FS4 (D) = PH6**. PH7 is configured by the
stock firmware but is not a footswitch. Debounce: 3 polls x 5 ms (`firmware-tankg/src/panel.h`).

## RGB rings [measured]
12 addressable pixels (3 per footswitch) on **PC10**, driven by SPI/DMA at `0x11D00` (divider 9, bit patterns 0x7E / 0x60).
Pixel order: `firmware-tankg/src/rgb_map.h`; encoder: `ws_enc.h`.

## Knobs [measured]
15 potentiometers behind two 8:1 multiplexers: address on **PC6..PC8**, read on SARADC **ch3 (PB1)** and **ch8**.
Slot order (knob index MASTER..NOISE GATE -> scan slot): `{0,4,2,1,6,5,3,7,8,12,10,14,9,13,11}` (`knob_map.h`).
Slot 15 = battery/supply (scale x6600/1023 mV, meaning [inferred]).

## Audio interface [measured]
I2S on **ALNK0** (`0x12E00`), pin group A (`IOMAP_CON5[7:6] = 01`):

| Signal | Pin |
|---|---|
| MCLK | PA4 |
| SCLK | PA5 |
| LRCK | PA6 |
| lane 0 TX (to codec) | PA7 |
| lane 1 RX (from codec) | PA8 |

Required static levels (as the stock firmware sets them): PC0=1, PC1=1, PC5=1, PA9=0. `CLK_CON2[11:8] = 1` (0 gives noise).
48 kHz, 24-bit in 32-bit slots, 128-frame blocks (2.666 ms), guitar on the left input. The codec is not identified and needs
no control bus that we found. Runtime knobs: `AUDIO SET <CLK|CON0|CON1|CON3|LEN> <hex>`.

## Second core
CPU1 starts with the felucca-bsp sequence (`fm1_cpu1.h`) [measured: starts and runs jobs on the author's unit,
`AUDIO STAT cpu1=1/0/0`]. felucca documents units whose CPU1 occasionally misreads shared RAM; the firmware retires it and
continues on one core if that happens.
