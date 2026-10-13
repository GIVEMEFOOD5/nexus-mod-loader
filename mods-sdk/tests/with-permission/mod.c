#include "es_mod.h"
ES_MOD_SETUP();
ES_EXPORT(es_on_data_loaded) void es_on_data_loaded(void) {
	int r = es_condition_set("mod: test", 5);
	es_info(r == 1 && es_condition_get("mod: test") == 5 ? "condition_set worked" : "UNEXPECTED: condition_set failed");
}
