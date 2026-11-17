/* C side: felucca-bsp chip layer (MPL-2.0) USB CDC. UNTESTED ON HARDWARE. */
#include <stdint.h>
#define FELUCCA_CDC 1
#define FELUCCA_OTA 0
#define FM1_CPU1 1             /* second core (felucca fm1_cpu1.h, MPL-2.0): runs the effects while CPU0 runs the capture network */
#define FM1_CPU1_FALLBACK 1    /* a misread / fault / late job retires it: one core until power-off, no restart */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#ifdef TG_FLASH_STORE  /* region must be proven free on the Tank-G first (-DTG_STORE_OFF=0x... ) */
#define FM1_FLASH_WRITE_REGIONS(X) X((TG_STORE_OFF), (TG_STORE_OFF) + 2u * 3u * 4096u)   /* tg::LogStore::kRegionBytes = 24 KiB */
#else
#define FM1_FLASH_WRITE_REGIONS(X)
#endif
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_timer.h"
#include "fm1_guard.h"
#include "fm1_gpio.h"
#include "fm1_cpu1.h"
#ifdef TG_FLASH_STORE
#include "fm1_flash.h"
#endif
#include "libc.c"
#include "usb.c"
void fm1_timer5_irq(void) { static uint32_t sub; fm1_timer5_ack(); if (++sub == 5u) { sub = 0; usb_poll(); } }
extern void isr_timer5(void);
/* CPU1: started once at power-on, before the timers, the USB and the audio (the start that works on the device), interrupts off. */
int tg_cpu1_start(void) { return fm1_cpu1_start(); }
int tg_cpu1_run(void (*fn)(uint32_t), uint32_t arg) { return fm1_cpu1_run(fn, arg); }
void tg_cpu1_wait(void) { (void)fm1_cpu1_wait(); }   /* -1 = retired, the job dropped: the audio of that block is the stale buffer, then one core */
void tg_cpu1_stat(uint32_t o[4]) { o[0] = fm1_cpu1_on; o[1] = fm1_cpu1_gave_up; o[2] = fm1_cpu1_fault.count; o[3] = fm1_cpu1_stack_used(); }
void tg_usb_init(void) { fm1_time_init(); usb_start(); fm1_timer5_start(isr_timer5, 4); fm1_irq_enable_all(); }
void tg_enter_uboot(void) { usb_detach(); fm1_delay_ms(30); fm1_enter_uboot(); }   /* back to the mask-ROM UBOOT: backup/restore path */
void tg_usb_service(void) { fm1_wdt_feed(); usb_retry(0); if (usb.uboot_req) tg_enter_uboot(); }   /* also the stock SysEx F0 22 24 35 7D F7 */
int tg_usb_getc(void) { if (ci_r == ci_w) return -1; return cdc_in[ci_r++ % CI_N]; }
void tg_usb_putc(char c) { if (!cdc.dtr || co_w - co_r >= CO_N) return; cdc_out[co_w % CO_N] = (uint8_t)c; RING_PUBLISH(); co_w++; }

extern void tg_main(void);
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];
static volatile struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));
static void fm1_fault(const fm1_crash_t *c) { (void)c; fm1_reboot(); }
uint32_t tg_ticks(void) { return fm1_ticks(); }
void tg_healthy(void) { guard.pending = 0; guard.failed = 0; }
/* Same brick-safety as felucca hwtest: two boots in a row that never reach tg_healthy() drop into mask-ROM UBOOT. */
void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_wdt_arm(0x0D);
    if (guard.magic != 0x54474B31u || guard.pending > 1u || guard.failed > 1u) { guard.magic = 0x54474B31u; guard.failed = 0; guard.pending = 0; }
    if (guard.pending) guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) { guard.failed = 0; guard.pending = 0; fm1_enter_uboot(); }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    tg_main();
    for (;;) ;
}

#ifdef TG_FLASH_STORE
int tg_fl_read(uint32_t o, uint8_t* d, uint32_t n) { return fl_read(o, d, n) == 0; }
int tg_fl_erase(uint32_t o) { uint32_t us; return fl_erase4k(o, &us) == 0; }
int tg_fl_prog(uint32_t o, const uint8_t* s, uint32_t n) { return fl_write(o, s, n) == 0; }
#endif

