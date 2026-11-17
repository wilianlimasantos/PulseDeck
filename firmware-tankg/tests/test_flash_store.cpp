#include <stdio.h>
#include <stdlib.h>
#include "flash_store.h"
struct Sim : tg::FlashDev {
  uint8_t m[tg::FlashStore::kRegionBytes]; int budget = -1;  // budget = bytes programmable before a simulated power cut
  Sim() { memset(m, 0xFF, sizeof m); }
  bool read(uint32_t o, uint8_t* d, uint32_t n) override { memcpy(d, m + o, n); return true; }
  bool erase4k(uint32_t o) override { if (budget == 0) return false; if (budget > 0) budget--; if (budget == 0) { memset(m + o, 0xFF, 2048); return false; } memset(m + o, 0xFF, 4096); return true; }
  bool program(uint32_t o, const uint8_t* s, uint32_t n) override { for (uint32_t i = 0; i < n; i++) { if (budget == 0) return false; if (budget > 0) budget--; m[o + i] &= s[i]; } return true; }
};
static int fails = 0, checks = 0;
#define CHK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
  static Sim sim; tg::FlashStore st(sim, 0);
  uint8_t a[300], b[300], r[4096]; size_t n;
  for (int i = 0; i < 300; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(255 - i); }
  CHK(!st.exists("p00")); CHK(st.write("p00", a, 300)); CHK(st.read("p00", r, sizeof r, n) && n == 300 && !memcmp(r, a, 300));
  CHK(st.write("p00", b, 300)); CHK(st.read("p00", r, sizeof r, n) && !memcmp(r, b, 300));
  CHK(st.write("gset", a, 100)); CHK(st.read("gset", r, sizeof r, n) && n == 100); CHK(!st.write("bad", a, 10)); CHK(!st.write("p32", a, 10));
  CHK(st.erase("p00") && !st.exists("p00"));
  // power cut at every possible point of an overwrite: old OR new value must survive, never garbage
  CHK(st.write("p05", a, 300));
  int old_ok = 0, new_ok = 0;
  for (int cut = 0; cut < 330; cut++) {
    static Sim s2; memcpy(s2.m, sim.m, sizeof s2.m); tg::FlashStore t(s2, 0);
    s2.budget = cut; t.write("p05", b, 300); s2.budget = -1;
    bool g = t.read("p05", r, sizeof r, n) && n == 300;
    if (g && !memcmp(r, a, 300)) old_ok++; else if (g && !memcmp(r, b, 300)) new_ok++; else { CHK(0); printf("cut=%d corrupt\n", cut); }
  }
  CHK(old_ok > 0 && new_ok > 0);
  for (int i = 0; i < 40; i++) { a[0] = (uint8_t)i; CHK(st.write("p07", a, 300)); }
  CHK(st.read("p07", r, sizeof r, n) && r[0] == 39);
  printf("%d checks, %d failed (old=%d new=%d)\n", checks, fails, old_ok, new_ok); return fails != 0;
}
