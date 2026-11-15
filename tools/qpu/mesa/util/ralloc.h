#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
static inline char *ralloc_strdup(const void *ctx, const char *s){(void)ctx;return strdup(s);}
static inline bool ralloc_asprintf_append(char **s, const char *fmt, ...){va_list a;va_start(a,fmt);char *t;vasprintf(&t,fmt,a);va_end(a);size_t n=*s?strlen(*s):0;*s=realloc(*s,n+strlen(t)+1);strcpy(*s+n,t);free(t);return true;}
static inline char *ralloc_asprintf(const void *ctx, const char *fmt, ...){(void)ctx;va_list a;va_start(a,fmt);char *t;vasprintf(&t,fmt,a);va_end(a);return t;}
static inline void ralloc_free(void *p){free(p);}
static inline void *rzalloc_size(const void *c,size_t n){(void)c;return calloc(1,n);}
static inline bool ralloc_vasprintf_rewrite_tail(char **s, size_t *start, const char *fmt, va_list a)
{char *t; vasprintf(&t,fmt,a); size_t n=*start; *s=realloc(*s,n+strlen(t)+1); strcpy(*s+n,t); *start=n+strlen(t); free(t); return true;}