/* Port A access for the LED/7-seg shift register (PA0 LATCH, PA1 /OE?, PA2 CLK, PA3 DATA). Only PA0..PA3 are ever touched. */
uint32_t tg_pa_out(void) { return FM1_PR(FM1_PA, FM1_OUT); }
void tg_pa_set(uint32_t v) { FM1_PR(FM1_PA, FM1_OUT) = (FM1_PR(FM1_PA, FM1_OUT) & ~0xFu) | (v & 0xFu); }
void tg_pa_nopull(uint32_t m) { m &= 0xFu; FM1_PR(FM1_PA, FM1_PU) &= ~m; FM1_PR(FM1_PA, FM1_PD) &= ~m; }
void tg_pa_dirout(uint32_t m) { m &= 0xFu; FM1_PR(FM1_PA, FM1_DIR) &= ~m; }
/* Footswitches, MEASURED on the real pedal with tools/port_probe.py (2026-11-15), all active LOW: FS1 = PA10, FS2 = PH9, FS3 = PH8, FS4 = PH6.
 * PH7 is also configured (stock sets PH6..9 + PA10) but is NOT a footswitch; it is reported separately as "aux" for diagnosis only.
 * Returns pressed bits (1 = pressed): bit0..3 = FS1..4, bit4 = PH7 (aux). rawA/rawH (optional) receive the raw IN registers.  Config: see tg_fs_read body. */
static int fs_ready;
uint32_t tg_fs_read(uint32_t* rawA, uint32_t* rawH) {
  if (!fs_ready) {   /* Same config as "PORTS PU", the one that DID show the presses on the real pedal: DIE + pull-up on all of PA4..15 and PH0..15.
                      * (The narrower stock-style setup of only PH6..9/PA10 read 00 on hardware, cause unknown.) Stays configured; never touches OUT/DIR. */
    FM1_PR(FM1_PH, FM1_PD) &= ~0xFFFFu; FM1_PR(FM1_PH, FM1_PU) |= 0xFFFFu; FM1_PR(FM1_PH, FM1_DIE) |= 0xFFFFu;
    FM1_PR(FM1_PA, FM1_PD) &= ~0xFFF0u; FM1_PR(FM1_PA, FM1_PU) |= 0xFFF0u; FM1_PR(FM1_PA, FM1_DIE) |= 0xFFF0u;
    fs_ready = 1; fm1_delay_ms(3);
  }
  uint32_t h = FM1_PR(FM1_PH, FM1_IN) & 0xFFFFu, a = FM1_PR(FM1_PA, FM1_IN) & 0xFFFFu;
  if (rawA) *rawA = a;
  if (rawH) *rawH = h;
  return ((~(a >> 10) & 1u)) | ((~(h >> 9) & 1u) << 1) | ((~(h >> 8) & 1u) << 2) | ((~(h >> 6) & 1u) << 3) | ((~(h >> 7) & 1u) << 4);
}
/* Diagnostic: input register of ports A..H (index 0..7) into out[]. pu=0: raw reads, no config change. pu=1: temporarily DIE+PU on all pins
 * (never PA0..PA3, never touches OUT/DIR), settle, read, then restore DIE/PU/PD exactly. */
void tg_ports_scan(int pu, uint32_t* out) {
  uint32_t die[8], pul[8], pdn[8]; int p;
  if (pu) {
    for (p = 0; p < 8; p++) {
      uint32_t m = (p == FM1_PA) ? 0xFFF0u : 0xFFFFu;
      die[p] = FM1_PR(p, FM1_DIE); pul[p] = FM1_PR(p, FM1_PU); pdn[p] = FM1_PR(p, FM1_PD);
      FM1_PR(p, FM1_PD) = pdn[p] & ~m; FM1_PR(p, FM1_PU) = pul[p] | m; FM1_PR(p, FM1_DIE) = die[p] | m;
    }
    fm1_delay_ms(3);
  }
  for (p = 0; p < 8; p++) out[p] = FM1_PR(p, FM1_IN) & 0xFFFFu;
  if (pu) for (p = 0; p < 8; p++) { FM1_PR(p, FM1_DIE) = die[p]; FM1_PR(p, FM1_PU) = pul[p]; FM1_PR(p, FM1_PD) = pdn[p]; }
}
/* RGB footswitch LEDs: SPI-like peripheral @0x11D00 + DMA, data pin PC10 (IOMAP 0x51020 bit4). Sequence copied from stock M5.48 fn +0x12ED8: first call
 * IOMAP|=0x10, PC10 OUT low + output, [+4]=9 (divider), [+0]=0x4021; per frame: if a frame is in flight wait for [+0]&0x8000, clear it with [+0]|=0x4000,
 * then [+12]=buffer, [+16]=length starts the DMA. UNTESTED ON HARDWARE; clock-gating/peripheral clock setup is assumed to be left on by the bootloader.
 * Returns 0 = started, -1 = previous frame still busy (timeout). div=0 keeps the stock value. */
