/* ModManifest.cpp
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

#include "ModManifest.h"

#include <cctype>
#include <fstream>
#include <sstream>

using namespace std;

namespace {
	struct Value {
		enum class Type { STRING, INTEGER, BOOL, ARRAY, TABLE } type = Type::STRING;
		string text;
		long long integer = 0;
		bool boolean = false;
		vector<string> array;
		map<string, Value> table;
	};

	void SkipSpace(const string &s, size_t &i)
	{
		while(i < s.size() && isspace(static_cast<unsigned char>(s[i])))
			++i;
	}

	// Remove a trailing comment that is not inside a string.
	string StripComment(const string &line)
	{
		bool inString = false;
		for(size_t i = 0; i < line.size(); ++i)
		{
			if(line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
				inString = !inString;
			else if(line[i] == '#' && !inString)
				return line.substr(0, i);
		}
		return line;
	}

	bool ParseString(const string &s, size_t &i, string &out)
	{
		if(i >= s.size() || s[i] != '"')
			return false;
		++i;
		out.clear();
		while(i < s.size() && s[i] != '"')
		{
			if(s[i] == '\\' && i + 1 < s.size())
			{
				char c = s[++i];
				out += (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
			}
			else
				out += s[i];
			++i;
		}
		if(i >= s.size())
			return false;
		++i;
		return true;
	}

	bool ParseKey(const string &s, size_t &i, string &out)
	{
		SkipSpace(s, i);
		if(i < s.size() && s[i] == '"')
			return ParseString(s, i, out);
		size_t start = i;
		while(i < s.size() && (isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-' || s[i] == '.'))
			++i;
		out = s.substr(start, i - start);
		return !out.empty();
	}

	bool ParseValue(const string &s, size_t &i, Value &out);

	bool ParseArray(const string &s, size_t &i, Value &out)
	{
		out.type = Value::Type::ARRAY;
		++i;
		while(true)
		{
			SkipSpace(s, i);
			if(i >= s.size())
				return false;
			if(s[i] == ']')
			{
				++i;
				return true;
			}
			Value item;
			if(!ParseValue(s, i, item))
				return false;
			if(item.type == Value::Type::STRING)
				out.array.push_back(item.text);
			else if(item.type == Value::Type::INTEGER)
				out.array.push_back(to_string(item.integer));
			SkipSpace(s, i);
			if(i < s.size() && s[i] == ',')
				++i;
		}
	}

	bool ParseTable(const string &s, size_t &i, Value &out)
	{
		out.type = Value::Type::TABLE;
		++i;
		while(true)
		{
			SkipSpace(s, i);
			if(i >= s.size())
				return false;
			if(s[i] == '}')
			{
				++i;
				return true;
			}
			string key;
			if(!ParseKey(s, i, key))
				return false;
			SkipSpace(s, i);
			if(i >= s.size() || s[i] != '=')
				return false;
			++i;
			Value item;
			if(!ParseValue(s, i, item))
				return false;
			out.table[key] = item;
			SkipSpace(s, i);
			if(i < s.size() && s[i] == ',')
				++i;
		}
	}

	bool ParseValue(const string &s, size_t &i, Value &out)
	{
		SkipSpace(s, i);
		if(i >= s.size())
			return false;
		if(s[i] == '"')
		{
			out.type = Value::Type::STRING;
			return ParseString(s, i, out.text);
		}
		if(s[i] == '[')
			return ParseArray(s, i, out);
		if(s[i] == '{')
			return ParseTable(s, i, out);
		size_t start = i;
		while(i < s.size() && !isspace(static_cast<unsigned char>(s[i])) && s[i] != ',' && s[i] != ']' && s[i] != '}')
			++i;
		string word = s.substr(start, i - start);
		if(word == "true" || word == "yes" || word == "false" || word == "no")
		{
			out.type = Value::Type::BOOL;
			out.boolean = (word == "true" || word == "yes");
			return true;
		}
		string digits;
		for(char c : word)
			if(c != '_')
				digits += c;
		try {
			size_t used = 0;
			out.integer = stoll(digits, &used);
			out.type = Value::Type::INTEGER;
			return used == digits.size();
		}
		catch(...) {
			return false;
		}
	}

	int Depth(const string &s)
	{
		int depth = 0;
		bool inString = false;
		for(size_t i = 0; i < s.size(); ++i)
		{
			if(s[i] == '"' && (i == 0 || s[i - 1] != '\\'))
				inString = !inString;
			else if(!inString && (s[i] == '[' || s[i] == '{'))
				++depth;
			else if(!inString && (s[i] == ']' || s[i] == '}'))
				--depth;
		}
		return depth;
	}

	vector<string> AsList(const Value &v)
	{
		if(v.type == Value::Type::ARRAY)
			return v.array;
		if(v.type == Value::Type::STRING)
			return {v.text};
		return {};
	}

	// Accept both "api_version" and "api-version" (and "api version" in quotes).
	string Normalize(string key)
	{
		for(char &c : key)
			if(c == '-' || c == ' ')
				c = '_';
		return key;
	}
}



ModManifest ModManifest::Load(const filesystem::path &file)
{
	ModManifest result;
	result.folder = file.parent_path();
	result.name = result.folder.filename().string();

	ifstream in(file);
	if(!in)
	{
		result.errors.push_back("Could not open " + file.string());
		return result;
	}

	map<string, Value> values;
	string section;
	string line;
	string pending;
	int lineNumber = 0;
	while(getline(in, line))
	{
		++lineNumber;
		line = StripComment(line);
		pending += line + ' ';
		if(Depth(pending) > 0)
			continue;
		string statement = pending;
		pending.clear();

		size_t i = 0;
		SkipSpace(statement, i);
		if(i >= statement.size())
			continue;
		if(statement[i] == '[')
		{
			size_t end = statement.find(']', i);
			if(end == string::npos)
			{
				result.errors.push_back("Line " + to_string(lineNumber) + ": unterminated section header.");
				continue;
			}
			section = Normalize(statement.substr(i + 1, end - i - 1));
			continue;
		}
		string key;
		if(!ParseKey(statement, i, key))
		{
			result.errors.push_back("Line " + to_string(lineNumber) + ": expected a key.");
			continue;
		}
		SkipSpace(statement, i);
		if(i >= statement.size() || statement[i] != '=')
		{
			result.errors.push_back("Line " + to_string(lineNumber) + ": expected '=' after \"" + key + "\".");
			continue;
		}
		++i;
		Value value;
		if(!ParseValue(statement, i, value))
		{
			result.errors.push_back("Line " + to_string(lineNumber) + ": could not read the value of \"" + key + "\".");
			continue;
		}
		key = Normalize(key);
		values[section.empty() ? key : section + "." + key] = value;
	}

	auto text = [&values](const string &key) -> const Value * {
		auto it = values.find(key);
		return it == values.end() ? nullptr : &it->second;
	};

	// The [mod] section is optional: keys may also sit at the top level.
	auto get = [&text](const string &key) -> const Value * {
		const Value *v = text(key);
		return v ? v : text("mod." + key);
	};

	if(const Value *v = get("name"))
		result.name = v->text;
	if(const Value *v = get("version"))
		result.version = v->text;
	if(const Value *v = get("authors"))
		result.authors = AsList(*v);
	if(const Value *v = get("description"))
		result.description = v->text;
	if(const Value *v = get("homepage"))
		result.homepage = v->text;
	if(const Value *v = get("runtime"))
		result.runtime = v->text;
	if(const Value *v = get("entry"))
		result.entry = v->text;
	if(const Value *v = get("api_version"))
		result.apiVersion = static_cast<int>(v->integer);
	if(const Value *v = get("game_versions"))
		result.gameVersions = AsList(*v);
	if(const Value *v = get("hooks"))
		result.hooks = AsList(*v);
	if(const Value *v = get("permissions"))
		for(const string &p : AsList(*v))
			result.permissions.insert(p);
	if(const Value *v = get("dependencies"))
		result.dependencies = AsList(*v);
	if(const Value *v = get("conflicts"))
		result.conflicts = AsList(*v);
	if(const Value *v = get("fuel_per_frame"))
		result.fuelPerFrame = v->integer;
	if(const Value *v = get("checksum"))
		result.checksum = v->text;

	// AI disclosure, as an inline table or an [ai_generated] section.
	map<string, Value> ai;
	if(const Value *v = get("ai_generated"); v && v->type == Value::Type::TABLE)
		ai = v->table;
	for(const auto &[key, value] : values)
		if(key.rfind("ai_generated.", 0) == 0)
			ai[key.substr(13)] = value;
	if(!ai.empty())
	{
		result.aiGenerated.declared = true;
		for(const auto &[rawKey, value] : ai)
		{
			string key = Normalize(rawKey);
			if(key == "tool")
				result.aiGenerated.tool = value.text;
			else if(key == "scope")
			{
				vector<string> parts = AsList(value);
				for(size_t n = 0; n < parts.size(); ++n)
					result.aiGenerated.scope += (n ? ", " : "") + parts[n];
			}
			else if(key == "human_reviewed")
				result.aiGenerated.humanReviewed = value.boolean;
		}
	}

	if(result.name.empty())
		result.errors.push_back("The manifest has no name.");
	if(result.runtime == "wasm" && result.apiVersion <= 0)
		result.errors.push_back("A wasm mod must set api_version.");
	return result;
}



bool ModManifest::IsValid() const
{
	return errors.empty();
}



const vector<string> &ModManifest::Errors() const
{
	return errors;
}
