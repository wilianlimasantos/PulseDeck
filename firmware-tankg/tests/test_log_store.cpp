#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "log_store.h"
struct Sim : tg::FlashDev {
  uint8_t m[tg::LogStore::kRegionBytes]; long budget = -1;  // budget = programmable bytes / erases before a simulated power cut
  Sim() { memset(m, 0xFF, sizeof m); }
  bool read(uint32_t o, uint8_t* d, uint32_t n) override { memcpy(d, m + o, n); return true; }
  bool erase4k(uint32_t o) override { if (budget == 0) return false; if (budget > 0) budget--; memset(m + o, 0xFF, 4096); if (budget == 0) memset(m + o, 0xA5, 2048); return budget != 0; }
  bool program(uint32_t o, const uint8_t* s, uint32_t n) override { for (uint32_t i = 0; i < n; i++) { if (budget == 0) return false; if (budget > 0) budget--; m[o + i] &= s[i]; } return true; }
};
static int fails = 0, checks = 0;
#define CHK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void fill(uint8_t* b, int n, int seed) { for (int i = 0; i < n; i++) b[i] = (uint8_t)(seed * 31 + i); }
static bool is(tg::LogStore& st, const char* k, int n, int seed) { uint8_t r[2048], e[2048]; size_t l; fill(e, n, seed); return st.read(k, r, sizeof r, l) && l == (size_t)n && !memcmp(r, e, n); }
int main() {
  static Sim sim; tg::LogStore st(sim, 0); static uint8_t a[8192]; uint8_t r[2048]; size_t n;
  CHK(!st.exists("p00")); fill(a, 290, 1); CHK(st.write("p00", a, 290)); CHK(is(st, "p00", 290, 1));
  fill(a, 250, 2); CHK(st.write("p00", a, 250)); CHK(is(st, "p00", 250, 2));   // newest wins
  fill(a, 900, 3); CHK(st.write("gset", a, 900)); CHK(is(st, "gset", 900, 3) && is(st, "p00", 250, 2));
  CHK(st.erase("p00") && !st.exists("p00") && !st.read("p00", r, sizeof r, n)); CHK(st.erase("nothing"));
  CHK(!st.write("", a, 4) && !st.write("0123456789abcdef", a, 4) && !st.write("x", a, tg::LogStore::kMaxData + 1));
  fill(a, 3000, 9); CHK(st.write("c00", a, 3000) && st.read("c00", a + 4000, 4000, n) && n == 3000 && !memcmp(a, a + 4000, 3000) && st.erase("c00"));   // a capture-sized record
  { tg::LogStore t(sim, 0); CHK(is(t, "gset", 900, 3) && !t.exists("p00")); }   // survives "reboot" (fresh object on the same flash)
  // many writes -> several compactions, 33 keys live
  for (int round = 0; round < 12; round++) for (int k = 0; k < 33; k++) { char key[8]; snprintf(key, sizeof key, k == 32 ? "gset" : "p%02d", k); fill(a, 230 + k, round * 40 + k); CHK(st.write(key, a, 230 + k)); }
  for (int k = 0; k < 33; k++) { char key[8]; snprintf(key, sizeof key, k == 32 ? "gset" : "p%02d", k); CHK(is(st, key, 230 + k, 11 * 40 + k)); }
  CHK(st.activeBank() <= 1 && st.used() < tg::LogStore::kBank);
  // worst case: 32 presets x 290 B + settings 923 B must fit
  for (int k = 0; k < 33; k++) { char key[8]; snprintf(key, sizeof key, k == 32 ? "gset" : "p%02d", k); fill(a, k == 32 ? 923 : 290, k); CHK(st.write(key, a, k == 32 ? 923 : 290)); }
  CHK(is(st, "gset", 923, 32) && is(st, "p31", 290, 31));
  // POWER CUT at every programmed byte / erase of an overwrite (incl. a compaction): old OR new value, never garbage; others untouched; store still writable
  int olds = 0, news = 0, maxc = 0;
  for (int scenario = 0; scenario < 2; scenario++) {
    static Sim base; memcpy(base.m, sim.m, sizeof base.m);
    if (scenario == 1) { tg::LogStore t(base, 0); while (t.used() + 304 <= tg::LogStore::kBank) { fill(a, 290, 77); if (!t.write("p03", a, 290)) break; } }  // park the tail right before a compaction
    for (int cut = 0; cut < 30000; cut += (cut < 700 ? 1 : 13)) {
      static Sim s2; memcpy(s2.m, base.m, sizeof s2.m); s2.budget = -1;
      tg::LogStore t(s2, 0); fill(a, 290, 200); s2.budget = cut; bool ok = t.write("p05", a, 290); long left = s2.budget; s2.budget = -1;
      if (ok && left != 0) { if (cut > maxc) maxc = cut; break; }   // finished without being cut: all further cuts are the same
      tg::LogStore u(s2, 0);   // reboot
      bool nw = is(u, "p05", 290, 200), od = is(u, "p05", 290, 5);
      CHK(nw || od); if (nw) news++; else olds++;
      CHK(is(u, "p07", 290, 7) && is(u, "gset", 923, 32));
      fill(a, 290, 201); CHK(u.write("p05", a, 290) && is(u, "p05", 290, 201) && is(u, "p07", 290, 7));
    }
  }
  CHK(olds > 0 && news > 0);
  // a torn record in the tail is skipped and the next write recovers
  { static Sim s3; tg::LogStore t(s3, 0); fill(a, 100, 1); t.write("p01", a, 100); s3.budget = 30; fill(a, 100, 2); t.write("p01", a, 100); s3.budget = -1;
    tg::LogStore u(s3, 0); CHK(u.dirty() && is(u, "p01", 100, 1)); fill(a, 100, 3); CHK(u.write("p01", a, 100) && is(u, "p01", 100, 3) && !u.dirty()); }
  printf("%d checks, %d failed (cuts: old=%d new=%d, longest write %d)\n", checks, fails, olds, news, maxc); return fails != 0;
}
