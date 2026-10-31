#include "mysql.hpp"

#include <yyjson.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace mysqlcomponent {

namespace {

enum JsonType : cell {
	JsonNone,
	JsonNull,
	JsonBool,
	JsonInt,
	JsonFloat,
	JsonString,
	JsonArray,
	JsonObject
};

cell optional(const cell* params, int index, cell fallback) {
	return params[0] >= cell(index * sizeof(cell)) ? params[index] : fallback;
}

int arguments(const cell* params) {
	return int(params[0] / sizeof(cell));
}

bool writeCell(AMX* amx, cell address, cell value) {
	cell* target = addressOf(amx, address);
	if (target == nullptr) {
		return false;
	}
	*target = value;
	return true;
}

long long toInteger(const char* text, uint32_t length) {
	long long value = 0;
	auto read = std::from_chars(text, text + length, value);
	if (read.ec == std::errc() && read.ptr == text + length) {
		return value;
	}
	return (long long)std::strtod(text, nullptr);
}

float toFloat(const char* text, uint32_t length) {
	float value = 0.0f;
	auto read = std::from_chars(text, text + length, value);
	return read.ec == std::errc() ? value : float(std::strtod(text, nullptr));
}

bool toBool(const char* text, uint32_t length) {
	if (length == 4 && (std::memcmp(text, "true", 4) == 0 || std::memcmp(text, "TRUE", 4) == 0)) {
		return true;
	}
	return toInteger(text, length) != 0;
}

int columnOf(AMX* amx, ResultSet& set, cell address) {
	static std::string name;
	if (!readString(amx, address, name)) {
		return -1;
	}
	int column = set.find(name);
	if (column < 0) {
		Script* script = scriptOf(amx);
		if (script != nullptr && script->missing.insert(name).second) {
			logError("[MySQL] The result has no field %s.", name.c_str());
		}
	}
	return column;
}

bool cellOf(AMX* amx, cell row, cell field, const char*& text, uint32_t& length) {
	ResultSet* set = currentSet(amx);
	if (set == nullptr || row < 0) {
		return false;
	}
	int column = columnOf(amx, *set, field);
	return column >= 0 && set->cell(uint32_t(row), uint32_t(column), text, length);
}

cell queue(AMX* amx, const cell* params, int callback, int context, int query, bool parallel) {
	Connection* connection = connectionOf(params[1]);
	Script* script = scriptOf(amx);
	if (connection == nullptr || script == nullptr) {
		return false;
	}
	auto job = std::make_unique<Job>();
	std::string error;
	if (!formatQuery(amx, params, query, query + 1, connection->backslashEscapes(), job->query, error)) {
		logError("[MySQL] A query was not sent: %s.", error.c_str());
		return false;
	}
	if (!connection->admit(job->query.size(), error)) {
		reportRefusal(*connection, error);
		return false;
	}
	if (callback != 0 && !readString(amx, params[callback], job->callback)) {
		return false;
	}
	job->context = context != 0 ? params[context] : 0;
	job->amx = amx;
	job->owner = script->serial;
	job->connection = connection->id();
	job->parallel = parallel;
	++connection->sent;
	connection->submit(std::move(job));
	return true;
}

bool runJob(AMX* amx, Connection& connection, std::unique_ptr<Job> job) {
	Script* script = nullptr;
	++connection.sent;
	job = connection.run(std::move(job));
	Result& result = *job->result;
	result.wait = std::chrono::duration<double, std::milli>(job->started - job->queued).count();
	result.time = std::chrono::duration<double, std::milli>(job->ended - job->started).count();
	report(connection, *job);
	script = scriptOf(amx);
	if (script == nullptr) {
		return false;
	}
	if (result.error != 0) {
		callError(*script, *job);
	}
	bool succeeded = result.error == 0;
	script->last = std::move(job->result);
	if (script->active.empty()) {
		script->used = nullptr;
	} else {
		script->active.back().result = script->last.get();
		script->active.back().owner = &script->last;
	}
	return succeeded;
}

bool runNow(AMX* amx, Connection& connection, std::string query) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return false;
	}
	std::string reason;
	if (!connection.admit(query.size(), reason)) {
		reportRefusal(connection, reason);
		return false;
	}
	auto job = std::make_unique<Job>();
	job->query = std::move(query);
	job->amx = amx;
	job->owner = script->serial;
	job->connection = connection.id();
	return runJob(amx, connection, std::move(job));
}

Builder* builderOf(AMX* amx, cell id) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return nullptr;
	}
	auto found = script->builders.find(int(id));
	return found == script->builders.end() ? nullptr : &found->second;
}

PreparedStatement* statementOf(AMX* amx, cell id) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return nullptr;
	}
	auto found = script->statements.find(int(id));
	return found == script->statements.end() ? nullptr : &found->second;
}

Batch* batchOf(AMX* amx, cell id) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return nullptr;
	}
	auto found = script->batches.find(int(id));
	return found == script->batches.end() ? nullptr : &found->second;
}

