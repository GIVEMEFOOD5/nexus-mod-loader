/* hello-c: a minimal Endless Sky code mod. Read-only; changes nothing in the game. */
#include "es_mod.h"

ES_MOD_SETUP();

static long long systemsSeen = 0;

ES_EXPORT(es_mod_init) int es_mod_init(void)
{
	es_info("hello-c starting up");
	return 0;
}

ES_EXPORT(es_on_data_loaded) void es_on_data_loaded(void)
{
	char name[128];
	char buffer[256];
	es_str s = es_str_init(buffer, sizeof(buffer));
	if(es_query("player.name", name, sizeof(name)) > 1)
	{
		es_str_add(&s, "Hello, captain ");
		es_str_add(&s, name);
		es_str_add(&s, "!");
	}
	else
		es_str_add(&s, "Hello! No pilot loaded yet.");
	es_message(buffer);
}

ES_EXPORT(es_on_event) void es_on_event(int kind, const char *text, int len)
{
	if(kind != ES_EVENT_ENTER_SYSTEM)
		return;
	++systemsSeen;
	char buffer[256];
	es_str s = es_str_init(buffer, sizeof(buffer));
	es_str_add(&s, "Welcome to ");
	es_str_add_n(&s, text, len);
	es_str_add(&s, ". Systems entered this session: ");
	es_str_add_int(&s, systemsSeen);
	es_message(buffer);
}

ES_EXPORT(es_on_frame) void es_on_frame(long long frame)
{
	/* The game runs at 60 steps per second. */
	if(frame % 3600 != 0)
		return;
	char buffer[128];
	es_str s = es_str_init(buffer, sizeof(buffer));
	es_str_add(&s, "Credits: ");
	es_str_add_int(&s, es_condition_get("credits"));
	es_message(buffer);
}