#define WS_BASE 0x11D00u
#define WS_R(o) (*(volatile uint32_t *)(WS_BASE + (o)))
static int ws_ready, ws_inflight; static uint32_t ws_div_cur = 9;
int tg_ws_send(const uint8_t* buf, uint32_t n, uint32_t div) {
  if (div && div != ws_div_cur) { ws_div_cur = div; if (ws_ready) WS_R(4) = ws_div_cur; }
  if (!ws_ready) {
    *(volatile uint32_t *)0x51020u |= 0x10u;
    FM1_PR(FM1_PC, FM1_OUT) &= ~0x400u; FM1_PR(FM1_PC, FM1_DIR) &= ~0x400u;
    WS_R(4) = ws_div_cur; WS_R(0) = 0x4021u; ws_ready = 1;
  }
  if (ws_inflight) {
    uint32_t t0 = fm1_ticks();
    while (!(WS_R(0) & 0x8000u)) if ((uint32_t)(fm1_ticks() - t0) > 5000u * FM1_TICKS_PER_US) return -1;
    WS_R(0) |= 0x4000u; ws_inflight = 0;
  }
  WS_R(12) = (uint32_t)(uintptr_t)buf; WS_R(16) = n; ws_inflight = 1;
  return 0;
}
void tg_delay_us(uint32_t us);
/* Knob scan (14-15 pots behind two 8:1 analog muxes). Reverse engineered from stock M5.48 fn +0xBD8E / +0xBC4E / +0xBBEE [V code]:
 *   mux address = PC6..PC8 (OUT bits 8:6), PC6..8 set up as digital outputs (DIE on, no pulls, DIR out);
 *   mux A output  = SARADC channel 3 (PB1), read with start delay 15;   mux B output = SARADC channel 8 (read with start delay 2) [role of channel 8 = PC9: inferred];
 *   the stock reads channel 8 only when the LED SPI/DMA at 0x11D00 has finished (bit 0x8000), so we wait for the LED frame first.
 * ADC register protocol = felucca-bsp boards/fm1/fm1_adc.h (0x13100 CON, 0x13104 result 10 bit, clock div 6). PB1 and PC9 are ONLY ever analog inputs
 * (DIE off, no pulls, never output; PB1 held low for 8 s would reset the chip). Results: out[0..7] = ch3 at mux 0..7, out[8..15] = pc9_ch at mux 0..7;
 * 0xFFFF = timeout. The mux is returned to address 0 afterwards, as the stock does. */
