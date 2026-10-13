#include "es_mod.h"
ES_MOD_SETUP();
static long long frames = 0;
ES_EXPORT(es_on_event) void es_on_event(int kind, const char *text, int len) {
	char b[200]; es_str s = es_str_init(b, sizeof b);
	es_str_add(&s, "event "); es_str_add_int(&s, kind); es_str_add(&s, ": "); es_str_add_n(&s, text, len);
	es_str_add(&s, " (frame "); es_str_add_int(&s, frames); es_str_add(&s, ")");
	es_info(b);
}
ES_EXPORT(es_on_frame) void es_on_frame(long long f) { frames = f; }
ES_EXPORT(es_mod_shutdown) void es_mod_shutdown(void) {
	char b[64]; es_str s = es_str_init(b, sizeof b); es_str_add(&s, "frames seen: "); es_str_add_int(&s, frames); es_info(b);
}
