#include "es_mod.h"
ES_MOD_SETUP();
ES_EXPORT(es_on_event) void es_on_event(int k, const char *t, int l) { (void)t; (void)l; if(k == ES_EVENT_LAND) __builtin_trap(); }
