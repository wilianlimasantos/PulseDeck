#pragma once
#include <stdarg.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int snprintf(char*, size_t, const char*, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char*, size_t, const char*, va_list);
#ifdef __cplusplus
}
#endif