#define ADC_CON (*(volatile uint32_t *)0x13100u)
#define ADC_RES (*(volatile uint32_t *)0x13104u)
#define ADC_WLA (*(volatile uint32_t *)0x11900u)
static uint16_t adc_read10(uint32_t ch, uint32_t delay) {
  uint32_t t, v;
  ADC_CON = 0;
  if (ADC_WLA & (1u << 14)) ADC_WLA &= ~(1u << 14);   /* as fm1_adc_start() */
  ADC_CON = ((6u | 8u | ((delay & 15u) << 12)) & 0xF00Fu) | 0x40u | ((ch & 0xFu) << 8);
  ADC_CON |= 0x10u; ADC_CON |= 0x40u;
  for (t = 0; t < 20000u; t++) if (ADC_CON & 0x80u) break;
  if (t == 20000u) { ADC_CON = 0x40u; ADC_CON = 0; return 0xFFFFu; }
  v = ADC_RES & 0x3FFu; ADC_CON = 0x40u; ADC_CON = 0;
  return (uint16_t)v;
}
void tg_knobs_scan(uint32_t pc9_ch, uint16_t out[16]) {
  static int init; uint32_t a;
  if (!init) {
    const uint32_t pb = 1u << 1, pc9 = 1u << 9, mux = 7u << 6;
    FM1_PR(FM1_PB, FM1_DIE) &= ~pb;  FM1_PR(FM1_PB, FM1_PU) &= ~pb;  FM1_PR(FM1_PB, FM1_PD) &= ~pb;  FM1_PR(FM1_PB, FM1_DIR) |= pb;     /* analog input */
    FM1_PR(FM1_PC, FM1_DIE) &= ~pc9; FM1_PR(FM1_PC, FM1_PU) &= ~pc9; FM1_PR(FM1_PC, FM1_PD) &= ~pc9; FM1_PR(FM1_PC, FM1_DIR) |= pc9;   /* analog input */
    FM1_PR(FM1_PC, FM1_PU) &= ~mux;  FM1_PR(FM1_PC, FM1_PD) &= ~mux;  FM1_PR(FM1_PC, FM1_OUT) &= ~mux;
    FM1_PR(FM1_PC, FM1_DIE) |= mux;  FM1_PR(FM1_PC, FM1_DIR) &= ~mux;                                                               /* mux select outputs, address 0 */
    init = 1;
  }
  if (ws_inflight) {   /* stock waits for the LED DMA to finish before reading channel 8 */
    uint32_t t0 = fm1_ticks();
    while (!(WS_R(0) & 0x8000u)) if ((uint32_t)(fm1_ticks() - t0) > 5000u * FM1_TICKS_PER_US) break;
    if (WS_R(0) & 0x8000u) { WS_R(0) |= 0x4000u; ws_inflight = 0; }
  }
  for (a = 0; a < 8; a++) {
    FM1_PR(FM1_PC, FM1_OUT) = (FM1_PR(FM1_PC, FM1_OUT) & ~(7u << 6)) | (a << 6);
    tg_delay_us(30);
    out[a] = adc_read10(3, 15);
    out[8 + a] = ws_inflight ? 0xFFFFu : adc_read10(pc9_ch, 2);
  }
  FM1_PR(FM1_PC, FM1_OUT) &= ~(7u << 6);
}
void tg_delay_us(uint32_t us) { uint32_t t0 = fm1_ticks(); while ((uint32_t)(fm1_ticks() - t0) < us * FM1_TICKS_PER_US) {} }

/* monotonic milliseconds from the free-running tick counter (call at least once per ~60 s so the 32-bit tick wrap is tracked) */
uint32_t tg_millis(void) {
    static uint32_t last, ms, rem; static int init;
    uint32_t now = fm1_ticks();
    if (!init) { last = now; init = 1; }
    rem += (uint32_t)(now - last) / FM1_TICKS_PER_US; last = now;   /* whole microseconds */
    ms += rem / 1000u; rem %= 1000u;
    return ms;
}

/* ---- Audio: ALNK0 I2S, port A group (IOMAP_CON5[7:6] = 01), lanes 0 (tx, PA7) and 1 (rx, PA8), MCLK PA4, SCLK PA5, LRCK PA6, optional codec reset PA9.
 * Register sequence = felucca-bsp boards/galaxia/gx_board.h gx_audio_init (hardware-verified on the M-VAVE Galaxia). For the Tank-G it is ONLY INFERRED:
 * port-A group chosen by the stock config byte (M5.48 +0x4466, byte 0), and no codec chip / control bus has been identified; PA9 as codec reset is a Galaxia assumption.
 * PA10 (FS1) is never touched (lane 3 unused). PA0..3 (panel shift register) untouched. UNTESTED ON HARDWARE. */
/* STOCK PIN STATES (M5.48, audio task init at +0x4A98..0x4AB0, verified in code): output PC0=1, PC1=1, PC5=1 and PA9=0, set BEFORE the ALNK init,
 * and PA9 is NEVER driven high anywhere in that path. (Our first try used the Galaxia polarity, PA9 low then HIGH: no sound on the Tank-G.)
 * What PC0/PC1/PC5 do is unknown (codec control / amp enable / mute release are only guesses). -DTG_AUDIO_STOCK_PINS=0 skips them,
 * -DTG_AUDIO_PA9_HIGH=1 restores the Galaxia behaviour (release PA9 high after the clocks run). */
