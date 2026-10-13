#include "es_mod.h"
ES_MOD_SETUP();
volatile int x;
ES_EXPORT(es_on_frame) void es_on_frame(long long frame) { if(frame == 100) for(;;) ++x; }