bool readRow(AMX* amx, const cell* params, int first, const std::string& kinds, Rows& rows, std::string& error) {
	static std::string text;
	size_t given = arguments(params) >= first ? size_t(arguments(params) - first + 1) : 0;
	if (given != kinds.size()) {
		error = "the statement takes " + std::to_string(kinds.size()) + " values, and " + std::to_string(given) + " were given";
		return false;
	}
	size_t textSize = rows.text.size();
	for (size_t index = 0; index != kinds.size(); ++index) {
		cell address = params[first + int(index)];
		bool read = true;
		switch (kinds[index]) {
			case 'i':
			case 'u':
			case 'f': {
				cell* value = addressOf(amx, address);
				if (value == nullptr) {
					read = false;
				} else if (kinds[index] == 'f') {
					rows.floats[index].push_back(amx_ctof(*value));
				} else {
					rows.integers[index].push_back(int32_t(*value));
				}
				break;
			}
			case 'Q': {
				cell* value = addressOf(amx, address);
				Builder* builder = value == nullptr ? nullptr : builderOf(amx, *value);
				if (builder == nullptr) {
					error = "%Q is not given a query being built";
					read = false;
				} else {
					rows.offsets[index].push_back(uint32_t(rows.text.size()));
					rows.lengths[index].push_back((unsigned long)builder->text.size());
					rows.text.append(builder->text);
				}
				break;
			}
			default:
				if (!readString(amx, address, text)) {
					read = false;
				} else {
					rows.offsets[index].push_back(uint32_t(rows.text.size()));
					rows.lengths[index].push_back((unsigned long)text.size());
					rows.text.append(text);
				}
				break;
		}
		if (read) {
			continue;
		}
		if (error.empty()) {
			error = "a value of the statement cannot be read";
		}
		for (size_t undo = 0; undo != index; ++undo) {
			if (kinds[undo] == 'i' || kinds[undo] == 'u') {
				rows.integers[undo].pop_back();
			} else if (kinds[undo] == 'f') {
				rows.floats[undo].pop_back();
			} else {
				rows.offsets[undo].pop_back();
				rows.lengths[undo].pop_back();
			}
		}
		rows.text.resize(textSize);
		return false;
	}
	++rows.count;
	return true;
}

std::unique_ptr<Job> statementJob(AMX* amx, Script& script, Connection& connection, const PreparedStatement& prepared, std::unique_ptr<Rows> rows, bool batch) {
	auto job = std::make_unique<Job>();
	job->kind = JobKind::Statement;
	job->query = prepared.sql;
	job->statement = prepared.key;
	job->rows = std::move(rows);
	job->batch = batch;
	job->amx = amx;
	job->owner = script.serial;
	job->connection = connection.id();
	return job;
}

bool admitStatement(Connection& connection, const PreparedStatement& prepared, const Rows& rows) {
	std::string reason;
	if (connection.admit(prepared.sql.size() + rows.bytes(), reason)) {
		return true;
	}
	reportRefusal(connection, reason);
	return false;
}

yyjson_val* jsonValue(AMX* amx, cell row, cell field, cell key, bool* latin1 = nullptr) {
	ResultSet* set = currentSet(amx);
	if (set == nullptr || row < 0) {
		return nullptr;
	}
	int column = columnOf(amx, *set, field);
	if (column < 0) {
		return nullptr;
	}
	JsonCell* json = jsonOf(*set, uint32_t(row), uint32_t(column));
	if (json == nullptr) {
		return nullptr;
	}
	if (latin1 != nullptr) {
		*latin1 = set->latin1;
	}
	yyjson_val* root = yyjson_doc_get_root(json->doc);
	static std::string name;
	static std::string buffer;
	if (key == 0 || !readString(amx, key, name) || name.empty()) {
		return root;
	}
	std::string_view path = set->latin1 ? latin1ToUtf8(name, buffer) : std::string_view(name);
	if (path[0] == '/') {
		return yyjson_ptr_getn(root, path.data(), path.size());
	}
	return yyjson_obj_getn(root, path.data(), path.size());
}

int writeJsonText(AMX* amx, cell address, std::string_view text, cell size, bool latin1) {
	static std::string buffer;
	return writeString(amx, address, latin1 ? utf8ToLatin1(text, buffer) : text, size);
}

cell AMX_NATIVE_CALL MySQL_Connect(AMX* amx, const cell* params) {
	Settings settings;
	if (!readString(amx, params[1], settings.host) || !readString(amx, params[2], settings.user) || !readString(amx, params[3], settings.password) || !readString(amx, params[4], settings.database)) {
		return 0;
	}
	settings.port = unsigned(optional(params, 5, 3306));
	std::string options;
	if (arguments(params) >= 6 && !readString(amx, params[6], options)) {
		return 0;
	}
	std::string error;
	if (!readOptions(options, settings, error)) {
		logError("[MySQL] MySQL_Connect: %s.", error.c_str());
		return 0;
	}
	return openConnection(amx, std::move(settings));
}

cell AMX_NATIVE_CALL MySQL_ConnectFile(AMX* amx, const cell* params) {
	std::string path;
	if (arguments(params) >= 1 && !readString(amx, params[1], path)) {
		return 0;
	}
	if (path.empty()) {
		path = "mysql.ini";
	}
	Settings settings;
	std::string error;
	if (!readSettingsFile(path, settings, error)) {
		logError("[MySQL] MySQL_ConnectFile: %s.", error.c_str());
		return 0;
	}
	return openConnection(amx, std::move(settings));
}

cell AMX_NATIVE_CALL MySQL_Close(AMX* amx, const cell* params) {
	return closeConnection(int(params[1]));
}

cell AMX_NATIVE_CALL MySQL_IsConnected(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	return connection != nullptr && connection->connected();
}

cell AMX_NATIVE_CALL MySQL_GetConnectionErrno(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	return connection == nullptr ? -1 : connection->lastError();
}

cell AMX_NATIVE_CALL MySQL_GetConnectionError(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	if (connection == nullptr) {
		writeString(amx, params[2], "", params[3]);
		return false;
	}
	return writeString(amx, params[2], connection->lastMessage(), params[3]) >= 0;
}

