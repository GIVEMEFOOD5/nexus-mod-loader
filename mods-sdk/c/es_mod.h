/* es_mod.h -- Endless Sky code mod SDK for C and C++, mod API version 1.

Build a mod with clang (no C library needed):

    clang --target=wasm32 -O2 -nostdlib -fno-builtin -Wl,--no-entry -I path/to/sdk/c -o mod.wasm mod.c

In exactly one source file, write ES_MOD_SETUP(); once at file scope.

Then export any of these hooks (all optional):

    ES_EXPORT(es_mod_init)       int  es_mod_init(void);          // return 0 for success
    ES_EXPORT(es_on_data_loaded) void es_on_data_loaded(void);    // game data and pilot loaded
    ES_EXPORT(es_on_frame)       void es_on_frame(long long frame); // each step in flight (60/s)
    ES_EXPORT(es_on_event)       void es_on_event(int kind, const char *text, int len);
    ES_EXPORT(es_mod_shutdown)   void es_mod_shutdown(void);

Sandbox rules: no files, network or clock. Each call has an instruction budget
(about 2 million per frame by default); a mod that runs past it, or crashes, is
stopped for the rest of the session and the player is told why.

This file is free software under the GNU GPL v3 or later, like Endless Sky.
*/

#ifndef ES_MOD_H
#define ES_MOD_H

