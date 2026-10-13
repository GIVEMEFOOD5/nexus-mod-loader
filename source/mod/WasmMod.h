/* WasmMod.h
Copyright (c) 2026 by the Nexus Mod Platform contributors

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "ModHost.h"
#include "ModManifest.h"

#include <cstdint>
#include <memory>
#include <string>

struct wasm_engine_t;



// One loaded WebAssembly mod, running in its own sandboxed store.
// Mods get no file, network or system access: the only functions they can
// import are the "es" module functions documented in sdk/c/es_mod.h.
// Every call into a mod runs with a fuel (instruction) budget and the store has
// a memory cap, so a runaway mod is stopped and disabled instead of freezing the game.
class WasmMod {
public:
	struct Stats {
		int64_t calls = 0;
		double lastMs = 0.;
		double peakMs = 0.;
		double totalMs = 0.;
	};

	// Budgets, in Wasmtime fuel units (roughly one per instruction).
	static constexpr uint64_t INIT_FUEL = 500'000'000;
	static constexpr uint64_t FRAME_FUEL = 2'000'000;
	static constexpr uint64_t MAX_FRAME_FUEL = 20'000'000;
	static constexpr uint64_t EVENT_FUEL = 10'000'000;
	static constexpr int64_t MEMORY_LIMIT = 64 * 1024 * 1024;


public:
	// Compile and instantiate the mod's module. Returns null and fills in error on failure.
	static std::unique_ptr<WasmMod> Load(const ModManifest &manifest, wasm_engine_t *engine,
		ModHost &host, std::string &error);
	~WasmMod();

	WasmMod(const WasmMod &) = delete;
	WasmMod &operator=(const WasmMod &) = delete;

	// Lifecycle hooks. Each is a no-op if the mod doesn't export it, or if it has faulted.
	bool Init();
	void DataLoaded();
	void Frame(int64_t frame);
	void Event(ModEvent kind, const std::string &payload);
	void Shutdown();

	const ModManifest &Manifest() const;
	const std::string &Name() const;
	bool IsFaulted() const;
	const std::string &Fault() const;
	const Stats &GetStats() const;


private:
	WasmMod(const ModManifest &manifest, ModHost &host);

	// Call an export by name. Returns false if the call trapped (the mod is then faulted).
	bool Call(const char *name, const void *args, size_t nargs, void *result, uint64_t fuel);
	bool HasExport(const char *name);
	void SetFault(const std::string &reason);

	// Copy a payload into the mod's scratch buffer. Returns the pointer, or -1.
	int32_t WriteScratch(const std::string &payload);


private:
	struct Impl;
	std::unique_ptr<Impl> impl;

	ModManifest manifest;
	ModHost &host;
	std::string fault;
	Stats stats;

	friend struct WasmModCallbacks;
};