cell AMX_NATIVE_CALL MySQL_GetConnectionTLS(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	std::string tls = connection == nullptr ? std::string() : connection->tls();
	writeString(amx, params[2], tls, params[3]);
	return !tls.empty();
}

cell AMX_NATIVE_CALL MySQL_GetPendingCount(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	return connection == nullptr ? -1 : cell(connection->pending());
}

cell AMX_NATIVE_CALL MySQL_GetStats(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	if (connection == nullptr) {
		return false;
	}
	float average = connection->done == 0 ? 0.0f : float(connection->totalTime / double(connection->done));
	float longest = float(connection->longest);
	writeCell(amx, params[2], cell(connection->sent));
	writeCell(amx, params[3], cell(connection->failed));
	writeCell(amx, params[4], amx_ftoc(average));
	writeCell(amx, params[5], amx_ftoc(longest));
	return true;
}

cell AMX_NATIVE_CALL MySQL_Send(AMX* amx, const cell* params) {
	return queue(amx, params, 0, 0, 2, false);
}

cell AMX_NATIVE_CALL MySQL_ThreadedQuery(AMX* amx, const cell* params) {
	return queue(amx, params, 2, 3, 4, false);
}

cell AMX_NATIVE_CALL MySQL_ParallelQuery(AMX* amx, const cell* params) {
	return queue(amx, params, 2, 3, 4, true);
}

cell AMX_NATIVE_CALL MySQL_Run(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	if (connection == nullptr) {
		return false;
	}
	std::string query;
	std::string error;
	if (!formatQuery(amx, params, 2, 3, connection->backslashEscapes(), query, error)) {
		logError("[MySQL] A query was not run: %s.", error.c_str());
		return false;
	}
	return runNow(amx, *connection, std::move(query));
}

cell AMX_NATIVE_CALL MySQL_Format(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	if (connection == nullptr || params[3] <= 0) {
		return -1;
	}
	std::string query;
	std::string error;
	if (!formatQuery(amx, params, 4, 5, connection->backslashEscapes(), query, error)) {
		logError("[MySQL] MySQL_Format: %s.", error.c_str());
		writeString(amx, params[2], "", params[3]);
		return -1;
	}
	if (query.size() >= size_t(params[3])) {
		logError("[MySQL] MySQL_Format: the query takes %u characters, more than the %d of the array.", unsigned(query.size()), int(params[3]) - 1);
		writeString(amx, params[2], "", params[3]);
		return -1;
	}
	return writeString(amx, params[2], query, params[3]);
}

cell AMX_NATIVE_CALL MySQL_CreateQuery(AMX* amx, const cell* params) {
	Connection* connection = connectionOf(params[1]);
	Script* script = scriptOf(amx);
	if (connection == nullptr || script == nullptr) {
		return 0;
	}
	Builder builder;
	builder.connection = connection->id();
	if (arguments(params) >= 2) {
		std::string error;
		if (!formatQuery(amx, params, 2, 3, connection->backslashEscapes(), builder.text, error)) {
			logError("[MySQL] MySQL_CreateQuery: %s.", error.c_str());
			return 0;
		}
	}
	int id = script->nextBuilder++;
	script->builders.emplace(id, std::move(builder));
	return id;
}

cell AMX_NATIVE_CALL MySQL_AppendQuery(AMX* amx, const cell* params) {
	Builder* builder = builderOf(amx, params[1]);
	Connection* connection = builder == nullptr ? nullptr : connectionOf(builder->connection);
	if (connection == nullptr) {
		return false;
	}
	size_t size = builder->text.size();
	std::string error;
	if (!formatQuery(amx, params, 2, 3, connection->backslashEscapes(), builder->text, error)) {
		builder->text.resize(size);
		logError("[MySQL] MySQL_AppendQuery: %s.", error.c_str());
		return false;
	}
	return true;
}

cell AMX_NATIVE_CALL MySQL_TrimQuery(AMX* amx, const cell* params) {
	Builder* builder = builderOf(amx, params[1]);
	if (builder == nullptr || params[2] < 0) {
		return false;
	}
	builder->text.resize(builder->text.size() - std::min(builder->text.size(), size_t(params[2])));
	return true;
}

cell AMX_NATIVE_CALL MySQL_GetQueryLength(AMX* amx, const cell* params) {
	Builder* builder = builderOf(amx, params[1]);
	return builder == nullptr ? -1 : cell(builder->text.size());
}

cell AMX_NATIVE_CALL MySQL_SendQuery(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	Builder* builder = builderOf(amx, params[1]);
	Connection* connection = builder == nullptr ? nullptr : connectionOf(builder->connection);
	if (connection == nullptr) {
		return false;
	}
	std::string reason;
	if (!connection->admit(builder->text.size(), reason)) {
		script->builders.erase(int(params[1]));
		reportRefusal(*connection, reason);
		return false;
	}
	auto job = std::make_unique<Job>();
	if (arguments(params) >= 2 && !readString(amx, params[2], job->callback)) {
		return false;
	}
	job->query = std::move(builder->text);
	job->context = optional(params, 3, 0);
	job->parallel = optional(params, 4, 0) != 0;
	job->amx = amx;
	job->owner = script->serial;
	job->connection = connection->id();
	script->builders.erase(int(params[1]));
	++connection->sent;
	connection->submit(std::move(job));
	return true;
}