#ifdef __cplusplus
extern "C" {
#endif

#define ES_MOD_API_VERSION 1

#define ES_EXPORT(name) __attribute__((export_name(#name)))
#define ES_IMPORT(name) __attribute__((import_module("es"), import_name(#name)))

/* Event kinds for es_on_event. Ignore kinds you don't know. */
enum {
	ES_EVENT_ENTER_SYSTEM = 1, /* text: system name */
	ES_EVENT_LAND = 2,         /* text: planet name (also on loading a save) */
	ES_EVENT_TAKE_OFF = 3,     /* text: planet name */
	ES_EVENT_DAY_PASSED = 4,   /* text: the new date */
};

enum { ES_INFO = 0, ES_WARNING = 1, ES_ERROR = 2 };

/* ---- Functions the game provides ("es" import module) ---- */

/* Write to the game's log (errors.txt and the mod manager console). */
ES_IMPORT(log) void es_log_n(int level, const char *text, int len);
/* Show a line in the in-game message log. At most 4 per call. */
ES_IMPORT(message) void es_message_n(const char *text, int len);
/* 1 if the manifest grants this permission, e.g. "conditions-write". */
ES_IMPORT(has_permission) int es_has_permission_n(const char *name, int len);
/* Read a condition (player conditions, "credits", "net worth"...). Unknown names give 0. */
ES_IMPORT(condition_get) long long es_condition_get_n(const char *name, int len);
/* Write a condition. Needs the "conditions-write" permission.
   Returns 1 on success, 0 if no pilot is loaded, -1 if the permission is missing. */
ES_IMPORT(condition_set) int es_condition_set_n(const char *name, int len, long long value);
/* Read a value: "game.date", "game.version", "api.version", "player.name", "player.system",
   "player.planet", "player.credits", "flagship.name", "flagship.model".
   Returns the value's length (and writes it to out if it fits in cap), or -1 for an unknown key. */
ES_IMPORT(query) int es_query_n(const char *key, int key_len, char *out, int cap);

/* ---- Helpers for NUL-terminated strings ---- */

static inline int es_strlen(const char *s)
{
	int n = 0;
	while(s[n])
		++n;
	return n;
}

static inline void es_log(int level, const char *text) { es_log_n(level, text, es_strlen(text)); }
static inline void es_info(const char *text) { es_log_n(ES_INFO, text, es_strlen(text)); }
static inline void es_message(const char *text) { es_message_n(text, es_strlen(text)); }
static inline int es_has_permission(const char *name) { return es_has_permission_n(name, es_strlen(name)); }
static inline long long es_condition_get(const char *name) { return es_condition_get_n(name, es_strlen(name)); }
static inline int es_condition_set(const char *name, long long value)
{
	return es_condition_set_n(name, es_strlen(name), value);
}

/* Query into a buffer and NUL-terminate it. Returns the length, or -1 if unknown or too long. */
static inline int es_query(const char *key, char *out, int cap)
{
	int len = es_query_n(key, es_strlen(key), out, cap - 1);
	if(len < 0 || len > cap - 1)
	{
		if(cap > 0)
			out[0] = '\0';
		return -1;
	}
	out[len] = '\0';
	return len;
}

/* A tiny string builder, since there's no printf in the sandbox. */
typedef struct {
	char *data;
	int len;
	int cap;
} es_str;

static inline es_str es_str_init(char *buffer, int cap)
{
	es_str s = {buffer, 0, cap};
	if(cap > 0)
		buffer[0] = '\0';
	return s;
}

static inline void es_str_add(es_str *s, const char *text)
{
	while(*text && s->len < s->cap - 1)
		s->data[s->len++] = *text++;
	s->data[s->len] = '\0';
}

static inline void es_str_add_n(es_str *s, const char *text, int n)
{
	for(int i = 0; i < n && s->len < s->cap - 1; ++i)
		s->data[s->len++] = text[i];
	s->data[s->len] = '\0';
}

static inline void es_str_add_int(es_str *s, long long value)
{
	char digits[24];
	int n = 0;
	unsigned long long v = value < 0 ? 0ull - (unsigned long long)value : (unsigned long long)value;
	do
		digits[n++] = (char)('0' + v % 10);
	while((v /= 10) && n < 23);
	if(value < 0)
		es_str_add(s, "-");
	while(n--)
		es_str_add_n(s, &digits[n], 1);
}

#ifdef __cplusplus
#define ES_EXTERN_C extern "C"
#else
#define ES_EXTERN_C
#endif

/* There is no C library in the sandbox, but compilers turn some loops and struct
   copies into calls to strlen, memcpy, memset, memmove or memcmp (clang 21 does
   this even for es_strlen above). ES_MOD_SETUP() provides small versions of them.
   no_builtin stops the compiler turning these bodies back into calls to themselves. */
#define ES_MOD_LIBC_SHIMS() \
	ES_EXTERN_C __attribute__((no_builtin)) __SIZE_TYPE__ strlen(const char *s) \
	{ \
		const char *p = s; \
		while(*p) \
			++p; \
		return (__SIZE_TYPE__)(p - s); \
	} \
	ES_EXTERN_C __attribute__((no_builtin)) void *memcpy(void *to, const void *from, __SIZE_TYPE__ n) \
	{ \
		unsigned char *d = (unsigned char *)to; \
		const unsigned char *s = (const unsigned char *)from; \
		while(n--) \
			*d++ = *s++; \
		return to; \
	} \
	ES_EXTERN_C __attribute__((no_builtin)) void *memmove(void *to, const void *from, __SIZE_TYPE__ n) \
	{ \
		unsigned char *d = (unsigned char *)to; \
		const unsigned char *s = (const unsigned char *)from; \
		if(d < s) \
			while(n--) \
				*d++ = *s++; \
		else \
			while(n--) \
				d[n] = s[n]; \
		return to; \
	} \
	ES_EXTERN_C __attribute__((no_builtin)) void *memset(void *to, int value, __SIZE_TYPE__ n) \
	{ \
		unsigned char *d = (unsigned char *)to; \
		while(n--) \
			*d++ = (unsigned char)value; \
		return to; \
	} \
	ES_EXTERN_C __attribute__((no_builtin)) int memcmp(const void *a, const void *b, __SIZE_TYPE__ n) \
	{ \
		const unsigned char *x = (const unsigned char *)a; \
		const unsigned char *y = (const unsigned char *)b; \
		for(; n; --n, ++x, ++y) \
			if(*x != *y) \
				return *x - *y; \
		return 0; \
	}

/* Required boilerplate: the API version export, the scratch buffer the game uses
   to pass event text to the mod, and the C library shims. Use once per mod, at
   file scope, in exactly one source file. */
#define ES_MOD_SCRATCH_SIZE 65536
#define ES_MOD_SETUP() \
	ES_MOD_LIBC_SHIMS() \
	ES_EXTERN_C ES_EXPORT(es_mod_api_version) int es_mod_api_version(void) { return ES_MOD_API_VERSION; } \
	static char es_mod_scratch_buffer[ES_MOD_SCRATCH_SIZE]; \
	ES_EXTERN_C ES_EXPORT(es_scratch) char *es_scratch(int size) \
	{ \
		return size <= ES_MOD_SCRATCH_SIZE ? es_mod_scratch_buffer : (char *)-1; \
	}

#ifdef __cplusplus
}
#endif

#endif
