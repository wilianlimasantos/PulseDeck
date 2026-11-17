// Minimal libc declarations so firmware-core can be compile-checked for pi32v2 without the SDK's newlib.
// With the AC79 SDK present, build.sh uses the SDK's own headers instead (see tools/build.sh).
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* memcpy(void*, const void*, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
int memcmp(const void*, const void*, size_t);
void* memchr(const void*, int, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
char* strchr(const char*, int);
#ifdef __cplusplus
}
#endif
