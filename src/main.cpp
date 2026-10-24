#include <mysql.h>

#include "mysql.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mysqlcomponent {

namespace {

constexpr size_t LoggedQuery = 512;
constexpr size_t PushedQuery = 1024;

double milliseconds(Clock::duration duration) {
	return std::chrono::duration<double, std::milli>(duration).count();
}

std::string clip(const std::string& query, size_t length) {
	std::string text = query.substr(0, length);
	for (char& character : text) {
		if (character == '\n' || character == '\r' || character == '\t') {
			character = ' ';
		}
	}
	if (query.size() > length) {
		text.append("...");
	}
	return text;
}

}

State& state() {
	static State instance;
	return instance;
}

Script* scriptOf(AMX* amx) {
	State& s = state();
	if (amx == s.lastAmx && s.lastScript != nullptr) {
		return s.lastScript;
	}
	auto found = s.scripts.find(amx);
	if (found == s.scripts.end()) {
		return nullptr;
	}
	s.lastAmx = amx;
	s.lastScript = &found->second;
	return s.lastScript;
}

bool findPublic(Script& script, const char* name, int& index) {
	if (script.api->FindPublic(name, &index) != AMX_ERR_NONE || index < 0) {
		return false;
	}
	char found[128] = {};
	return script.api->GetPublic(index, found) == AMX_ERR_NONE && std::strcmp(found, name) == 0;
}

Connection* connectionOf(cell handle) {
	State& s = state();
	if (handle < 1 || handle > cell(s.connections.size())) {
		return nullptr;
	}
	return s.connections[size_t(handle) - 1].get();
}

Result* currentResult(AMX* amx) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return nullptr;
	}
	if (!script->active.empty()) {
		return script->active.back().result;
	}
	return script->used != nullptr ? script->used : script->last.get();
}

ResultSet* currentSet(AMX* amx) {
	Result* result = currentResult(amx);
	if (result == nullptr || result->set >= result->sets.size()) {
		return nullptr;
	}
	return &result->sets[result->set];
}