#ifndef TG_AUDIO_STOCK_PINS
#define TG_AUDIO_STOCK_PINS 1
#endif
#ifndef TG_AUDIO_PA9_HIGH
#define TG_AUDIO_PA9_HIGH 0
#endif
#define AU_FR 128u                      /* frames per DMA half (2.9 ms) */
#define AU_HW (AU_FR * 2u)              /* int32 words per half (L,R) */
#define AL_CON0 (*(volatile uint16_t *)0x12E00u)
#define AL_CON1 (*(volatile uint16_t *)0x12E04u)
#define AL_CON2 (*(volatile uint8_t *)0x12E08u)
#define AL_CON3 (*(volatile uint8_t *)0x12E0Cu)
#define AL_ADR0 (*(volatile uint32_t *)0x12E10u)
#define AL_ADR1 (*(volatile uint32_t *)0x12E14u)
#define AL_LEN (*(volatile uint16_t *)0x12E20u)
#define AU_CLK_CON2 (*(volatile uint32_t *)0x10014u)
#define AU_IOMAP5 (*(volatile uint32_t *)0x51030u)
extern void tg_audio_process(const int32_t *in, int32_t *out, uint32_t frames);   /* C++ (audio.h), runs in the ISR */
extern void isr_alnk0(void);
static int32_t au_tx[2u * AU_HW] __attribute__((aligned(4)));
static int32_t au_rx[2u * AU_HW] __attribute__((aligned(4)));
static volatile uint32_t au_on, au_irqs, au_missed, au_last_ticks, au_max_ticks;
/* ALNK settings. DEFAULTS = what the stock M5.48 code ends up with (verified in +0x4466/+0x4860/+0x48C4), except where noted:
 *  CON0 0x180 (bit7 master, bit8 per [1568]=1), CON3 0x83, CON1 0x00D5 (lane0 tx, lane1 rx 24 bit; the stock start code also ORs bit0 of every lane
 *  nibble, i.e. 0x1111 - try CON1=11D5), CLK_CON2[11:8]: stock does |=0x100 and &=~0x200 (Galaxia/FM-1 clear the whole nibble -> 0).  All changeable at run time (AUDIO SET). */
