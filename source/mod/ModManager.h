/* ModManager.h
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

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

class PlayerInfo;



// Finds code mods in "<config>/mods/", loads the enabled ones in load order and
// forwards game hooks to them. This is the only class the rest of the engine
// talks to; all calls happen on the main thread.
//
// Mods are enabled and ordered by "<config>/mods/load order.txt": one mod folder
// name per line, top first. Lines starting with '#' are comments. If the file
// doesn't exist, every mod found is enabled, in alphabetical order.
class ModManager {
public:
	enum class Status { RUNNING, DISABLED, NOT_LOADED, FAULTED, DATA_ONLY };

	struct Info {
		ModManifest manifest;
		Status status;
		// Why the mod isn't running, if it isn't.
		std::string reason;
		// Warnings to show the player (AI-made, permissions, unsupported runtime...).
		std::vector<std::string> warnings;
		double peakFrameMs = 0.;
		double averageFrameMs = 0.;
	};


public:
	// Discover and start mods. Call once, after Files::Init(). If devMode is set,
	// mods whose .wasm file changes on disk are reloaded while the game runs.
	static void Init(bool enabled, bool devMode);
	// Called once the game data and the most recent pilot have loaded.
	static void DataLoaded(PlayerInfo &player);
	// Called once per game step while in flight, with the calculation thread paused.
	static void Frame(PlayerInfo &player);
	// Called when something happens in the game world. Safe to call from any
	// thread: off the main thread, the event is delivered at the next frame.
	static void Event(PlayerInfo &player, ModEvent kind, const std::string &payload);
	// Stop all mods. Call before the game exits.
	static void Shutdown();

	// A snapshot of every mod that was found, for UIs and the --mods listing.
	static std::vector<Info> List();
	static void Print(std::ostream &out);

	static std::filesystem::path ModsFolder();
};
