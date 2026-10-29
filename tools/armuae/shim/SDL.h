/* SDL2 stand-in for building Amiberry's CPU core alone (armcpu): only the
 * libc-like helpers the core headers call. Anything else that reaches here
 * is a sign a non-CPU module was pulled in. */
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#define SDL_snprintf snprintf
#define SDL_vsnprintf vsnprintf
#define SDL_strlen strlen
#define SDL_strcmp strcmp
#define SDL_strncmp strncmp
#define SDL_strcasecmp strcasecmp
#define SDL_strncasecmp strncasecmp
#define SDL_strchr strchr
#define SDL_strrchr strrchr
#define SDL_strstr strstr
#define SDL_strdup strdup
#define SDL_strtol strtol
#define SDL_strtoul strtoul
#define SDL_strtoll strtoll
#define SDL_strtod strtod
#define SDL_atoi atoi
#define SDL_atof atof
#define SDL_toupper toupper
#define SDL_tolower tolower
#define SDL_isspace isspace
#define SDL_isdigit isdigit
#define SDL_free free
#define SDL_malloc malloc
#define SDL_calloc calloc
#define SDL_realloc realloc
#define SDL_memcpy memcpy
#define SDL_memset memset
#define SDL_memcmp memcmp
#define SDL_strlcpy(d, s, n) (snprintf((d), (n), "%s", (s)))
#define SDL_strlcat(d, s, n) (strncat((d), (s), (n) - strlen(d) - 1), strlen(d))
#define SDL_Log(...) fprintf(stderr, __VA_ARGS__)
/* thread types, opaque: the CPU core runs on one thread */
typedef struct SDL_Semaphore SDL_Semaphore;
typedef struct SDL_Mutex SDL_Mutex;
typedef struct SDL_Condition SDL_Condition;
typedef struct SDL_Thread SDL_Thread;
typedef int (*SDL_ThreadFunction)(void *data);
typedef uint32_t SDL_SpinLock;
void SDL_SignalSemaphore(SDL_Semaphore *);
void SDL_WaitSemaphore(SDL_Semaphore *);
bool SDL_TryWaitSemaphore(SDL_Semaphore *);
bool SDL_WaitSemaphoreTimeout(SDL_Semaphore *, int32_t);
uint32_t SDL_GetSemaphoreValue(SDL_Semaphore *);
uint64_t SDL_GetTicks(void);
uint64_t SDL_GetPerformanceCounter(void);
uint64_t SDL_GetPerformanceFrequency(void);
void SDL_Delay(uint32_t);
void SDL_Quit(void);
/* SDL2 spellings */
typedef struct SDL_semaphore SDL_sem;
typedef struct SDL_mutex SDL_mutex;
typedef unsigned long SDL_threadID;
int SDL_SemPost(SDL_sem *);
int SDL_SemWait(SDL_sem *);
int SDL_SemTryWait(SDL_sem *);
int SDL_SemWaitTimeout(SDL_sem *, uint32_t);
uint32_t SDL_SemValue(SDL_sem *);
SDL_sem *SDL_CreateSemaphore(uint32_t);
void SDL_DestroySemaphore(SDL_sem *);
SDL_threadID SDL_ThreadID(void);
SDL_threadID SDL_GetThreadID(SDL_Thread *);
int SDL_LockMutex(SDL_mutex *);
int SDL_UnlockMutex(SDL_mutex *);
typedef uint8_t Uint8; typedef uint16_t Uint16; typedef uint32_t Uint32; typedef uint64_t Uint64;
typedef int8_t Sint8; typedef int16_t Sint16; typedef int32_t Sint32; typedef int64_t Sint64;