cell AMX_NATIVE_CALL MySQL_RunQuery(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	Builder* builder = builderOf(amx, params[1]);
	Connection* connection = builder == nullptr ? nullptr : connectionOf(builder->connection);
	if (connection == nullptr) {
		return false;
	}
	std::string query = std::move(builder->text);
	script->builders.erase(int(params[1]));
	return runNow(amx, *connection, std::move(query));
}

cell AMX_NATIVE_CALL MySQL_DestroyQuery(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	return script != nullptr && script->builders.erase(int(params[1])) != 0;
}

cell AMX_NATIVE_CALL MySQL_ClearQuery(AMX* amx, const cell* params) {
	Builder* builder = builderOf(amx, params[1]);
	if (builder == nullptr) {
		return false;
	}
	builder->text.clear();
	return true;
}

cell AMX_NATIVE_CALL MySQL_PrepareStatement(AMX* amx, const cell* params) {
	static std::string pattern;
	Connection* connection = connectionOf(params[1]);
	Script* script = scriptOf(amx);
	if (connection == nullptr || script == nullptr || !readString(amx, params[2], pattern)) {
		return 0;
	}
	PreparedStatement prepared;
	std::string error;
	if (!parseStatement(pattern, prepared.sql, prepared.types, error)) {
		logError("[MySQL] MySQL_PrepareStatement: %s.", error.c_str());
		return 0;
	}
	prepared.connection = connection->id();
	prepared.key = ++state().statementSerial;
	auto job = std::make_unique<Job>();
	job->kind = JobKind::Prepare;
	job->query = prepared.sql;
	job->statement = prepared.key;
	job->rows = std::make_unique<Rows>();
	job->rows->init(prepared.types);
	job->amx = amx;
	job->owner = script->serial;
	job->connection = connection->id();
	++connection->sent;
	connection->submit(std::move(job));
	int id = script->nextStatement++;
	script->statements.emplace(id, std::move(prepared));
	return id;
}

cell AMX_NATIVE_CALL MySQL_ExecuteStatement(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	PreparedStatement* prepared = statementOf(amx, params[1]);
	Connection* connection = prepared == nullptr ? nullptr : connectionOf(prepared->connection);
	if (connection == nullptr) {
		return false;
	}
	auto rows = std::make_unique<Rows>();
	rows->init(prepared->types);
	std::string error;
	if (!readRow(amx, params, 4, prepared->types, *rows, error)) {
		logError("[MySQL] MySQL_ExecuteStatement: %s.", error.c_str());
		return false;
	}
	if (!admitStatement(*connection, *prepared, *rows)) {
		return false;
	}
	auto job = statementJob(amx, *script, *connection, *prepared, std::move(rows), false);
	if (!readString(amx, params[2], job->callback)) {
		return false;
	}
	job->context = params[3];
	++connection->sent;
	connection->submit(std::move(job));
	return true;
}

cell AMX_NATIVE_CALL MySQL_RunStatement(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	PreparedStatement* prepared = statementOf(amx, params[1]);
	Connection* connection = prepared == nullptr ? nullptr : connectionOf(prepared->connection);
	if (connection == nullptr) {
		return false;
	}
	auto rows = std::make_unique<Rows>();
	rows->init(prepared->types);
	std::string error;
	if (!readRow(amx, params, 2, prepared->types, *rows, error)) {
		logError("[MySQL] MySQL_RunStatement: %s.", error.c_str());
		return false;
	}
	if (!admitStatement(*connection, *prepared, *rows)) {
		return false;
	}
	return runJob(amx, *connection, statementJob(amx, *script, *connection, *prepared, std::move(rows), false));
}

cell AMX_NATIVE_CALL MySQL_CreateBatch(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	PreparedStatement* prepared = statementOf(amx, params[1]);
	if (prepared == nullptr) {
		return 0;
	}
	int id = script->nextBatch++;
	Batch& batch = script->batches[id];
	batch.statement = int(params[1]);
	batch.rows.init(prepared->types);
	return id;
}

cell AMX_NATIVE_CALL MySQL_AddBatchRow(AMX* amx, const cell* params) {
	Batch* batch = batchOf(amx, params[1]);
	PreparedStatement* prepared = batch == nullptr ? nullptr : statementOf(amx, batch->statement);
	if (prepared == nullptr) {
		return false;
	}
	std::string error;
	if (!readRow(amx, params, 2, prepared->types, batch->rows, error)) {
		logError("[MySQL] MySQL_AddBatchRow: %s.", error.c_str());
		return false;
	}
	return true;
}

cell AMX_NATIVE_CALL MySQL_GetBatchRows(AMX* amx, const cell* params) {
	Batch* batch = batchOf(amx, params[1]);
	return batch == nullptr ? -1 : cell(batch->rows.count);
}

cell sendBatch(AMX* amx, const cell* params, bool wait) {
	Script* script = scriptOf(amx);
	Batch* batch = batchOf(amx, params[1]);
	PreparedStatement* prepared = batch == nullptr ? nullptr : statementOf(amx, batch->statement);
	Connection* connection = prepared == nullptr ? nullptr : connectionOf(prepared->connection);
	if (connection == nullptr) {
		if (batch != nullptr) {
			script->batches.erase(int(params[1]));
		}
		return false;
	}
	auto rows = std::make_unique<Rows>(std::move(batch->rows));
	script->batches.erase(int(params[1]));
	std::string callback;
	if (!wait && arguments(params) >= 2 && !readString(amx, params[2], callback)) {
		return false;
	}
	if (!wait && rows->count == 0 && callback.empty()) {
		return true;
	}
	if (!admitStatement(*connection, *prepared, *rows)) {
		return false;
	}
	auto job = statementJob(amx, *script, *connection, *prepared, std::move(rows), true);
	if (wait) {
		return runJob(amx, *connection, std::move(job));
	}
	job->callback = std::move(callback);
	job->context = optional(params, 3, 0);
	++connection->sent;
	connection->submit(std::move(job));
	return true;
}

