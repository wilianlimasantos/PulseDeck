// The complete firmware (src/main.cpp: tg_main loop, Engine, panel, media, log store, DSP) as a host process: serial lines on stdin/stdout,
// flash 0x75000..0x7B000 in a file (TG_FLASH_FILE) so a restart is a power cycle. Used by PulseDeck's end-to-end test
// (crates/pulsedeck-core/tests/firmware_e2e.rs) to run the editor's real upload/sync code against the real firmware logic. No hardware involved.
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint8_t g_flash[0x7B000 - 0x75000]; static const char* g_file;
static void flashSave() { if (!g_file) return; FILE* f = fopen(g_file, "wb"); if (f) { fwrite(g_flash, 1, sizeof g_flash, f); fclose(f); } }
extern "C" {
void tg_audio_load(uint32_t* o) { o[0] = 0; o[1] = 0; o[2] = 2666; }
int tg_audio_set(const char*, uint32_t) { return 0; } void tg_audio_cfg(uint32_t* o) { for (int i = 0; i < 5; i++) o[i] = 0; }
int tg_pin_out(uint32_t, uint32_t, uint32_t) { return 0; } int tg_audio_start(void) { return 0; } void tg_audio_stop(void) {}
void tg_audio_stat(uint32_t* o) { for (int i = 0; i < 6; i++) o[i] = 0; }
uint32_t tg_millis(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u); }
void tg_knobs_scan(uint32_t, uint16_t* o) { for (int i = 0; i < 16; i++) o[i] = 0xFFFF; o[0] = 1023; }   // only MASTER reads (full); the other knobs are "not connected"
int tg_ws_send(const uint8_t*, uint32_t, uint32_t) { return 0; } void tg_ports_scan(int, uint32_t* o) { for (int i = 0; i < 8; i++) o[i] = 0; }
uint32_t tg_fs_read(uint32_t* a, uint32_t* h) { if (a) *a = 0; if (h) *h = 0; return 0; }
void tg_enter_uboot(void) {} uint32_t tg_pa_out(void) { return 0; } void tg_pa_set(uint32_t) {} void tg_pa_nopull(uint32_t) {} void tg_pa_dirout(uint32_t) {}
void tg_delay_us(uint32_t) {} void tg_healthy(void) {}
int tg_fl_read(uint32_t o, uint8_t* d, uint32_t n) { if (o < 0x75000 || o + n > 0x7B000) abort(); memcpy(d, g_flash + (o - 0x75000), n); return 1; }
int tg_fl_erase(uint32_t o) { if (o < 0x75000 || o + 4096 > 0x7B000 || (o & 4095)) abort(); memset(g_flash + (o - 0x75000), 0xFF, 4096); flashSave(); return 1; }
int tg_fl_prog(uint32_t o, const uint8_t* s, uint32_t n) { if (o < 0x75000 || o + n > 0x7B000) abort(); for (uint32_t i = 0; i < n; i++) g_flash[o - 0x75000 + i] &= s[i]; flashSave(); return 1; }   // any write outside the store region aborts the simulator
void tg_usb_init(void) {} void tg_usb_service(void) {}
int tg_usb_getc(void) {
  static unsigned char buf[4096]; static ssize_t n = 0, at = 0;
  if (at >= n) { struct pollfd p = {0, POLLIN, 0}; if (poll(&p, 1, 1) <= 0) return -1; n = read(0, buf, sizeof buf); at = 0; if (n <= 0) exit(0); }
  return buf[at++];
}
void tg_usb_putc(char c) { if (c == '\r') return; putchar(c); if (c == '\n') fflush(stdout); }
}
#define TG_HOST_TEST 1
#define TG_HOST_SIM 1
#define TG_FLASH_STORE 1
#define TG_STORE_OFF 0x75000u
#include <new>
#include "../src/main.cpp"
int main() {
  memset(g_flash, 0xFF, sizeof g_flash); g_file = getenv("TG_FLASH_FILE");
  if (g_file) { FILE* f = fopen(g_file, "rb"); if (f) { size_t k = fread(g_flash, 1, sizeof g_flash, f); (void)k; fclose(f); } }
  tg_main(); return 0;
}
