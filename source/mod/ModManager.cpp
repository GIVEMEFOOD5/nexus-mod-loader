/* ModManager.cpp
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

#include "ModManager.h"

#include "WasmMod.h"

#include "../Files.h"
#include "../GameData.h"
#include "../GameVersion.h"
#include "../Logger.h"
#include "../Messages.h"
#include "../Planet.h"
#include "../PlayerInfo.h"
#include "../Ship.h"
#include "../System.h"

#include <wasmtime.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

using namespace std;

namespace {
	// Average frame time above which a mod gets a performance warning.
	constexpr double SLOW_FRAME_MS = 1.;
	// In dev mode, how often to look for changed mod files.
	constexpr int64_t RELOAD_CHECK_FRAMES = 60;

	// The game side of the mod API.
	class GameHost : public ModHost {
	public:
		PlayerInfo *player = nullptr;

		void Log(const string &mod, int level, const string &text) override
		{
			Logger::Log("[mod " + mod + "] " + text, level == 0 ? Logger::Level::INFO
				: level == 1 ? Logger::Level::WARNING : Logger::Level::ERROR);
		}

		void Message(const string &mod, const string &text) override
		{
			if(GameData::IsLoaded())
				Messages::Add({"[" + mod + "] " + text, GameData::MessageCategories().Get("normal")});
			// Keep a copy in the log, so tools and the mod manager console can see it.
			Log(mod, 0, "message: " + text);
		}

		int64_t GetCondition(const string &name) override
		{
			if(player && player->IsLoaded())
				return player->Conditions().Get(name);
			return GameData::GlobalConditions().Get(name);
		}

		bool SetCondition(const string &name, int64_t value) override
		{
			if(!player || !player->IsLoaded() || name.empty())
				return false;
			player->Conditions().Set(name, value);
			return true;
		}

		optional<string> Query(const string &key) override
		{
			if(key == "api.version")
				return to_string(MOD_API_VERSION);
			if(key == "game.version")
				return GameVersion::Running().ToString();
			if(!player || !player->IsLoaded())
				return key.starts_with("player.") || key.starts_with("flagship.") || key == "game.date"
					? optional<string>("") : nullopt;
			if(key == "game.date")
				return player->GetDate().ToString();
			if(key == "player.name")
				return player->FirstName() + " " + player->LastName();
			if(key == "player.system")
				return player->GetSystem() ? player->GetSystem()->TrueName() : "";
			if(key == "player.planet")
				return player->GetPlanet() ? player->GetPlanet()->TrueName() : "";
			if(key == "player.credits")
				return to_string(player->Accounts().Credits());
			const Ship *flagship = player->Flagship();
			if(key == "flagship.name")
				return flagship ? flagship->GivenName() : "";
			if(key == "flagship.model")
				return flagship ? flagship->DisplayModelName() : "";
			return nullopt;
		}
	};


	struct Entry {
		ModManager::Info info;
		unique_ptr<WasmMod> mod;
		filesystem::file_time_type loadedTime;
	};


	GameHost host;
	wasm_engine_t *engine = nullptr;
	vector<Entry> entries;
	bool devMode = false;
	thread::id mainThread;
	// Events raised off the main thread wait here until the next frame.
	mutex pendingMutex;
	vector<pair<ModEvent, string>> pendingEvents;
	bool dataLoaded = false;
	int64_t frame = 0;


	filesystem::file_time_type ModifiedTime(const filesystem::path &file)
	{
		error_code ec;
		auto time = filesystem::last_write_time(file, ec);
		return ec ? filesystem::file_time_type{} : time;
	}


	vector<string> Warnings(const ModManifest &manifest)
	{
		vector<string> warnings;
		const auto &ai = manifest.aiGenerated;
		if(ai.declared)
			warnings.push_back("AI-made (author declares): " + (ai.scope.empty() ? string("unspecified") : ai.scope)
				+ (ai.tool.empty() ? "" : ", tool: " + ai.tool)
				+ (ai.humanReviewed ? ", human reviewed" : ", NOT human reviewed"));
		if(!manifest.permissions.empty())
		{
			string list;
			for(const string &p : manifest.permissions)
				list += (list.empty() ? "" : ", ") + p;
			warnings.push_back("Requests permissions: " + list);
		}
		if(manifest.checksum.empty())
			warnings.push_back("Unverified: no checksum or signature.");
		return warnings;
	}


	// Load and start one mod. Updates the entry's status.
	void Start(Entry &entry)
	{
		ModManager::Info &info = entry.info;
		string error;
		entry.mod = WasmMod::Load(info.manifest, engine, host, error);
		entry.loadedTime = ModifiedTime(info.manifest.folder / info.manifest.entry);
		if(!entry.mod)
		{
			info.status = ModManager::Status::NOT_LOADED;
			info.reason = error;
			Logger::Log("Mod \"" + info.manifest.name + "\" was not loaded: " + error, Logger::Level::WARNING);
			return;
		}
		if(!entry.mod->Init())
		{
			info.status = ModManager::Status::FAULTED;
			info.reason = entry.mod->Fault();
			Logger::Log("Mod \"" + info.manifest.name + "\" failed to start: " + info.reason, Logger::Level::WARNING);
			entry.mod.reset();
			return;
		}
		info.status = ModManager::Status::RUNNING;
		info.reason.clear();
		Logger::Log("Loaded mod \"" + info.manifest.name + "\" " + info.manifest.version, Logger::Level::INFO);
		for(const string &warning : info.warnings)
			Logger::Log("  " + warning, Logger::Level::INFO);
	}


	// After any call, notice mods that faulted and stop them.
	void CheckFaults()
	{
		for(Entry &entry : entries)
			if(entry.mod && entry.mod->IsFaulted())
			{
				entry.info.status = ModManager::Status::FAULTED;
				entry.info.reason = entry.mod->Fault();
				string text = "Mod \"" + entry.info.manifest.name + "\" was stopped: " + entry.info.reason;
				Logger::Log(text, Logger::Level::WARNING);
				if(GameData::IsLoaded())
					Messages::Add({text, GameData::MessageCategories().Get("high")});
				entry.mod.reset();
			}
	}


	void UpdateStats(Entry &entry)
	{
		if(!entry.mod)
			return;
		const WasmMod::Stats &stats = entry.mod->GetStats();
		entry.info.peakFrameMs = stats.peakMs;
		entry.info.averageFrameMs = stats.calls ? stats.totalMs / stats.calls : 0.;
	}


	void ReloadChanged(PlayerInfo &player)
	{
		for(Entry &entry : entries)
		{
			if(entry.info.status == ModManager::Status::DISABLED || entry.info.status == ModManager::Status::DATA_ONLY)
				continue;
			filesystem::path manifestFile = entry.info.manifest.folder / "mod.toml";
			auto time = ModifiedTime(entry.info.manifest.folder / entry.info.manifest.entry);
			if(time == filesystem::file_time_type{} || time == entry.loadedTime)
				continue;

			Logger::Log("Reloading mod \"" + entry.info.manifest.name + "\"", Logger::Level::INFO);
			if(entry.mod)
				entry.mod->Shutdown();
			entry.mod.reset();
			ModManifest manifest = ModManifest::Load(manifestFile);
			if(manifest.IsValid())
				entry.info.manifest = manifest;
			entry.info.warnings = Warnings(entry.info.manifest);
			host.player = &player;
			Start(entry);
			if(entry.mod && dataLoaded)
				entry.mod->DataLoaded();
			CheckFaults();
			Messages::Add({"Reloaded mod \"" + entry.info.manifest.name + "\": "
				+ (entry.mod ? string("running") : entry.info.reason), GameData::MessageCategories().Get("normal")});
		}
	}
}



void ModManager::Init(bool enabled, bool isDevMode)
{
	devMode = isDevMode;
	mainThread = this_thread::get_id();
	filesystem::path folder = ModsFolder();
	error_code ec;
	if(!filesystem::is_directory(folder, ec))
	{
		filesystem::create_directories(folder, ec);
		return;
	}

	// Find every folder with a manifest.
	map<string, ModManifest> found;
	for(const auto &dir : filesystem::directory_iterator(folder, ec))
	{
		filesystem::path manifestFile = dir.path() / "mod.toml";
		if(!dir.is_directory() || !filesystem::exists(manifestFile))
			continue;
		found.emplace(dir.path().filename().string(), ModManifest::Load(manifestFile));
	}

	// Work out the load order.
	vector<string> order;
	ifstream orderFile(folder / "load order.txt");
	if(orderFile)
	{
		string line;
		while(getline(orderFile, line))
		{
			while(!line.empty() && isspace(static_cast<unsigned char>(line.back())))
				line.pop_back();
			if(line.empty() || line[0] == '#')
				continue;
			if(found.contains(line))
				order.push_back(line);
			else
				Logger::Log("load order.txt lists \"" + line + "\", which is not in the mods folder.",
					Logger::Level::WARNING);
		}
	}
	else
		for(const auto &it : found)
			order.push_back(it.first);

	// Anything not in the order is found but disabled.
	set<string> enabledSet(order.begin(), order.end());
	for(const auto &it : found)
		if(!enabledSet.contains(it.first))
			order.push_back(it.first);

	wasm_config_t *config = wasm_config_new();
	wasmtime_config_consume_fuel_set(config, true);
	engine = wasm_engine_new_with_config(config);

	set<string> running;
	for(const string &folderName : order)
	{
		Entry &entry = entries.emplace_back();
		Info &info = entry.info;
		info.manifest = found[folderName];
		info.warnings = Warnings(info.manifest);
		const ModManifest &m = info.manifest;

		if(!enabled || !enabledSet.contains(folderName))
		{
			info.status = Status::DISABLED;
			info.reason = enabled ? "not in load order.txt" : "mods are turned off (--no-mods)";
			continue;
		}
		info.status = Status::NOT_LOADED;
		if(!m.IsValid())
		{
			for(const string &error : m.Errors())
				info.reason += (info.reason.empty() ? "" : "; ") + error;
			Logger::Log("Mod in \"" + folderName + "\" has a bad mod.toml: " + info.reason, Logger::Level::WARNING);
			continue;
		}
		if(m.runtime == "data")
		{
			info.status = Status::DATA_ONLY;
			info.reason = "data-only mods are loaded as plugins";
			continue;
		}
		if(m.runtime != "wasm")
		{
			info.reason = "the \"" + m.runtime + "\" runtime is not supported by this build";
			if(m.runtime == "external")
				info.warnings.push_back("Not sandboxed: runs as a normal program with full access to your computer.");
			Logger::Log("Mod \"" + m.name + "\": " + info.reason, Logger::Level::WARNING);
			continue;
		}
		auto missing = find_if(m.dependencies.begin(), m.dependencies.end(),
			[&running](const string &dep) { return !running.contains(dep); });
		if(missing != m.dependencies.end())
		{
			info.reason = "needs \"" + *missing + "\" to be running and earlier in the load order";
			Logger::Log("Mod \"" + m.name + "\" " + info.reason, Logger::Level::WARNING);
			continue;
		}
		auto conflict = find_if(m.conflicts.begin(), m.conflicts.end(),
			[&running](const string &other) { return running.contains(other); });
		if(conflict != m.conflicts.end())
		{
			info.reason = "conflicts with \"" + *conflict + "\"";
			Logger::Log("Mod \"" + m.name + "\" " + info.reason, Logger::Level::WARNING);
			continue;
		}

		Start(entry);
		if(entry.mod)
			running.insert(m.name);
	}
}



void ModManager::DataLoaded(PlayerInfo &player)
{
	dataLoaded = true;
	host.player = &player;
	int count = 0;
	bool anyAI = false;
	for(Entry &entry : entries)
		if(entry.mod)
		{
			entry.mod->DataLoaded();
			++count;
			anyAI |= entry.info.manifest.aiGenerated.declared;
		}
	CheckFaults();
	if(count)
		Messages::Add({to_string(count) + (count == 1 ? " code mod is" : " code mods are") + " running"
			+ (anyAI ? " (includes AI-made code, see errors.txt / the mod manager)." : "."),
			GameData::MessageCategories().Get("normal")});
}



void ModManager::Frame(PlayerInfo &player)
{
	if(entries.empty())
		return;
	++frame;
	host.player = &player;
	vector<pair<ModEvent, string>> events;
	{
		lock_guard<mutex> lock(pendingMutex);
		events.swap(pendingEvents);
	}
	for(const auto &[kind, payload] : events)
		Event(player, kind, payload);
	if(devMode && frame % RELOAD_CHECK_FRAMES == 0)
		ReloadChanged(player);
	for(Entry &entry : entries)
		if(entry.mod)
		{
			entry.mod->Frame(frame);
			UpdateStats(entry);
			if(entry.info.averageFrameMs > SLOW_FRAME_MS && entry.mod->GetStats().calls == 600)
			{
				string text = "Mod \"" + entry.info.manifest.name + "\" is slow: "
					+ to_string(entry.info.averageFrameMs) + " ms per frame on average.";
				entry.info.warnings.push_back(text);
				Logger::Log(text, Logger::Level::WARNING);
			}
		}
	CheckFaults();
}



void ModManager::Event(PlayerInfo &player, ModEvent kind, const string &payload)
{
	if(entries.empty())
		return;
	if(this_thread::get_id() != mainThread)
	{
		lock_guard<mutex> lock(pendingMutex);
		pendingEvents.emplace_back(kind, payload);
		return;
	}
	host.player = &player;
	for(Entry &entry : entries)
		if(entry.mod)
			entry.mod->Event(kind, payload);
	CheckFaults();
}



void ModManager::Shutdown()
{
	for(Entry &entry : entries)
		if(entry.mod)
			entry.mod->Shutdown();
	entries.clear();
	if(engine)
		wasm_engine_delete(engine);
	engine = nullptr;
	host.player = nullptr;
}



vector<ModManager::Info> ModManager::List()
{
	vector<Info> result;
	for(Entry &entry : entries)
	{
		UpdateStats(entry);
		result.push_back(entry.info);
	}
	return result;
}



void ModManager::Print(ostream &out)
{
	static const char *STATUS[] = {"running", "disabled", "not loaded", "faulted", "data only"};
	out << "Mods folder: " << ModsFolder().string() << '\n';
	out << "Mod API version: " << MOD_API_VERSION << '\n';
	if(entries.empty())
		out << "No mods found.\n";
	for(const Info &info : List())
	{
		out << '\n' << info.manifest.name;
		if(!info.manifest.version.empty())
			out << ' ' << info.manifest.version;
		out << "  [" << STATUS[static_cast<int>(info.status)] << "]\n";
		if(!info.manifest.description.empty())
			out << "    " << info.manifest.description << '\n';
		if(!info.reason.empty())
			out << "    reason: " << info.reason << '\n';
		for(const string &warning : info.warnings)
			out << "    warning: " << warning << '\n';
	}
	out.flush();
}



filesystem::path ModManager::ModsFolder()
{
	return Files::Config() / "mods";
}