cell AMX_NATIVE_CALL MySQL_ExecuteBatch(AMX* amx, const cell* params) {
	return sendBatch(amx, params, false);
}

cell AMX_NATIVE_CALL MySQL_RunBatch(AMX* amx, const cell* params) {
	return sendBatch(amx, params, true);
}

cell AMX_NATIVE_CALL MySQL_DestroyBatch(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	return script != nullptr && script->batches.erase(int(params[1])) != 0;
}

cell AMX_NATIVE_CALL MySQL_CloseStatement(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	PreparedStatement* prepared = statementOf(amx, params[1]);
	if (prepared == nullptr) {
		return false;
	}
	Connection* connection = connectionOf(prepared->connection);
	if (connection != nullptr) {
		auto job = std::make_unique<Job>();
		job->kind = JobKind::CloseStatement;
		job->statement = prepared->key;
		job->connection = connection->id();
		connection->submit(std::move(job));
	}
	for (auto entry = script->batches.begin(); entry != script->batches.end();) {
		if (entry->second.statement == int(params[1])) {
			entry = script->batches.erase(entry);
		} else {
			++entry;
		}
	}
	script->statements.erase(int(params[1]));
	return true;
}

cell AMX_NATIVE_CALL MySQL_GetErrno(AMX* amx, const cell* params) {
	Result* result = currentResult(amx);
	return result == nullptr ? 0 : result->error;
}

cell AMX_NATIVE_CALL MySQL_GetError(AMX* amx, const cell* params) {
	Result* result = currentResult(amx);
	return writeString(amx, params[1], result == nullptr ? std::string_view() : std::string_view(result->message), params[2]) >= 0 && result != nullptr;
}

cell AMX_NATIVE_CALL MySQL_GetQuery(AMX* amx, const cell* params) {
	Result* result = currentResult(amx);
	return writeString(amx, params[1], result == nullptr ? std::string_view() : std::string_view(result->query), params[2]) >= 0 && result != nullptr;
}

cell AMX_NATIVE_CALL MySQL_GetQueryTime(AMX* amx, const cell* params) {
	Result* result = currentResult(amx);
	float time = result == nullptr ? 0.0f : float(result->time);
	return amx_ftoc(time);
}

cell AMX_NATIVE_CALL MySQL_GetResultSetCount(AMX* amx, const cell* params) {
	Result* result = currentResult(amx);
	return result == nullptr ? 0 : cell(result->sets.size());
}

cell AMX_NATIVE_CALL MySQL_SelectResultSet(AMX* amx, const cell* params) {
	Result* result = currentResult(amx);
	if (result == nullptr || params[1] < 0 || size_t(params[1]) >= result->sets.size()) {
		return false;
	}
	result->set = uint32_t(params[1]);
	return true;
}

cell AMX_NATIVE_CALL MySQL_GetRowCount(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	return set == nullptr ? 0 : cell(set->rows);
}

cell AMX_NATIVE_CALL MySQL_GetFieldCount(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	return set == nullptr ? 0 : cell(set->columns.size());
}

cell AMX_NATIVE_CALL MySQL_GetFieldName(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	if (set == nullptr || params[1] < 0 || size_t(params[1]) >= set->columns.size()) {
		writeString(amx, params[2], "", params[3]);
		return false;
	}
	return writeString(amx, params[2], set->columns[size_t(params[1])].name, params[3]) >= 0;
}

cell AMX_NATIVE_CALL MySQL_GetFieldIndex(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	static std::string name;
	if (set == nullptr || !readString(amx, params[1], name)) {
		return -1;
	}
	return set->find(name);
}

cell AMX_NATIVE_CALL MySQL_GetAffectedRows(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	return set == nullptr ? 0 : cell(set->affectedRows);
}

cell AMX_NATIVE_CALL MySQL_GetInsertID(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	return set == nullptr ? 0 : cell(set->insertId);
}

cell AMX_NATIVE_CALL MySQL_GetWarningCount(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	return set == nullptr ? 0 : cell(set->warnings);
}

cell AMX_NATIVE_CALL MySQL_IsNull(AMX* amx, const cell* params) {
	const char* text = nullptr;
	uint32_t length = 0;
	return !cellOf(amx, params[1], params[2], text, length) || text == nullptr;
}

cell AMX_NATIVE_CALL MySQL_GetInt(AMX* amx, const cell* params) {
	const char* text = nullptr;
	uint32_t length = 0;
	if (!cellOf(amx, params[1], params[2], text, length) || text == nullptr) {
		return 0;
	}
	return cell(toInteger(text, length));
}

cell AMX_NATIVE_CALL MySQL_GetFloat(AMX* amx, const cell* params) {
	const char* text = nullptr;
	uint32_t length = 0;
	float value = 0.0f;
	if (cellOf(amx, params[1], params[2], text, length) && text != nullptr) {
		value = toFloat(text, length);
	}
	return amx_ftoc(value);
}

cell AMX_NATIVE_CALL MySQL_GetBool(AMX* amx, const cell* params) {
	const char* text = nullptr;
	uint32_t length = 0;
	return cellOf(amx, params[1], params[2], text, length) && text != nullptr && toBool(text, length);
}

