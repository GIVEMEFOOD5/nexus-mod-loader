/* ModHost.h
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

#include <cstdint>
#include <optional>
#include <string>



// The version of the mod API this engine provides. A mod's es_mod_api_version()
// export and its manifest's api_version must both equal this number.
constexpr int MOD_API_VERSION = 1;

// Event kinds passed to a mod's es_on_event(kind, ptr, len) export.
// New kinds may be added within an API version; mods must ignore unknown ones.
enum class ModEvent : int32_t {
	ENTER_SYSTEM = 1,  // payload: system name
	LAND = 2,          // payload: planet name. Also sent when a save is loaded on a planet.
	TAKE_OFF = 3,      // payload: planet name
	DAY_PASSED = 4,    // payload: new date, e.g. "Tue, 17 Nov 3013"
};



// What the game offers to mods. The engine implements this; a test harness can
// implement it too, so mods can be run without the full game.
class ModHost {
public:
	virtual ~ModHost() = default;

	// level: 0 = info, 1 = warning, 2 = error.
	virtual void Log(const std::string &mod, int level, const std::string &text) = 0;
	// Show a line in the in-game message log.
	virtual void Message(const std::string &mod, const std::string &text) = 0;
	// Read a game condition (player conditions, "credits", etc.). Unknown names read as 0.
	virtual int64_t GetCondition(const std::string &name) = 0;
	// Write a game condition. The caller has already checked the mod's permission.
	virtual bool SetCondition(const std::string &name, int64_t value) = 0;
	// Read-only snapshot values, e.g. "game.date", "player.system", "flagship.name".
	virtual std::optional<std::string> Query(const std::string &key) = 0;
};
