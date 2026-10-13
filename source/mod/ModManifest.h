/* ModManifest.h
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

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>



// The contents of a mod's "mod.toml" file. Only a small subset of TOML is
// understood: [sections], key = "string" | integer | true/false | ["array"],
// and one-level inline tables (key = { a = "b", c = true }).
class ModManifest {
public:
	struct AIDisclosure {
		bool declared = false;
		std::string tool;
		std::string scope;
		bool humanReviewed = false;
	};


public:
	// Parse the given mod.toml. On failure, IsValid() is false and Errors() explains why.
	static ModManifest Load(const std::filesystem::path &file);

	bool IsValid() const;
	const std::vector<std::string> &Errors() const;

	// The folder that holds the manifest.
	std::filesystem::path folder;

	std::string name;
	std::string version;
	std::vector<std::string> authors;
	std::string description;
	std::string homepage;

	// "wasm", "lua", "external" or "data". This loader only runs "wasm".
	std::string runtime = "wasm";
	// The module file, relative to the mod folder.
	std::string entry = "mod.wasm";

	int apiVersion = 0;
	std::vector<std::string> gameVersions;
	std::vector<std::string> hooks;
	std::set<std::string> permissions;
	std::vector<std::string> dependencies;
	std::vector<std::string> conflicts;

	AIDisclosure aiGenerated;

	// Optional per-call fuel budget override, capped by the engine.
	long long fuelPerFrame = 0;
	std::string checksum;


private:
	std::vector<std::string> errors;
};