cell AMX_NATIVE_CALL MySQL_GetString(AMX* amx, const cell* params) {
	const char* text = nullptr;
	uint32_t length = 0;
	if (!cellOf(amx, params[1], params[2], text, length) || text == nullptr) {
		writeString(amx, params[3], "", params[4]);
		return false;
	}
	return writeString(amx, params[3], std::string_view(text, length), params[4]) >= 0;
}

cell AMX_NATIVE_CALL MySQL_GetRow(AMX* amx, const cell* params) {
	ResultSet* set = currentSet(amx);
	static std::string format;
	if (set == nullptr || params[1] < 0 || uint32_t(params[1]) >= set->rows || !readString(amx, params[2], format)) {
		return 0;
	}
	uint32_t row = uint32_t(params[1]);
	int count = arguments(params);
	int argument = 3;
	uint32_t column = 0;
	cell read = 0;
	for (size_t at = 0; at < format.size(); ++at) {
		char specifier = format[at];
		if (specifier == ' ') {
			continue;
		}
		cell size = 0;
		if (specifier == 's' && at + 1 < format.size() && format[at + 1] == '[') {
			size_t close = format.find(']', at);
			if (close == std::string::npos) {
				logError("[MySQL] MySQL_GetRow: the format \"%s\" leaves a string size open.", format.c_str());
				return read;
			}
			std::from_chars(format.data() + at + 2, format.data() + close, size);
			at = close;
		}
		if (column >= set->columns.size()) {
			logError("[MySQL] MySQL_GetRow: the format \"%s\" reads more fields than the %u of the result.", format.c_str(), unsigned(set->columns.size()));
			return read;
		}
		const char* text = nullptr;
		uint32_t length = 0;
		set->cell(row, column++, text, length);
		if (specifier == '-' || specifier == '_') {
			continue;
		}
		if (argument > count) {
			logError("[MySQL] MySQL_GetRow: the format \"%s\" has more fields than variables.", format.c_str());
			return read;
		}
		cell address = params[argument++];
		switch (specifier) {
			case 'i':
			case 'd':
				writeCell(amx, address, text == nullptr ? 0 : cell(toInteger(text, length)));
				break;
			case 'f': {
				float value = text == nullptr ? 0.0f : toFloat(text, length);
				writeCell(amx, address, amx_ftoc(value));
				break;
			}
			case 'b':
				writeCell(amx, address, text != nullptr && toBool(text, length));
				break;
			case 's':
				if (size <= 0) {
					logError("[MySQL] MySQL_GetRow: a string in \"%s\" needs its size, as s[32].", format.c_str());
					return read;
				}
				writeString(amx, address, text == nullptr ? std::string_view() : std::string_view(text, length), size);
				break;
			default:
				logError("[MySQL] MySQL_GetRow: unknown specifier %c in \"%s\".", specifier, format.c_str());
				return read;
		}
		++read;
	}
	return read;
}

cell AMX_NATIVE_CALL MySQL_KeepResult(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return 0;
	}
	std::unique_ptr<Result>* owner = nullptr;
	if (!script->active.empty()) {
		owner = script->active.back().owner;
	} else if (script->used != nullptr) {
		for (auto& kept : script->kept) {
			if (kept.second.get() == script->used) {
				return kept.first;
			}
		}
		return 0;
	} else {
		owner = &script->last;
	}
	if (owner == nullptr || !*owner) {
		return 0;
	}
	for (auto& kept : script->kept) {
		if (&kept.second == owner) {
			return kept.first;
		}
	}
	int id = script->nextResult++;
	std::unique_ptr<Result>& slot = script->kept[id];
	slot = std::move(*owner);
	for (ActiveResult& active : script->active) {
		if (active.owner == owner) {
			active.owner = &slot;
		}
		if (active.originalOwner == owner) {
			active.originalOwner = &slot;
		}
	}
	if (script->active.empty()) {
		script->used = slot.get();
	}
	return id;
}

cell AMX_NATIVE_CALL MySQL_UseResult(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return false;
	}
	Result* result = nullptr;
	std::unique_ptr<Result>* owner = nullptr;
	if (params[1] != 0) {
		auto found = script->kept.find(int(params[1]));
		if (found == script->kept.end()) {
			return false;
		}
		owner = &found->second;
		result = found->second.get();
	}
	if (script->active.empty()) {
		script->used = result;
		return true;
	}
	ActiveResult& active = script->active.back();
	active.result = result != nullptr ? result : active.original;
	active.owner = result != nullptr ? owner : active.originalOwner;
	return true;
}

cell AMX_NATIVE_CALL MySQL_DestroyResult(AMX* amx, const cell* params) {
	Script* script = scriptOf(amx);
	if (script == nullptr) {
		return false;
	}
	auto found = script->kept.find(int(params[1]));
	if (found == script->kept.end()) {
		return false;
	}
	for (ActiveResult& active : script->active) {
		if (active.owner == &found->second || active.originalOwner == &found->second) {
			return false;
		}
	}
	if (script->used == found->second.get()) {
		script->used = nullptr;
	}
	script->kept.erase(found);
	return true;
}

cell AMX_NATIVE_CALL MySQL_GetJSONType(AMX* amx, const cell* params) {
	yyjson_val* value = jsonValue(amx, params[1], params[2], optional(params, 3, 0));
	if (value == nullptr) {
		return JsonNone;
	}
	switch (yyjson_get_type(value)) {
		case YYJSON_TYPE_NULL:
			return JsonNull;
		case YYJSON_TYPE_BOOL:
			return JsonBool;
		case YYJSON_TYPE_NUM:
			return yyjson_is_real(value) ? JsonFloat : JsonInt;
		case YYJSON_TYPE_STR:
			return JsonString;
		case YYJSON_TYPE_ARR:
			return JsonArray;
		case YYJSON_TYPE_OBJ:
			return JsonObject;
		default:
			return JsonNone;
	}
}

