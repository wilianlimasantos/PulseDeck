#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int atoi(const char*);
long strtol(const char*, char**, int);
unsigned long strtoul(const char*, char**, int);
#ifdef __cplusplus
}
#endif
