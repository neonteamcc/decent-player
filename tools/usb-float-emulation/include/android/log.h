#pragma once
#include <stdarg.h>
#include <stdio.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
static inline int __android_log_print(int, const char *tag, const char *fmt, ...) {
    va_list args;va_start(args,fmt);fprintf(stderr,"%s: ",tag);
    int n=vfprintf(stderr,fmt,args);fputc('\n',stderr);va_end(args);return n;
}