cell AMX_NATIVE_CALL MySQL_GetJSONCount(AMX* amx, const cell* params) {
	yyjson_val* value = jsonValue(amx, params[1], params[2], optional(params, 3, 0));
	if (yyjson_is_obj(value)) {
		return cell(yyjson_obj_size(value));
	}
	if (yyjson_is_arr(value)) {
		return cell(yyjson_arr_size(value));
	}
	return 0;
}

cell AMX_NATIVE_CALL MySQL_GetJSONKey(AMX* amx, const cell* params) {
	bool latin1 = false;
	yyjson_val* object = jsonValue(amx, params[1], params[2], optional(params, 6, 0), &latin1);
	if (!yyjson_is_obj(object) || params[3] < 0 || size_t(params[3]) >= yyjson_obj_size(object)) {
		writeString(amx, params[4], "", params[5]);
		return false;
	}
	yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
	yyjson_val* key = nullptr;
	for (cell index = 0; index <= params[3]; ++index) {
		key = yyjson_obj_iter_next(&iterator);
	}
	return writeJsonText(amx, params[4], std::string_view(yyjson_get_str(key), yyjson_get_len(key)), params[5], latin1) >= 0;
}

cell AMX_NATIVE_CALL MySQL_GetJSONInt(AMX* amx, const cell* params) {
	yyjson_val* value = jsonValue(amx, params[1], params[2], params[3]);
	if (yyjson_is_int(value)) {
		return cell(yyjson_get_sint(value));
	}
	if (yyjson_is_real(value)) {
		return cell(yyjson_get_real(value));
	}
	if (yyjson_is_bool(value)) {
		return yyjson_get_bool(value) ? 1 : 0;
	}
	if (yyjson_is_str(value)) {
		return cell(toInteger(yyjson_get_str(value), uint32_t(yyjson_get_len(value))));
	}
	return optional(params, 4, 0);
}

cell AMX_NATIVE_CALL MySQL_GetJSONFloat(AMX* amx, const cell* params) {
	yyjson_val* value = jsonValue(amx, params[1], params[2], params[3]);
	float result = 0.0f;
	if (yyjson_is_num(value)) {
		result = float(yyjson_get_num(value));
	} else if (yyjson_is_bool(value)) {
		result = yyjson_get_bool(value) ? 1.0f : 0.0f;
	} else if (yyjson_is_str(value)) {
		result = toFloat(yyjson_get_str(value), uint32_t(yyjson_get_len(value)));
	} else {
		return optional(params, 4, 0);
	}
	return amx_ftoc(result);
}

cell AMX_NATIVE_CALL MySQL_GetJSONBool(AMX* amx, const cell* params) {
	yyjson_val* value = jsonValue(amx, params[1], params[2], params[3]);
	if (yyjson_is_bool(value)) {
		return yyjson_get_bool(value);
	}
	if (yyjson_is_num(value)) {
		return yyjson_get_num(value) != 0.0;
	}
	if (yyjson_is_str(value)) {
		return toBool(yyjson_get_str(value), uint32_t(yyjson_get_len(value)));
	}
	return optional(params, 4, 0) != 0;
}

cell AMX_NATIVE_CALL MySQL_GetJSONString(AMX* amx, const cell* params) {
	bool latin1 = false;
	yyjson_val* value = jsonValue(amx, params[1], params[2], params[3], &latin1);
	if (value == nullptr || yyjson_is_null(value)) {
		writeString(amx, params[4], "", params[5]);
		return false;
	}
	if (yyjson_is_str(value)) {
		return writeJsonText(amx, params[4], std::string_view(yyjson_get_str(value), yyjson_get_len(value)), params[5], latin1) >= 0;
	}
	size_t length = 0;
	char* text = yyjson_val_write(value, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &length);
	if (text == nullptr) {
		writeString(amx, params[4], "", params[5]);
		return false;
	}
	bool written = writeJsonText(amx, params[4], std::string_view(text, length), params[5], latin1) >= 0;
	std::free(text);
	return written;
}

cell submitPassword(AMX* amx, std::unique_ptr<PasswordJob> job, cell callback, cell context) {
	Script* script = scriptOf(amx);
	int index = -1;
	if (script == nullptr || !readString(amx, callback, job->callback) || !findPublic(*script, job->callback.c_str(), index)) {
		return false;
	}
	job->context = context;
	job->amx = amx;
	job->owner = script->serial;
	state().passwords.submit(std::move(job));
	return true;
}

cell AMX_NATIVE_CALL MySQL_HashArgon2(AMX* amx, const cell* params) {
	auto job = std::make_unique<PasswordJob>();
	job->work = PasswordWork::Argon2;
	cell memory = optional(params, 4, 19456);
	cell iterations = optional(params, 5, 2);
	cell parallelism = optional(params, 6, 1);
	if (memory < 8192 || memory > 1048576 || iterations < 1 || iterations > 16 || parallelism < 1 || parallelism > 8 || !readString(amx, params[1], job->password)) {
		return false;
	}
	job->memory = uint32_t(memory);
	job->iterations = uint32_t(iterations);
	job->parallelism = uint32_t(parallelism);
	return submitPassword(amx, std::move(job), params[2], optional(params, 3, 0));
}

