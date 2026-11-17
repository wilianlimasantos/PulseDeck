/* Minimal libc for firmware-core (string + tiny printf subset: %d %ld %u %x %s %c %% with 0-pad width). */
#include <stddef.h>
#include <stdarg.h>
size_t strlen(const char* s) { const char* p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char* a, const char* b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char* strchr(const char* s, int c) { for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return 0; } }
void* memchr(const void* s, int c, size_t n) { const unsigned char* p = s; while (n--) { if (*p == (unsigned char)c) return (void*)p; p++; } return 0; }
void* memmove(void* d, const void* s, size_t n) { unsigned char* a = d; const unsigned char* b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
unsigned long strtoul(const char* s, char** e, int base) {
  unsigned long v = 0; if (base == 0) base = 10;
  if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  for (;; s++) { int d; char c = *s; if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else break; if (d >= base) break; v = v * (unsigned)base + (unsigned)d; }
  if (e) *e = (char*)s; return v; }
long strtol(const char* s, char** e, int base) { int neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; long v = (long)strtoul(s, e, base); return neg ? -v : v; }
int atoi(const char* s) { return (int)strtol(s, 0, 10); }
int vsnprintf(char* o, size_t cap, const char* f, va_list ap) {
  size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) o[n] = (ch); n++; } while (0)
  for (; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++; int zero = 0, w = 0, lng = 0;
    if (*f == '0') { zero = 1; f++; }
    while (*f >= '0' && *f <= '9') w = w * 10 + (*f++ - '0');
    if (*f == 'l') { lng = 1; f++; }
    char t[12]; int tl = 0; const char* s = 0;
    if (*f == 'd' || *f == 'i') { long v = lng ? va_arg(ap, long) : va_arg(ap, int); unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v; if (v < 0) PUT('-'); do { t[tl++] = (char)('0' + u % 10); u /= 10; } while (u); }
    else if (*f == 'u' || *f == 'x') { unsigned long u = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); unsigned b = *f == 'u' ? 10 : 16; do { t[tl++] = "0123456789abcdef"[u % b]; u /= b; } while (u); }
    else if (*f == 's') { s = va_arg(ap, const char*); if (!s) s = "(null)"; }
    else if (*f == 'c') { PUT((char)va_arg(ap, int)); continue; }
    else { PUT(*f); continue; }
    if (s) { while (*s) { PUT(*s); s++; } continue; }
    for (int i = tl; i < w; i++) PUT(zero ? '0' : ' ');
    while (tl) PUT(t[--tl]);
  }
  if (cap) o[n < cap ? n : cap - 1] = 0;
  return (int)n;
}
int snprintf(char* o, size_t cap, const char* f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(o, cap, f, ap); va_end(ap); return r; }
