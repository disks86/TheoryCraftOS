#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
static inline char *ralloc_strdup(void *c, const char *s){(void)c;return strdup(s);}
static inline char *ralloc_asprintf(void *c, const char *f, ...){(void)c;va_list a;va_start(a,f);char*r=NULL;vasprintf(&r,f,a);va_end(a);return r;}
static inline char *ralloc_strcat(char **dst, const char *s){size_t n=strlen(*dst)+strlen(s)+1;*dst=(char*)realloc(*dst,n);strcat(*dst,s);return *dst;}
static inline bool ralloc_asprintf_append(char **dst,const char*f,...){va_list a;va_start(a,f);char*t=NULL;vasprintf(&t,f,a);va_end(a);size_t n=strlen(*dst)+strlen(t)+1;*dst=(char*)realloc(*dst,n);strcat(*dst,t);free(t);return true;}
static inline void ralloc_free(void*p){free(p);}

static inline void *rzalloc_size(void *c, size_t n){(void)c;return calloc(1,n);}
static inline bool ralloc_vasprintf_rewrite_tail(char **str, size_t *start, const char *fmt, va_list args){
  char *t=NULL; vasprintf(&t,fmt,args); size_t n=strlen(t);
  *str=(char*)realloc(*str,*start+n+1); memcpy(*str+*start,t,n+1); *start+=n; free(t); return true;}