cell AMX_NATIVE_CALL MySQL_HashBcrypt(AMX* amx, const cell* params) {
	auto job = std::make_unique<PasswordJob>();
	job->work = PasswordWork::Bcrypt;
	cell cost = optional(params, 4, 12);
	if (cost < 4 || cost > 20 || !readString(amx, params[1], job->password)) {
		return false;
	}
	job->cost = uint32_t(cost);
	return submitPassword(amx, std::move(job), params[2], optional(params, 3, 0));
}

cell AMX_NATIVE_CALL MySQL_VerifyPassword(AMX* amx, const cell* params) {
	auto job = std::make_unique<PasswordJob>();
	job->work = PasswordWork::Verify;
	if (!readString(amx, params[1], job->password) || !readString(amx, params[2], job->hash)) {
		return false;
	}
	return submitPassword(amx, std::move(job), params[3], optional(params, 4, 0));
}

const AMX_NATIVE_INFO natives[] = {
	{ "MySQL_Connect", MySQL_Connect },
	{ "MySQL_ConnectFile", MySQL_ConnectFile },
	{ "MySQL_Close", MySQL_Close },
	{ "MySQL_IsConnected", MySQL_IsConnected },
	{ "MySQL_GetConnectionErrno", MySQL_GetConnectionErrno },
	{ "MySQL_GetConnectionError", MySQL_GetConnectionError },
	{ "MySQL_GetConnectionTLS", MySQL_GetConnectionTLS },
	{ "MySQL_GetPendingCount", MySQL_GetPendingCount },
	{ "MySQL_GetStats", MySQL_GetStats },
	{ "MySQL_Send", MySQL_Send },
	{ "MySQL_ThreadedQuery", MySQL_ThreadedQuery },
	{ "MySQL_ParallelQuery", MySQL_ParallelQuery },
	{ "MySQL_Run", MySQL_Run },
	{ "MySQL_Format", MySQL_Format },
	{ "MySQL_CreateQuery", MySQL_CreateQuery },
	{ "MySQL_AppendQuery", MySQL_AppendQuery },
	{ "MySQL_TrimQuery", MySQL_TrimQuery },
	{ "MySQL_GetQueryLength", MySQL_GetQueryLength },
	{ "MySQL_SendQuery", MySQL_SendQuery },
	{ "MySQL_RunQuery", MySQL_RunQuery },
	{ "MySQL_DestroyQuery", MySQL_DestroyQuery },
	{ "MySQL_ClearQuery", MySQL_ClearQuery },
	{ "MySQL_PrepareStatement", MySQL_PrepareStatement },
	{ "MySQL_ExecuteStatement", MySQL_ExecuteStatement },
	{ "MySQL_RunStatement", MySQL_RunStatement },
	{ "MySQL_CreateBatch", MySQL_CreateBatch },
	{ "MySQL_AddBatchRow", MySQL_AddBatchRow },
	{ "MySQL_GetBatchRows", MySQL_GetBatchRows },
	{ "MySQL_ExecuteBatch", MySQL_ExecuteBatch },
	{ "MySQL_RunBatch", MySQL_RunBatch },
	{ "MySQL_DestroyBatch", MySQL_DestroyBatch },
	{ "MySQL_CloseStatement", MySQL_CloseStatement },
	{ "MySQL_GetErrno", MySQL_GetErrno },
	{ "MySQL_GetError", MySQL_GetError },
	{ "MySQL_GetQuery", MySQL_GetQuery },
	{ "MySQL_GetQueryTime", MySQL_GetQueryTime },
	{ "MySQL_GetResultSetCount", MySQL_GetResultSetCount },
	{ "MySQL_SelectResultSet", MySQL_SelectResultSet },
	{ "MySQL_GetRowCount", MySQL_GetRowCount },
	{ "MySQL_GetFieldCount", MySQL_GetFieldCount },
	{ "MySQL_GetFieldName", MySQL_GetFieldName },
	{ "MySQL_GetFieldIndex", MySQL_GetFieldIndex },
	{ "MySQL_GetAffectedRows", MySQL_GetAffectedRows },
	{ "MySQL_GetInsertID", MySQL_GetInsertID },
	{ "MySQL_GetWarningCount", MySQL_GetWarningCount },
	{ "MySQL_IsNull", MySQL_IsNull },
	{ "MySQL_GetInt", MySQL_GetInt },
	{ "MySQL_GetFloat", MySQL_GetFloat },
	{ "MySQL_GetBool", MySQL_GetBool },
	{ "MySQL_GetString", MySQL_GetString },
	{ "MySQL_GetRow", MySQL_GetRow },
	{ "MySQL_KeepResult", MySQL_KeepResult },
	{ "MySQL_UseResult", MySQL_UseResult },
	{ "MySQL_DestroyResult", MySQL_DestroyResult },
	{ "MySQL_GetJSONType", MySQL_GetJSONType },
	{ "MySQL_GetJSONCount", MySQL_GetJSONCount },
	{ "MySQL_GetJSONKey", MySQL_GetJSONKey },
	{ "MySQL_GetJSONInt", MySQL_GetJSONInt },
	{ "MySQL_GetJSONFloat", MySQL_GetJSONFloat },
	{ "MySQL_GetJSONBool", MySQL_GetJSONBool },
	{ "MySQL_GetJSONString", MySQL_GetJSONString },
	{ "MySQL_HashArgon2", MySQL_HashArgon2 },
	{ "MySQL_HashBcrypt", MySQL_HashBcrypt },
	{ "MySQL_VerifyPassword", MySQL_VerifyPassword },
};

}

void registerNatives(IPawnScript& script) {
	script.Register(natives, int(sizeof natives / sizeof natives[0]));
}

}