static uint32_t au_clk = 1u, au_con0 = 0x180u, au_con1 = 0xD5u, au_con3 = 0x83u, au_len = AU_HW;
int tg_audio_set(const char *name, uint32_t v) {   /* returns 0 ok, -1 unknown/invalid; takes effect at the next start */
  if (!name[0] || name[1] == 0) return -1;
  if (name[0] == 'C' && name[1] == 'L' && name[2] == 'K' && !name[3]) { if (v > 15u) return -1; au_clk = v; return 0; }
  if (name[0] == 'C' && name[1] == 'O' && name[2] == 'N' && name[4] == 0) {
    if (name[3] == '0') { au_con0 = v & 0x7FFu; return 0; }   /* never sets the run bit 0x800 here */
    if (name[3] == '1') { au_con1 = v & 0xFFFFu; return 0; }
    if (name[3] == '3') { au_con3 = v & 0xFFu; return 0; }
  }
  if (name[0] == 'L' && name[1] == 'E' && name[2] == 'N' && !name[3]) { if (v < 16u || v > AU_HW || (v & 1u)) return -1; au_len = v; return 0; }
  return -1;
}
void tg_audio_cfg(uint32_t o[5]) { o[0] = au_clk; o[1] = au_con0; o[2] = au_con1; o[3] = au_con3; o[4] = au_len; }
void fm1_alnk0_irq(void) {
  uint8_t p = (uint8_t)((AL_CON2 >> 4) & 15u);
  AL_CON2 = (uint8_t)(p & 15u);   /* ack, as gx_audio_ack */
  au_irqs++;
  if (p & 2u) {   /* lane 1 received a half: process it into the matching tx half */
    uint32_t h = ((AL_CON0 >> 12) & 1u) ^ 1u;
    uint32_t t0 = fm1_ticks();   /* TIMER4, 24 MHz crystal: how long the DSP takes per block (AUDIO STAT load=) */
    tg_audio_process(au_rx + h * au_len, au_tx + h * au_len, au_len / 2u);
    t0 = fm1_ticks() - t0; au_last_ticks = t0; if (t0 > au_max_ticks) au_max_ticks = t0;
  }
  if ((p & 3u) == 0u) au_missed++;
}
#define AU_OUT_M ((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7))
#define AU_IN_M (1u << 8)
#define AU_RST_M (1u << 9)
int tg_audio_start(void) {
  uint32_t i;
  if (au_on) return 0;
  for (i = 0; i < 2u * AU_HW; i++) { au_tx[i] = 0; au_rx[i] = 0; }
  fm1_irq_off(); fm1_irq_mask(FM1_IRQ_ALNK0);
#if TG_AUDIO_STOCK_PINS
  FM1_PR(FM1_PC, FM1_OUT) |= (1u << 0) | (1u << 1) | (1u << 5); FM1_PR(FM1_PC, FM1_DIR) &= ~((1u << 0) | (1u << 1) | (1u << 5));   /* stock: output(32,1) (33,1) (37,1) */
  FM1_PR(FM1_PA, FM1_OUT) &= ~AU_RST_M; FM1_PR(FM1_PA, FM1_DIR) &= ~AU_RST_M;                                                       /* stock: output(9,0) */
#endif
  fm1_delay_ms(10);
  AL_CON0 = 0; AL_CON1 = 0; AL_CON2 = 0; AL_CON3 = 0;
  AU_IOMAP5 = (AU_IOMAP5 & ~0xC0u) | 0x40u;                       /* port A group */
  FM1_PR(FM1_PA, FM1_OUT) |= AU_OUT_M;
  FM1_PR(FM1_PA, FM1_DIE) |= AU_OUT_M | AU_IN_M;
  FM1_PR(FM1_PA, FM1_DIR) = (FM1_PR(FM1_PA, FM1_DIR) & ~AU_OUT_M) | AU_IN_M;
  FM1_PR(FM1_PA, FM1_PD) &= ~AU_IN_M; FM1_PR(FM1_PA, FM1_PU) |= AU_IN_M;
  AU_CLK_CON2 = (AU_CLK_CON2 & ~0xF00u) | (au_clk << 8);         /* [11:8]: 0 = 11.2896 MHz MCLK on Galaxia/FM-1; stock Tank-G writes 1 */
  AL_CON3 = (uint8_t)au_con3;
  AL_CON0 = (uint16_t)au_con0;
  AL_LEN = (uint16_t)au_len;
  AL_ADR0 = (uint32_t)(uintptr_t)au_tx; AL_ADR1 = (uint32_t)(uintptr_t)au_rx;
  AL_CON1 = (uint16_t)au_con1;                                    /* lane 0 on 24 bit tx; lane 1 on 24 bit rx */
  AL_CON2 = 0x0Fu;
  fm1_irq_attach(FM1_IRQ_ALNK0, isr_alnk0, 3);
  AL_CON0 |= 0x800u;
  fm1_delay_ms(10);
#if TG_AUDIO_PA9_HIGH
  FM1_PR(FM1_PA, FM1_OUT) |= AU_RST_M;                            /* Galaxia polarity: release the converter */
#endif
  au_on = 1; fm1_irq_on();
  return 0;
}
void tg_audio_stop(void) {
  if (!au_on) return;
  fm1_irq_off(); fm1_irq_mask(FM1_IRQ_ALNK0);
  AL_CON0 &= ~0x800u; AL_CON2 = 0x0Fu;
  au_on = 0; fm1_irq_on();
}
/* o[0] DSP time of the last block (us), o[1] worst since the previous call (us), o[2] the block period (us) at 48 kHz */
void tg_audio_load(uint32_t o[3]) { o[0] = au_last_ticks / FM1_TICKS_PER_US; o[1] = au_max_ticks / FM1_TICKS_PER_US; au_max_ticks = 0; o[2] = (au_len / 2u) * 1000000u / 48000u; }
void tg_audio_stat(uint32_t o[6]) { o[0] = au_on; o[1] = au_irqs; o[2] = au_missed; o[3] = AL_CON0; o[4] = *(volatile uint32_t *)0x119A0u; o[5] = *(volatile uint32_t *)0x119A4u; }
/* Bring-up helper: drive one pin as a push-pull output (port 0=A..7=H, bit 0..15). Refuses PA0..PA3 (panel shift register). */
int tg_pin_out(uint32_t port, uint32_t bit, uint32_t v) {
  if (port > 7u || bit > 15u || (port == 0u && bit < 4u)) return -1;
  if (v) FM1_PR(port, FM1_OUT) |= 1u << bit; else FM1_PR(port, FM1_OUT) &= ~(1u << bit);
  FM1_PR(port, FM1_DIR) &= ~(1u << bit);
  return 0;
}
