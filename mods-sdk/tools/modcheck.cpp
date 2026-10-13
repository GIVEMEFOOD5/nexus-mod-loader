/* modcheck.cpp -- run an Endless Sky code mod outside the game.

Loads a mod folder exactly as the game does (same manifest parser, same sandbox
and budgets), then calls its hooks against a fake game state and reports what
happened. Useful while writing a mod, and in CI.

    modcheck <mod folder> [frames]

Exit code 0 if the mod ran without faulting, 1 otherwise.

Copyright (c) 2026 by the Nexus Mod Platform contributors. GPL v3 or later.
*/

#include "../../source/mod/ModHost.h"
#include "../../source/mod/ModManifest.h"
#include "../../source/mod/WasmMod.h"

#include <wasmtime.h>

#include <iostream>
#include <map>

using namespace std;

namespace {
	class FakeHost : public ModHost {
	public:
		map<string, int64_t> conditions{{"credits", 131000}};
		int messages = 0;

		void Log(const string &mod, int level, const string &text) override
		{
			static const char *LEVEL[] = {"info", "warning", "error"};
			cout << "  [" << mod << " " << LEVEL[level] << "] " << text << '\n';
		}
		void Message(const string &mod, const string &text) override
		{
			++messages;
			cout << "  [" << mod << " message] " << text << '\n';
		}
		int64_t GetCondition(const string &name) override
		{
			auto it = conditions.find(name);
			return it == conditions.end() ? 0 : it->second;
		}
		bool SetCondition(const string &name, int64_t value) override
		{
			cout << "  (condition \"" << name << "\" set to " << value << ")\n";
			conditions[name] = value;
			return true;
		}
		optional<string> Query(const string &key) override
		{
			static const map<string, string> VALUES = {
				{"api.version", to_string(MOD_API_VERSION)},
				{"game.version", "0.11.4 (modcheck)"},
				{"game.date", "Sat, 16 Nov 3013"},
				{"player.name", "Test Pilot"},
				{"player.system", "Rutilicus"},
				{"player.planet", "New Boston"},
				{"player.credits", "131000"},
				{"flagship.name", "Testbed"},
				{"flagship.model", "Sparrow"},
			};
			auto it = VALUES.find(key);
			return it == VALUES.end() ? nullopt : optional<string>(it->second);
		}
	};
}



int main(int argc, char *argv[])
{
	if(argc < 2)
	{
		cerr << "usage: modcheck <mod folder> [frames]\n";
		return 2;
	}
	int64_t frames = argc > 2 ? stoll(argv[2]) : 3600;

	ModManifest manifest = ModManifest::Load(filesystem::path(argv[1]) / "mod.toml");
	cout << "Mod: " << manifest.name << ' ' << manifest.version << " (runtime " << manifest.runtime
		<< ", api " << manifest.apiVersion << ")\n";
	if(manifest.aiGenerated.declared)
		cout << "AI-made (declared): " << manifest.aiGenerated.scope << ", tool " << manifest.aiGenerated.tool
			<< (manifest.aiGenerated.humanReviewed ? ", human reviewed\n" : ", NOT human reviewed\n");
	for(const string &p : manifest.permissions)
		cout << "Permission requested: " << p << '\n';
	if(!manifest.IsValid())
	{
		for(const string &error : manifest.Errors())
			cout << "Manifest error: " << error << '\n';
		return 1;
	}

	wasm_config_t *config = wasm_config_new();
	wasmtime_config_consume_fuel_set(config, true);
	wasm_engine_t *engine = wasm_engine_new_with_config(config);
	FakeHost host;

	int result = 0;
	{
		string error;
		unique_ptr<WasmMod> mod = WasmMod::Load(manifest, engine, host, error);
		if(!mod)
		{
			cout << "FAILED to load: " << error << '\n';
			wasm_engine_delete(engine);
			return 1;
		}
		cout << "\ninit\n";
		mod->Init();
		cout << "data loaded\n";
		mod->DataLoaded();
		cout << "event: enter system \"Rutilicus\"\n";
		mod->Event(ModEvent::ENTER_SYSTEM, "Rutilicus");
		cout << "event: land \"New Boston\"\n";
		mod->Event(ModEvent::LAND, "New Boston");
		cout << "running " << frames << " frames\n";
		for(int64_t frame = 1; frame <= frames && !mod->IsFaulted(); ++frame)
			mod->Frame(frame);
		mod->Shutdown();

		const WasmMod::Stats &stats = mod->GetStats();
		cout << "\nCalls: " << stats.calls << ", average " << (stats.calls ? stats.totalMs / stats.calls : 0.)
			<< " ms, peak " << stats.peakMs << " ms\n";
		if(mod->IsFaulted())
		{
			cout << "FAULTED: " << mod->Fault() << '\n';
			result = 1;
		}
		else
			cout << "OK\n";
	}
	wasm_engine_delete(engine);
	return result;
}
