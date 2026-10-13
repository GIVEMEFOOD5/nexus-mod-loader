#include "es_mod.h"
ES_MOD_SETUP();
ES_EXPORT(es_mod_init) int es_mod_init(void) {
	for(int i = 0; i < 100000; ++i) if(__builtin_wasm_memory_grow(0, 16) == (unsigned long)-1) { es_info("memory.grow refused, as expected"); return 0; }
	return 0;
}
ES_EXPORT(es_on_frame) void es_on_frame(long long f) { (void)f; }
