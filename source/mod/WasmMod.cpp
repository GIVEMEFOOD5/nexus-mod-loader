/* WasmMod.cpp
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

#include "WasmMod.h"

#include <wasmtime.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using namespace std;

namespace {
	// Limits on what a single call into a mod may do, to stop log or message spam.
	constexpr int MAX_LOGS_PER_CALL = 32;
	constexpr int MAX_MESSAGES_PER_CALL = 4;
	constexpr int32_t MAX_STRING = 64 * 1024;

	// Keep the cause and drop Wasmtime's backtrace, which means nothing to players.
	string Simplify(string text)
	{
		size_t cause = text.rfind("Caused by:");
		if(cause != string::npos)
			text = text.substr(cause + 10);
		size_t start = text.find_first_not_of(" \t\n");
		size_t end = text.find_last_not_of(" \t\n\0");
		text = (start == string::npos) ? string() : text.substr(start, end - start + 1);
		if(text.rfind("wasm trap: ", 0) == 0)
			text = text.substr(11);
		return text;
	}

	string ErrorText(wasmtime_error_t *error)
	{
		wasm_byte_vec_t message;
		wasmtime_error_message(error, &message);
		string text(message.data, message.size);
		wasm_byte_vec_delete(&message);
		wasmtime_error_delete(error);
		return Simplify(text);
	}

	string TrapText(wasm_trap_t *trap)
	{
		string text;
		wasmtime_trap_code_t code;
		if(wasmtime_trap_code(trap, &code) && code == WASMTIME_TRAP_CODE_OUT_OF_FUEL)
			text = "ran past its time budget (out of fuel)";
		else
		{
			wasm_byte_vec_t message;
			wasm_trap_message(trap, &message);
			// The message is NUL-terminated; drop the terminator.
			text = "crashed: " + Simplify(string(message.data, message.size ? message.size - 1 : 0));
			wasm_byte_vec_delete(&message);
		}
		wasm_trap_delete(trap);
		return text;
	}

	wasm_trap_t *Trap(const string &text)
	{
		return wasmtime_trap_new(text.data(), text.size());
	}

	wasm_functype_t *FuncType(initializer_list<wasm_valkind_t> params, initializer_list<wasm_valkind_t> results)
	{
		wasm_valtype_vec_t p;
		wasm_valtype_vec_t r;
		wasm_valtype_vec_new_uninitialized(&p, params.size());
		wasm_valtype_vec_new_uninitialized(&r, results.size());
		size_t i = 0;
		for(wasm_valkind_t kind : params)
			p.data[i++] = wasm_valtype_new(kind);
		i = 0;
		for(wasm_valkind_t kind : results)
			r.data[i++] = wasm_valtype_new(kind);
		return wasm_functype_new(&p, &r);
	}
}



struct WasmMod::Impl {
	wasmtime_module_t *module = nullptr;
	wasmtime_store_t *store = nullptr;
	wasmtime_context_t *context = nullptr;
	wasmtime_instance_t instance{};
	bool instantiated = false;

	int logsThisCall = 0;
	int messagesThisCall = 0;
	uint64_t frameFuel = WasmMod::FRAME_FUEL;
};



// Host functions imported by mods from the "es" module.
struct WasmModCallbacks {
	// Read a string out of the calling mod's linear memory.
	static bool ReadString(wasmtime_caller_t *caller, int32_t ptr, int32_t len, string &out)
	{
		if(ptr < 0 || len < 0 || len > MAX_STRING)
			return false;
		wasmtime_extern_t item;
		if(!wasmtime_caller_export_get(caller, "memory", 6, &item) || item.kind != WASMTIME_EXTERN_MEMORY)
			return false;
		wasmtime_context_t *context = wasmtime_caller_context(caller);
		size_t size = wasmtime_memory_data_size(context, &item.of.memory);
		if(static_cast<size_t>(ptr) + static_cast<size_t>(len) > size)
			return false;
		const uint8_t *data = wasmtime_memory_data(context, &item.of.memory);
		out.assign(reinterpret_cast<const char *>(data + ptr), len);
		return true;
	}

	static bool WriteBytes(wasmtime_caller_t *caller, int32_t ptr, const string &bytes)
	{
		wasmtime_extern_t item;
		if(ptr < 0 || !wasmtime_caller_export_get(caller, "memory", 6, &item) || item.kind != WASMTIME_EXTERN_MEMORY)
			return false;
		wasmtime_context_t *context = wasmtime_caller_context(caller);
		size_t size = wasmtime_memory_data_size(context, &item.of.memory);
		if(static_cast<size_t>(ptr) + bytes.size() > size)
			return false;
		copy(bytes.begin(), bytes.end(), wasmtime_memory_data(context, &item.of.memory) + ptr);
		return true;
	}

	// es.log(level: i32, ptr: i32, len: i32)
	static wasm_trap_t *Log(void *env, wasmtime_caller_t *caller, const wasmtime_val_t *args, size_t,
		wasmtime_val_t *, size_t)
	{
		WasmMod &mod = *static_cast<WasmMod *>(env);
		string text;
		if(!ReadString(caller, args[1].of.i32, args[2].of.i32, text))
			return Trap("es.log: string out of bounds");
		if(++mod.impl->logsThisCall <= MAX_LOGS_PER_CALL)
			mod.host.Log(mod.Name(), clamp(args[0].of.i32, 0, 2), text);
		return nullptr;
	}

	// es.message(ptr: i32, len: i32)
	static wasm_trap_t *Message(void *env, wasmtime_caller_t *caller, const wasmtime_val_t *args, size_t,
		wasmtime_val_t *, size_t)
	{
		WasmMod &mod = *static_cast<WasmMod *>(env);
		string text;
		if(!ReadString(caller, args[0].of.i32, args[1].of.i32, text))
			return Trap("es.message: string out of bounds");
		if(++mod.impl->messagesThisCall <= MAX_MESSAGES_PER_CALL)
			mod.host.Message(mod.Name(), text);
		return nullptr;
	}

	// es.has_permission(ptr: i32, len: i32) -> i32
	static wasm_trap_t *HasPermission(void *env, wasmtime_caller_t *caller, const wasmtime_val_t *args, size_t,
		wasmtime_val_t *results, size_t)
	{
		WasmMod &mod = *static_cast<WasmMod *>(env);
		string name;
		if(!ReadString(caller, args[0].of.i32, args[1].of.i32, name))
			return Trap("es.has_permission: string out of bounds");
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = mod.Manifest().permissions.contains(name);
		return nullptr;
	}

	// es.condition_get(ptr: i32, len: i32) -> i64
	static wasm_trap_t *ConditionGet(void *env, wasmtime_caller_t *caller, const wasmtime_val_t *args, size_t,
		wasmtime_val_t *results, size_t)
	{
		WasmMod &mod = *static_cast<WasmMod *>(env);
		string name;
		if(!ReadString(caller, args[0].of.i32, args[1].of.i32, name))
			return Trap("es.condition_get: string out of bounds");
		results[0].kind = WASMTIME_I64;
		results[0].of.i64 = mod.host.GetCondition(name);
		return nullptr;
	}

	// es.condition_set(ptr: i32, len: i32, value: i64) -> i32 (1 = ok, 0 = failed, -1 = no permission)
	static wasm_trap_t *ConditionSet(void *env, wasmtime_caller_t *caller, const wasmtime_val_t *args, size_t,
		wasmtime_val_t *results, size_t)
	{
		WasmMod &mod = *static_cast<WasmMod *>(env);
		string name;
		if(!ReadString(caller, args[0].of.i32, args[1].of.i32, name))
			return Trap("es.condition_set: string out of bounds");
		results[0].kind = WASMTIME_I32;
		if(!mod.Manifest().permissions.contains("conditions-write"))
		{
			if(++mod.impl->logsThisCall <= MAX_LOGS_PER_CALL)
				mod.host.Log(mod.Name(), 1, "condition_set(\"" + name
					+ "\") refused: the manifest does not request the \"conditions-write\" permission.");
			results[0].of.i32 = -1;
		}
		else
			results[0].of.i32 = mod.host.SetCondition(name, args[2].of.i64) ? 1 : 0;
		return nullptr;
	}

	// es.query(key_ptr: i32, key_len: i32, out_ptr: i32, out_cap: i32) -> i32
	// Returns the value's length (writing it if it fits in out_cap), or -1 for an unknown key.
	static wasm_trap_t *Query(void *env, wasmtime_caller_t *caller, const wasmtime_val_t *args, size_t,
		wasmtime_val_t *results, size_t)
	{
		WasmMod &mod = *static_cast<WasmMod *>(env);
		string key;
		if(!ReadString(caller, args[0].of.i32, args[1].of.i32, key))
			return Trap("es.query: string out of bounds");
		results[0].kind = WASMTIME_I32;
		optional<string> value = mod.host.Query(key);
		if(!value)
		{
			results[0].of.i32 = -1;
			return nullptr;
		}
		int32_t length = static_cast<int32_t>(min<size_t>(value->size(), MAX_STRING));
		value->resize(length);
		if(length <= args[3].of.i32 && !WriteBytes(caller, args[2].of.i32, *value))
			return Trap("es.query: output buffer out of bounds");
		results[0].of.i32 = length;
		return nullptr;
	}
};



unique_ptr<WasmMod> WasmMod::Load(const ModManifest &manifest, wasm_engine_t *engine, ModHost &host, string &error)
{
	filesystem::path file = manifest.folder / manifest.entry;
	ifstream in(file, ios::binary);
	if(!in)
	{
		error = "could not open " + file.string();
		return nullptr;
	}
	vector<uint8_t> bytes((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());

	unique_ptr<WasmMod> mod(new WasmMod(manifest, host));
	Impl &impl = *mod->impl;
	if(manifest.fuelPerFrame > 0)
		impl.frameFuel = min<uint64_t>(manifest.fuelPerFrame, MAX_FRAME_FUEL);

	if(wasmtime_error_t *err = wasmtime_module_new(engine, bytes.data(), bytes.size(), &impl.module))
	{
		error = "not a valid WebAssembly module: " + ErrorText(err);
		return nullptr;
	}

	impl.store = wasmtime_store_new(engine, nullptr, nullptr);
	impl.context = wasmtime_store_context(impl.store);
	// One instance, one memory, a capped memory size.
	wasmtime_store_limiter(impl.store, MEMORY_LIMIT, 100'000, 1, 1, 1);

	wasmtime_linker_t *linker = wasmtime_linker_new(engine);
	struct HostFunc {
		const char *name;
		wasmtime_func_callback_t callback;
		wasm_functype_t *type;
	};
	const wasm_valkind_t I32 = WASM_I32;
	const wasm_valkind_t I64 = WASM_I64;
	HostFunc functions[] = {
		{"log", &WasmModCallbacks::Log, FuncType({I32, I32, I32}, {})},
		{"message", &WasmModCallbacks::Message, FuncType({I32, I32}, {})},
		{"has_permission", &WasmModCallbacks::HasPermission, FuncType({I32, I32}, {I32})},
		{"condition_get", &WasmModCallbacks::ConditionGet, FuncType({I32, I32}, {I64})},
		{"condition_set", &WasmModCallbacks::ConditionSet, FuncType({I32, I32, I64}, {I32})},
		{"query", &WasmModCallbacks::Query, FuncType({I32, I32, I32, I32}, {I32})},
	};
	for(HostFunc &f : functions)
	{
		if(wasmtime_error_t *err = wasmtime_linker_define_func(linker, "es", 2, f.name, strlen(f.name),
				f.type, f.callback, mod.get(), nullptr))
			error = ErrorText(err);
		wasm_functype_delete(f.type);
	}
	if(!error.empty())
	{
		wasmtime_linker_delete(linker);
		return nullptr;
	}

	// Instantiation runs the module's start function, if any, so it needs fuel too.
	wasmtime_context_set_fuel(impl.context, INIT_FUEL);
	wasm_trap_t *trap = nullptr;
	wasmtime_error_t *err = wasmtime_linker_instantiate(linker, impl.context, impl.module, &impl.instance, &trap);
	wasmtime_linker_delete(linker);
	if(err)
	{
		// The usual cause is importing something the sandbox doesn't provide, e.g. WASI.
		error = "could not start: " + ErrorText(err);
		return nullptr;
	}
	if(trap)
	{
		error = "crashed while starting: " + TrapText(trap);
		return nullptr;
	}
	impl.instantiated = true;

	// Check the API version the module was built against.
	if(!mod->HasExport("es_mod_api_version"))
	{
		error = "does not export es_mod_api_version (was it built with the Endless Sky mod SDK?)";
		return nullptr;
	}
	wasmtime_val_t version;
	if(!mod->Call("es_mod_api_version", nullptr, 0, &version, INIT_FUEL))
	{
		error = mod->Fault();
		return nullptr;
	}
	if(version.of.i32 != MOD_API_VERSION || manifest.apiVersion != MOD_API_VERSION)
	{
		error = "built for mod API version " + to_string(version.of.i32) + " (manifest says "
			+ to_string(manifest.apiVersion) + "), but this game provides version " + to_string(MOD_API_VERSION);
		return nullptr;
	}
	return mod;
}



WasmMod::WasmMod(const ModManifest &manifest, ModHost &host)
	: impl(make_unique<Impl>()), manifest(manifest), host(host)
{
}



WasmMod::~WasmMod()
{
	if(impl->store)
		wasmtime_store_delete(impl->store);
	if(impl->module)
		wasmtime_module_delete(impl->module);
}



bool WasmMod::Init()
{
	if(!HasExport("es_mod_init"))
		return !IsFaulted();
	wasmtime_val_t result;
	if(!Call("es_mod_init", nullptr, 0, &result, INIT_FUEL))
		return false;
	if(result.of.i32 != 0)
	{
		SetFault("es_mod_init returned error code " + to_string(result.of.i32));
		return false;
	}
	return true;
}



void WasmMod::DataLoaded()
{
	if(HasExport("es_on_data_loaded"))
		Call("es_on_data_loaded", nullptr, 0, nullptr, INIT_FUEL);
}



void WasmMod::Frame(int64_t frame)
{
	if(!HasExport("es_on_frame"))
		return;
	wasmtime_val_t arg;
	arg.kind = WASMTIME_I64;
	arg.of.i64 = frame;
	Call("es_on_frame", &arg, 1, nullptr, impl->frameFuel);
}



void WasmMod::Event(ModEvent kind, const string &payload)
{
	if(!HasExport("es_on_event"))
		return;
	int32_t ptr = WriteScratch(payload);
	if(ptr < 0)
		return;
	wasmtime_val_t args[3];
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = static_cast<int32_t>(kind);
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = ptr;
	args[2].kind = WASMTIME_I32;
	args[2].of.i32 = static_cast<int32_t>(payload.size());
	Call("es_on_event", args, 3, nullptr, EVENT_FUEL);
}



void WasmMod::Shutdown()
{
	if(HasExport("es_mod_shutdown"))
		Call("es_mod_shutdown", nullptr, 0, nullptr, EVENT_FUEL);
}



const ModManifest &WasmMod::Manifest() const
{
	return manifest;
}



const string &WasmMod::Name() const
{
	return manifest.name;
}



bool WasmMod::IsFaulted() const
{
	return !fault.empty();
}



const string &WasmMod::Fault() const
{
	return fault;
}



const WasmMod::Stats &WasmMod::GetStats() const
{
	return stats;
}



bool WasmMod::HasExport(const char *name)
{
	if(!impl->instantiated || IsFaulted())
		return false;
	wasmtime_extern_t item;
	if(!wasmtime_instance_export_get(impl->context, &impl->instance, name, strlen(name), &item))
		return false;
	bool isFunc = (item.kind == WASMTIME_EXTERN_FUNC);
	wasmtime_extern_delete(&item);
	return isFunc;
}



bool WasmMod::Call(const char *name, const void *args, size_t nargs, void *result, uint64_t fuel)
{
	if(!impl->instantiated || IsFaulted())
		return false;
	wasmtime_extern_t item;
	if(!wasmtime_instance_export_get(impl->context, &impl->instance, name, strlen(name), &item)
			|| item.kind != WASMTIME_EXTERN_FUNC)
		return false;

	// Check the export's signature, so a mismatch is a clear error rather than a crash.
	wasm_functype_t *type = wasmtime_func_type(impl->context, &item.of.func);
	size_t expectedResults = result ? 1 : 0;
	bool signatureOk = wasm_functype_params(type)->size == nargs
		&& wasm_functype_results(type)->size <= 1
		&& (!result || wasm_functype_results(type)->size == 1);
	size_t actualResults = wasm_functype_results(type)->size;
	wasm_functype_delete(type);
	if(!signatureOk)
	{
		SetFault(string("export ") + name + " has the wrong signature");
		return false;
	}

	impl->logsThisCall = 0;
	impl->messagesThisCall = 0;
	wasmtime_context_set_fuel(impl->context, fuel);

	wasmtime_val_t discard;
	wasmtime_val_t *out = result ? static_cast<wasmtime_val_t *>(result) : &discard;
	wasm_trap_t *trap = nullptr;
	auto start = chrono::steady_clock::now();
	wasmtime_error_t *err = wasmtime_func_call(impl->context, &item.of.func,
		static_cast<const wasmtime_val_t *>(args), nargs, out, max(expectedResults, actualResults), &trap);
	double ms = chrono::duration<double, milli>(chrono::steady_clock::now() - start).count();

	++stats.calls;
	stats.lastMs = ms;
	stats.peakMs = max(stats.peakMs, ms);
	stats.totalMs += ms;

	if(err)
	{
		SetFault(string(name) + " failed: " + ErrorText(err));
		return false;
	}
	if(trap)
	{
		SetFault(string(name) + " " + TrapText(trap));
		return false;
	}
	return true;
}



int32_t WasmMod::WriteScratch(const string &payload)
{
	if(!HasExport("es_scratch"))
		return -1;
	wasmtime_val_t arg;
	arg.kind = WASMTIME_I32;
	arg.of.i32 = static_cast<int32_t>(payload.size());
	wasmtime_val_t ptr;
	if(!Call("es_scratch", &arg, 1, &ptr, EVENT_FUEL) || ptr.of.i32 < 0)
		return -1;

	wasmtime_extern_t item;
	if(!wasmtime_instance_export_get(impl->context, &impl->instance, "memory", 6, &item)
			|| item.kind != WASMTIME_EXTERN_MEMORY)
		return -1;
	size_t size = wasmtime_memory_data_size(impl->context, &item.of.memory);
	if(static_cast<size_t>(ptr.of.i32) + payload.size() > size)
	{
		SetFault("es_scratch returned a buffer outside its memory");
		return -1;
	}
	copy(payload.begin(), payload.end(), wasmtime_memory_data(impl->context, &item.of.memory) + ptr.of.i32);
	return ptr.of.i32;
}



void WasmMod::SetFault(const string &reason)
{
	if(fault.empty())
		fault = reason;
}
