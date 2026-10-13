#include "es_mod.h"
ES_MOD_SETUP();
__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write"))) int fd_write(int, int, int, int);
ES_EXPORT(es_mod_init) int es_mod_init(void) { return fd_write(1, 0, 0, 0); }