void log(const char* format, ...) {
	State& s = state();
	if (s.core == nullptr) {
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	s.core->vprintLn(format, arguments);
	va_end(arguments);
}

void logError(const char* format, ...) {
	State& s = state();
	if (s.core == nullptr) {
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	s.core->vlogLn(LogLevel::Error, format, arguments);
	va_end(arguments);
}

int openConnection(AMX* amx, Settings settings) {
	State& s = state();
	size_t slot = 0;
	while (slot != s.connections.size() && s.connections[slot]) {
		++slot;
	}
	if (slot == s.connections.size()) {
		s.connections.emplace_back();
		s.owners.emplace_back();
	}
	int id = int(slot) + 1;
	auto connection = std::make_unique<Connection>(id, std::move(settings));
	connection->start();
	auto job = std::make_unique<Job>();
	job->kind = JobKind::Connect;
	job->connection = id;
	job->amx = amx;
	job = connection->run(std::move(job));
	const Settings& used = connection->settings();
	const Result& result = *job->result;
	if (result.error == 0) {
		log("[MySQL] Connected to %s:%u as %s, database %s, %s, character set %s, %s.", used.host.c_str(), used.port, used.user.c_str(), used.database.empty() ? "none" : used.database.c_str(), result.server.c_str(), used.charset.c_str(), result.tls.empty() ? "without TLS" : result.tls.c_str());
	} else {
		logError("[MySQL] Cannot connect to %s:%u as %s: error %d, %s. Queries will try again.", used.host.c_str(), used.port, used.user.c_str(), result.error, result.message.c_str());
	}
	s.connections[slot] = std::move(connection);
	s.owners[slot] = amx;
	return id;
}

void report(Connection& connection, Job& job) {
	Result& result = *job.result;
	++connection.done;
	connection.totalTime += result.time;
	connection.longest = std::max(connection.longest, result.time);
	if (job.kind == JobKind::Connect) {
		return;
	}
	if (result.error != 0) {
		++connection.failed;
		if (connection.settings().logErrors && job.batch) {
			logError("[MySQL] Error %d (%s) in a batch of %u row%s of \"%s\": %s", result.error, result.sqlstate.c_str(), unsigned(job.rows->count), job.rows->count == 1 ? "" : "s", clip(result.query, LoggedQuery).c_str(), result.message.c_str());
		} else if (connection.settings().logErrors) {
			logError("[MySQL] Error %d (%s) in \"%s\": %s", result.error, result.sqlstate.c_str(), clip(result.query, LoggedQuery).c_str(), result.message.c_str());
		}
	}
	if (connection.settings().slowQuery > 0.0 && result.time >= connection.settings().slowQuery) {
		log("[MySQL] Slow query, %.1f ms after %.1f ms in the queue: %s", result.time, result.wait, clip(result.query, LoggedQuery).c_str());
	}
}

void reportRefusal(Connection& connection, const std::string& reason) {
	++connection.refused;
	if (!connection.settings().logErrors) {
		return;
	}
	Clock::time_point now = Clock::now();
	if (now - connection.refusalLogged < std::chrono::seconds(1)) {
		++connection.refusalsUnlogged;
		return;
	}
	if (connection.refusalsUnlogged != 0) {
		logError("[MySQL] A query was refused: %s. %llu more were refused since the line before.", reason.c_str(), (unsigned long long)connection.refusalsUnlogged);
	} else {
		logError("[MySQL] A query was refused: %s.", reason.c_str());
	}
	connection.refusalLogged = now;
	connection.refusalsUnlogged = 0;
}

void callError(Script& script, Job& job) {
	int index = -1;
	if (!findPublic(script, "OnMySQLError", index)) {
		return;
	}
	Result& result = *job.result;
	std::string query = clip(result.query, PushedQuery);
	cell callback = 0;
	cell text = 0;
	cell message = 0;
	script.api->PushString(&callback, nullptr, StringView(job.callback.data(), job.callback.size()), false, false);
	script.api->PushString(&text, nullptr, StringView(query.data(), query.size()), false, false);
	script.api->PushString(&message, nullptr, StringView(result.message.data(), result.message.size()), false, false);
	script.api->Push(result.error);
	script.api->Push(job.connection);
	cell ignored = 0;
	script.api->Exec(&ignored, index);
	script.api->Release(callback);
}

bool closeConnection(int id) {
	State& s = state();
	if (id < 1 || id > int(s.connections.size()) || !s.connections[size_t(id) - 1]) {
		return false;
	}
	std::unique_ptr<Connection> connection = std::move(s.connections[size_t(id) - 1]);
	s.owners[size_t(id) - 1] = nullptr;
	connection->stop();
	std::vector<std::unique_ptr<Job>> jobs;
	connection->collect(jobs);
	for (std::unique_ptr<Job>& job : jobs) {
		if (job->kind == JobKind::CloseStatement) {
			continue;
		}
		job->result->time = milliseconds(job->ended - job->started);
		job->result->wait = milliseconds(job->started - job->queued);
		report(*connection, *job);
	}
	return true;
}

void dispatch(std::vector<std::unique_ptr<Job>>& jobs) {
	for (std::unique_ptr<Job>& job : jobs) {
		if (job->kind == JobKind::CloseStatement) {
			continue;
		}
		Result& result = *job->result;
		result.wait = milliseconds(job->started - job->queued);
		result.time = milliseconds(job->ended - job->started);
		Connection* connection = connectionOf(job->connection);
		if (connection != nullptr) {
			report(*connection, *job);
		}
		Script* script = scriptOf(job->amx);
		if (script == nullptr || script->serial != job->owner) {
			continue;
		}
		if (result.error != 0) {
			callError(*script, *job);
		}
		if (job->callback.empty()) {
			continue;
		}
		int index = -1;
		if (!findPublic(*script, job->callback.c_str(), index)) {
			logError("[MySQL] The callback %s of a query is not a public of the script.", job->callback.c_str());
			continue;
		}
		script->active.push_back({ job->result.get(), &job->result, job->result.get(), &job->result });
		script->api->Push(job->context);
		cell ignored = 0;
		script->api->Exec(&ignored, index);
		script = scriptOf(job->amx);
		if (script != nullptr && !script->active.empty()) {
			script->active.pop_back();
		}
	}
}

void forgetScript(AMX* amx) {
	State& s = state();
	for (size_t slot = 0; slot != s.connections.size(); ++slot) {
		if (s.owners[slot] == amx && s.connections[slot]) {
			closeConnection(int(slot) + 1);
		}
	}
	auto script = s.scripts.find(amx);
	if (script != s.scripts.end()) {
		for (auto& entry : script->second.statements) {
			Connection* connection = connectionOf(entry.second.connection);
			if (connection == nullptr) {
				continue;
			}
			auto job = std::make_unique<Job>();
			job->kind = JobKind::CloseStatement;
			job->statement = entry.second.key;
			job->connection = connection->id();
			connection->submit(std::move(job));
		}
	}
	s.scripts.erase(amx);
	s.lastAmx = nullptr;
	s.lastScript = nullptr;
}

void closeAll() {
	State& s = state();
	for (size_t slot = 0; slot != s.connections.size(); ++slot) {
		if (s.connections[slot]) {
			closeConnection(int(slot) + 1);
		}
	}
}

}

using namespace mysqlcomponent;

class MySQLComponent final : public IComponent, public PawnEventHandler, public CoreEventHandler {
public:
	PROVIDE_UID(0xe55ced8c47fd2d9b);

	StringView componentName() const override {
		return "mysql";
	}

	SemanticVersion componentVersion() const override {
		return SemanticVersion(COMPONENT_VERSION_MAJOR, COMPONENT_VERSION_MINOR, COMPONENT_VERSION_PATCH, 0);
	}

	void onLoad(ICore* core) override {
		State& s = state();
		s.core = core;
		mysql_library_init(0, nullptr, nullptr);
		core->getEventDispatcher().addEventHandler(this);
		core->printLn("mysql " COMPONENT_VERSION " with MariaDB Connector/C %s.", mysql_get_client_info());
	}

	void onInit(IComponentList* components) override {
		State& s = state();
		s.pawn = components->queryComponent<IPawnComponent>();
		if (s.pawn != nullptr) {
			s.pawn->getEventDispatcher().addEventHandler(this);
		}
	}

	void onAmxLoad(IPawnScript& script) override {
		State& s = state();
		registerNatives(script);
		Script& entry = s.scripts[script.GetAMX()];
		entry = Script();
		entry.api = &script;
		entry.serial = ++s.serial;
		s.lastAmx = nullptr;
		s.lastScript = nullptr;
	}

	void onAmxUnload(IPawnScript& script) override {
		forgetScript(script.GetAMX());
	}

	void onTick(Microseconds elapsed, TimePoint now) override {
		State& s = state();
		std::vector<std::unique_ptr<Job>> jobs;
		for (std::unique_ptr<Connection>& connection : s.connections) {
			if (connection) {
				connection->collect(jobs);
			}
		}
		if (!jobs.empty()) {
			dispatch(jobs);
		}
		std::vector<std::unique_ptr<PasswordJob>> passwords;
		s.passwords.collect(passwords);
		if (!passwords.empty()) {
			dispatchPasswords(passwords);
		}
	}

	void onFree(IComponent* component) override {
		State& s = state();
		if (component == s.pawn && s.pawn != nullptr) {
			s.pawn->getEventDispatcher().removeEventHandler(this);
			s.pawn = nullptr;
		}
	}

	void reset() override {
	}

	void free() override {
		State& s = state();
		if (s.core != nullptr) {
			s.core->getEventDispatcher().removeEventHandler(this);
		}
		if (s.pawn != nullptr) {
			s.pawn->getEventDispatcher().removeEventHandler(this);
			s.pawn = nullptr;
		}
		s.passwords.stop();
		closeAll();
		mysql_library_end();
		delete this;
	}
};

COMPONENT_ENTRY_POINT() {
	return new MySQLComponent();
}
