#include <mysql.h>
#include <errmsg.h>
#include <mysqld_error.h>

#include "mysql.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mysqlcomponent {

namespace {

constexpr auto PingAfter = std::chrono::seconds(30);
constexpr auto RetryFirst = std::chrono::milliseconds(500);
constexpr auto RetryLongest = std::chrono::milliseconds(5000);

bool lostConnection(unsigned code) {
	return code == CR_SERVER_GONE_ERROR || code == CR_SERVER_LOST || code == CR_CONNECTION_ERROR || code == CR_CONN_HOST_ERROR || code == CR_SERVER_LOST_EXTENDED || code == ER_CONNECTION_KILLED || code == ER_SERVER_SHUTDOWN;
}

void setText(MYSQL* mysql, mysql_option option, const std::string& value) {
	if (!value.empty()) {
		mysql_options(mysql, option, value.c_str());
	}
}

void setSeconds(MYSQL* mysql, mysql_option option, unsigned seconds) {
	if (seconds != 0) {
		unsigned int value = seconds;
		mysql_options(mysql, option, &value);
	}
}

void copySet(MYSQL_RES* res, ResultSet& set) {
	unsigned fields = mysql_num_fields(res);
	MYSQL_FIELD* list = mysql_fetch_fields(res);
	set.columns.resize(fields);
	for (unsigned index = 0; index != fields; ++index) {
		Column& column = set.columns[index];
		column.name.assign(list[index].name, list[index].name_length);
		column.type = uint32_t(list[index].type);
		std::string key = column.name;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		set.names.emplace(std::move(key), index);
	}
	uint64_t rows = mysql_num_rows(res);
	set.rows = uint32_t(rows);
	set.offsets.reserve(size_t(rows) * fields);
	set.lengths.reserve(size_t(rows) * fields);
	MYSQL_ROW row;
	while ((row = mysql_fetch_row(res)) != nullptr) {
		unsigned long* lengths = mysql_fetch_lengths(res);
		for (unsigned index = 0; index != fields; ++index) {
			set.offsets.push_back(uint32_t(set.data.size()));
			if (row[index] == nullptr) {
				set.lengths.push_back(NullLength);
				continue;
			}
			set.lengths.push_back(uint32_t(lengths[index]));
			set.data.append(row[index], lengths[index]);
			set.data.push_back('\0');
		}
	}
}

size_t lengthBytes(size_t length) {
	return length < 251 ? 1 : length < 65536 ? 3 : length < 16777216 ? 4 : 9;
}

bool bulkSupported(MYSQL* mysql) {
	unsigned long capabilities = 0;
	if (mariadb_get_infov(mysql, MARIADB_CONNECTION_EXTENDED_SERVER_CAPABILITIES, &capabilities) != 0) {
		return false;
	}
	return (capabilities & (MARIADB_CLIENT_STMT_BULK_OPERATIONS >> 32)) != 0;
}

bool ownTransaction(MYSQL* mysql) {
	unsigned int status = 0;
	if (mariadb_get_infov(mysql, MARIADB_CONNECTION_SERVER_STATUS, &status) != 0) {
		return false;
	}
	return (status & SERVER_STATUS_AUTOCOMMIT) != 0 && (status & SERVER_STATUS_IN_TRANS) == 0;
}

void bindRows(Rows& rows, uint32_t first, bool arrays, std::vector<std::vector<char*>>& pointers, std::vector<MYSQL_BIND>& binds) {
	size_t count = rows.types.size();
	binds.assign(count, MYSQL_BIND());
	for (size_t index = 0; index != count; ++index) {
		MYSQL_BIND& bind = binds[index];
		switch (rows.types[index]) {
			case 'i':
			case 'u':
				bind.buffer_type = MYSQL_TYPE_LONG;
				bind.is_unsigned = rows.types[index] == 'u';
				bind.buffer = rows.integers[index].data() + first;
				break;
			case 'f':
				bind.buffer_type = MYSQL_TYPE_FLOAT;
				bind.buffer = rows.floats[index].data() + first;
				break;
			default:
				bind.buffer_type = MYSQL_TYPE_STRING;
				bind.length = rows.lengths[index].data() + first;
				if (arrays) {
					bind.buffer = pointers[index].data() + first;
				} else {
					bind.buffer = pointers[index][first];
					bind.buffer_length = rows.lengths[index][first];
				}
				break;
		}
	}
}

void copyStatementSet(MYSQL_STMT* statement, MYSQL_RES* meta, ResultSet& set) {
	unsigned fields = mysql_num_fields(meta);
	MYSQL_FIELD* list = mysql_fetch_fields(meta);
	set.columns.resize(fields);
	std::vector<MYSQL_BIND> binds(fields, MYSQL_BIND());
	std::vector<std::vector<char>> buffers(fields);
	std::vector<unsigned long> lengths(fields);
	std::vector<my_bool> nulls(fields);
	std::vector<my_bool> errors(fields);
	for (unsigned index = 0; index != fields; ++index) {
		Column& column = set.columns[index];
		column.name.assign(list[index].name, list[index].name_length);
		column.type = uint32_t(list[index].type);
		std::string key = column.name;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		set.names.emplace(std::move(key), index);
		buffers[index].resize(std::max<size_t>(list[index].max_length, 64) + 1);
		MYSQL_BIND& bind = binds[index];
		bind.buffer_type = MYSQL_TYPE_STRING;
		bind.buffer = buffers[index].data();
		bind.buffer_length = (unsigned long)buffers[index].size();
		bind.length = &lengths[index];
		bind.is_null = &nulls[index];
		bind.error = &errors[index];
	}
	mysql_stmt_bind_result(statement, binds.data());
	uint64_t total = mysql_stmt_num_rows(statement);
	set.offsets.reserve(size_t(total) * fields);
	set.lengths.reserve(size_t(total) * fields);
	std::vector<char> whole;
	for (;;) {
		int status = mysql_stmt_fetch(statement);
		if (status == 1 || status == MYSQL_NO_DATA) {
			break;
		}
		for (unsigned index = 0; index != fields; ++index) {
			set.offsets.push_back(uint32_t(set.data.size()));
			if (nulls[index]) {
				set.lengths.push_back(NullLength);
				continue;
			}
			set.lengths.push_back(uint32_t(lengths[index]));
			if (lengths[index] < buffers[index].size()) {
				set.data.append(buffers[index].data(), lengths[index]);
			} else {
				whole.resize(size_t(lengths[index]) + 1);
				MYSQL_BIND one = binds[index];
				one.buffer = whole.data();
				one.buffer_length = (unsigned long)whole.size();
				mysql_stmt_fetch_column(statement, &one, index, 0);
				set.data.append(whole.data(), lengths[index]);
			}
			set.data.push_back('\0');
		}
		++set.rows;
	}
}

}

void Rows::init(const std::string& kinds) {
	count = 0;
	types = kinds;
	std::replace(types.begin(), types.end(), 'Q', 's');
	integers.assign(kinds.size(), {});
	floats.assign(kinds.size(), {});
	offsets.assign(kinds.size(), {});
	lengths.assign(kinds.size(), {});
	text.clear();
}

size_t Rows::bytes() const {
	return text.size() + size_t(count) * types.size() * 4;
}

size_t Rows::rowBytes(uint32_t row) const {
	size_t total = 0;
	for (size_t index = 0; index != types.size(); ++index) {
		if (types[index] == 's') {
			total += 1 + lengthBytes(lengths[index][row]) + lengths[index][row];
		} else {
			total += 5;
		}
	}
	return total;
}

Connection::Connection(int id, Settings settings)
	: id_(id)
	, settings_(std::move(settings)) {
}

Connection::~Connection() {
	stop();
}

void Connection::start() {
	if (started_) {
		return;
	}
	started_ = true;
	ordered_.ordered = true;
	ordered_.thread = std::thread([this]() {
		work(ordered_);
	});
	for (int index = 0; index < settings_.pool; ++index) {
		pool_.push_back(std::make_unique<Worker>());
		Worker* worker = pool_.back().get();
		worker->thread = std::thread([this, worker]() {
			work(*worker);
		});
	}
}

void Connection::submit(std::unique_ptr<Job> job) {
	job->queued = Clock::now();
	bool pooled = job->parallel && !pool_.empty();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		(pooled ? poolQueue_ : orderedQueue_).push_back(std::move(job));
	}
	(pooled ? poolWake_ : orderedWake_).notify_one();
}

std::unique_ptr<Job> Connection::run(std::unique_ptr<Job> job) {
	job->sync = true;
	job->parallel = false;
	Job* waiting = job.get();
	submit(std::move(job));
	std::unique_lock<std::mutex> lock(mutex_);
	finishedWake_.wait(lock, [waiting]() {
		return waiting->finished;
	});
	auto found = std::find_if(waited_.begin(), waited_.end(), [waiting](const std::unique_ptr<Job>& entry) {
		return entry.get() == waiting;
	});
	std::unique_ptr<Job> done = std::move(*found);
	waited_.erase(found);
	return done;
}

void Connection::collect(std::vector<std::unique_ptr<Job>>& jobs) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (finished_.empty()) {
		return;
	}
	for (std::unique_ptr<Job>& job : finished_) {
		jobs.push_back(std::move(job));
	}
	finished_.clear();
}

void Connection::stop() {
	if (!started_) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	orderedWake_.notify_all();
	poolWake_.notify_all();
	if (ordered_.thread.joinable()) {
		ordered_.thread.join();
	}
	for (std::unique_ptr<Worker>& worker : pool_) {
		if (worker->thread.joinable()) {
			worker->thread.join();
		}
	}
	started_ = false;
}

size_t Connection::pending() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return orderedQueue_.size() + poolQueue_.size() + running_;
}

int Connection::lastError() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return lastError_;
}

std::string Connection::lastMessage() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return lastMessage_;
}

std::string Connection::tls() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return tls_;
}

bool Connection::admit(size_t length, std::string& reason) {
	if (settings_.maxQueryLength != 0 && length > settings_.maxQueryLength) {
		reason = "it has " + std::to_string(length) + " bytes, more than max_query_length " + std::to_string(settings_.maxQueryLength);
		return false;
	}
	if (settings_.maxPending != 0 && pending() >= settings_.maxPending) {
		reason = std::to_string(settings_.maxPending) + " queries are already waiting (max_pending)";
		return false;
	}
	if (settings_.maxPerSecond != 0) {
		Clock::time_point now = Clock::now();
		while (!recent_.empty() && now - recent_.front() >= std::chrono::seconds(1)) {
			recent_.pop_front();
		}
		if (recent_.size() >= settings_.maxPerSecond) {
			reason = std::to_string(settings_.maxPerSecond) + " queries were sent in the last second (max_queries_per_second)";
			return false;
		}
		recent_.push_back(now);
	}
	return true;
}

void Connection::work(Worker& worker) {
	mysql_thread_init();
	std::deque<std::unique_ptr<Job>>& queue = worker.ordered ? orderedQueue_ : poolQueue_;
	std::condition_variable& wake = worker.ordered ? orderedWake_ : poolWake_;
	for (;;) {
		std::unique_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake.wait(lock, [this, &queue]() {
				return stopping_ || !queue.empty();
			});
			if (queue.empty()) {
				break;
			}
			job = std::move(queue.front());
			queue.pop_front();
			++running_;
		}
		execute(worker, *job);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			--running_;
			if (job->sync) {
				job->finished = true;
				waited_.push_back(std::move(job));
			} else {
				finished_.push_back(std::move(job));
			}
		}
		finishedWake_.notify_all();
	}
	close(worker);
	mysql_thread_end();
}

void Connection::execute(Worker& worker, Job& job) {
	job.started = Clock::now();
	job.result = std::make_unique<Result>();
	Result& result = *job.result;
	result.connection = id_;
	result.query = std::move(job.query);
	if (job.kind == JobKind::CloseStatement) {
		dropStatement(worker, job.statement);
	} else if (job.kind == JobKind::Prepare || job.kind == JobKind::Statement) {
		if (ensure(worker, result)) {
			if (job.kind == JobKind::Prepare) {
				prepared(worker, job, result);
			} else {
				runStatement(worker, job, result);
			}
			worker.lastUsed = Clock::now();
		}
	} else if (ensure(worker, result) && job.kind == JobKind::Query) {
		if (mysql_real_query(worker.mysql, result.query.data(), (unsigned long)result.query.size()) != 0) {
			fail(worker, result);
		} else {
			for (;;) {
				MYSQL_RES* rows = mysql_store_result(worker.mysql);
				if (rows == nullptr && mysql_field_count(worker.mysql) != 0) {
					fail(worker, result);
					break;
				}
				ResultSet& set = result.sets.emplace_back();
				set.latin1 = settings_.charset == "latin1";
				if (rows != nullptr) {
					copySet(rows, set);
					mysql_free_result(rows);
				}
				set.affectedRows = mysql_affected_rows(worker.mysql);
				set.insertId = mysql_insert_id(worker.mysql);
				set.warnings = mysql_warning_count(worker.mysql);
				int next = mysql_next_result(worker.mysql);
				if (next > 0) {
					fail(worker, result);
					break;
				}
				if (next != 0) {
					break;
				}
			}
		}
		if (worker.connected && result.error != 0 && mysql_more_results(worker.mysql)) {
			close(worker);
		}
		worker.lastUsed = Clock::now();
	}
	job.ended = Clock::now();
}

bool Connection::ensure(Worker& worker, Result& result) {
	Clock::time_point now = Clock::now();
	if (worker.connected) {
		if (now - worker.lastUsed < PingAfter || mysql_ping(worker.mysql) == 0) {
			return true;
		}
		close(worker);
	}
	if (now < worker.retryAt) {
		std::lock_guard<std::mutex> lock(mutex_);
		result.error = lastError_ != 0 ? lastError_ : CR_CONN_HOST_ERROR;
		result.message = lastMessage_.empty() ? std::string("not connected") : lastMessage_;
		return false;
	}
	return open(worker, result);
}

bool Connection::open(Worker& worker, Result& result) {
	close(worker);
	worker.mysql = mysql_init(nullptr);
	if (worker.mysql == nullptr) {
		result.error = CR_OUT_OF_MEMORY;
		result.message = "out of memory";
		return false;
	}
	MYSQL* mysql = worker.mysql;
	mysql_options(mysql, MYSQL_SET_CHARSET_NAME, settings_.charset.c_str());
	setSeconds(mysql, MYSQL_OPT_CONNECT_TIMEOUT, settings_.timeout);
	setSeconds(mysql, MYSQL_OPT_READ_TIMEOUT, settings_.readTimeout);
	setSeconds(mysql, MYSQL_OPT_WRITE_TIMEOUT, settings_.writeTimeout);
	my_bool verify = settings_.sslVerify ? 1 : 0;
	mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
	bool tls = settings_.tls();
	if (tls) {
		my_bool enforce = 1;
		mysql_options(mysql, MYSQL_OPT_SSL_ENFORCE, &enforce);
		setText(mysql, MYSQL_OPT_SSL_CA, settings_.sslCa);
		setText(mysql, MYSQL_OPT_SSL_CAPATH, settings_.sslCaPath);
		setText(mysql, MYSQL_OPT_SSL_CERT, settings_.sslCert);
		setText(mysql, MYSQL_OPT_SSL_KEY, settings_.sslKey);
		setText(mysql, MYSQL_OPT_SSL_CRL, settings_.sslCrl);
		setText(mysql, MYSQL_OPT_SSL_CRLPATH, settings_.sslCrlPath);
		setText(mysql, MYSQL_OPT_SSL_CIPHER, settings_.sslCipher);
		setText(mysql, MARIADB_OPT_TLS_VERSION, settings_.tlsVersion);
		setText(mysql, MARIADB_OPT_TLS_PEER_FP, settings_.sslFingerprint);
	}
	unsigned long flags = CLIENT_MULTI_RESULTS | (settings_.multiStatements ? CLIENT_MULTI_STATEMENTS : 0);
	const char* database = settings_.database.empty() ? nullptr : settings_.database.c_str();
	if (mysql_real_connect(mysql, settings_.host.c_str(), settings_.user.c_str(), settings_.password.c_str(), database, settings_.port, nullptr, flags) == nullptr) {
		result.error = int(mysql_errno(mysql));
		result.message = mysql_error(mysql);
		result.sqlstate = mysql_sqlstate(mysql);
		return failOpen(worker, result);
	}
	const char* cipher = mysql_get_ssl_cipher(mysql);
	if (tls && cipher == nullptr) {
		result.error = CR_SSL_CONNECTION_ERROR;
		result.message = "TLS is required, but the server answered without it";
		return failOpen(worker, result);
	}
	if (cipher != nullptr) {
		const char* version = nullptr;
		mariadb_get_infov(mysql, MARIADB_CONNECTION_TLS_VERSION, &version);
		result.tls = std::string(version != nullptr ? version : "TLS") + " " + (*cipher != '\0' ? cipher : "with an unnamed cipher");
	}
	worker.connected = true;
	worker.failures = 0;
	worker.lastUsed = Clock::now();
	result.server = mysql_get_server_info(mysql);
	if (worker.ordered) {
		bool backslash = true;
		if (mysql_query(worker.mysql, "SELECT @@SESSION.sql_mode, @@SESSION.max_allowed_packet") == 0) {
			MYSQL_RES* modes = mysql_store_result(worker.mysql);
			if (modes != nullptr) {
				MYSQL_ROW row = mysql_fetch_row(modes);
				if (row != nullptr && row[0] != nullptr && std::strstr(row[0], "NO_BACKSLASH_ESCAPES") != nullptr) {
					backslash = false;
				}
				if (row != nullptr && row[1] != nullptr) {
					worker.maxPacket = std::strtoul(row[1], nullptr, 10);
				}
				mysql_free_result(modes);
			}
		}
		backslash_.store(backslash, std::memory_order_relaxed);
		connected_.store(true, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(mutex_);
		lastError_ = 0;
		lastMessage_.clear();
		tls_ = result.tls;
	}
	return true;
}

bool Connection::failOpen(Worker& worker, Result& result) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		lastError_ = result.error;
		lastMessage_ = result.message;
	}
	close(worker);
	worker.failures = std::min(worker.failures + 1, 8);
	worker.retryAt = Clock::now() + std::min<Clock::duration>(RetryFirst * (1 << (worker.failures - 1)), RetryLongest);
	return false;
}

void Connection::close(Worker& worker) {
	for (auto& entry : worker.statements) {
		mysql_stmt_close(entry.second);
	}
	worker.statements.clear();
	if (worker.mysql != nullptr) {
		mysql_close(worker.mysql);
		worker.mysql = nullptr;
	}
	if (worker.ordered) {
		connected_.store(false, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(mutex_);
		tls_.clear();
	}
	worker.connected = false;
}

void Connection::fail(Worker& worker, Result& result) {
	unsigned code = mysql_errno(worker.mysql);
	result.error = int(code);
	result.message = mysql_error(worker.mysql);
	result.sqlstate = mysql_sqlstate(worker.mysql);
	if (lostConnection(code)) {
		close(worker);
	}
}

void Connection::failStatement(Worker& worker, MYSQL_STMT* statement, Result& result) {
	unsigned code = mysql_stmt_errno(statement);
	result.error = int(code);
	result.message = mysql_stmt_error(statement);
	result.sqlstate = mysql_stmt_sqlstate(statement);
	if (lostConnection(code)) {
		close(worker);
	}
}

MYSQL_STMT* Connection::prepared(Worker& worker, Job& job, Result& result) {
	auto found = worker.statements.find(job.statement);
	if (found != worker.statements.end()) {
		return found->second;
	}
	MYSQL_STMT* statement = mysql_stmt_init(worker.mysql);
	if (statement == nullptr) {
		result.error = CR_OUT_OF_MEMORY;
		result.message = "out of memory";
		return nullptr;
	}
	if (mysql_stmt_prepare(statement, result.query.data(), (unsigned long)result.query.size()) != 0) {
		unsigned code = mysql_stmt_errno(statement);
		result.error = int(code);
		result.message = mysql_stmt_error(statement);
		result.sqlstate = mysql_stmt_sqlstate(statement);
		mysql_stmt_close(statement);
		if (lostConnection(code)) {
			close(worker);
		}
		return nullptr;
	}
	size_t values = job.rows ? job.rows->types.size() : 0;
	if (mysql_stmt_param_count(statement) != values) {
		result.error = CR_INVALID_PARAMETER_NO;
		result.message = "the statement has " + std::to_string(mysql_stmt_param_count(statement)) + " placeholders for " + std::to_string(values) + " values, so it has a ? of its own";
		mysql_stmt_close(statement);
		return nullptr;
	}
	worker.statements.emplace(job.statement, statement);
	return statement;
}

void Connection::dropStatement(Worker& worker, uint64_t key) {
	auto found = worker.statements.find(key);
	if (found == worker.statements.end()) {
		return;
	}
	mysql_stmt_close(found->second);
	worker.statements.erase(found);
}

void Connection::runStatement(Worker& worker, Job& job, Result& result) {
	for (int attempt = 0; attempt != 2; ++attempt) {
		MYSQL_STMT* statement = prepared(worker, job, result);
		if (statement == nullptr) {
			return;
		}
		bool done = job.batch ? executeBatch(worker, statement, *job.rows, result) : executeRow(worker, statement, *job.rows, 0, result) && fetchSets(worker, statement, result);
		if (done || result.error != ER_NEED_REPREPARE || !worker.connected) {
			return;
		}
		dropStatement(worker, job.statement);
		result.sets.clear();
		result.error = 0;
		result.message.clear();
		result.sqlstate.clear();
	}
}

bool Connection::executeRow(Worker& worker, MYSQL_STMT* statement, Rows& rows, uint32_t row, Result& result) {
	std::vector<std::vector<char*>> pointers(rows.types.size());
	for (size_t index = 0; index != rows.types.size(); ++index) {
		if (rows.types[index] == 's') {
			pointers[index].assign(rows.count, nullptr);
			pointers[index][row] = rows.text.data() + rows.offsets[index][row];
		}
	}
	std::vector<MYSQL_BIND> binds;
	bindRows(rows, row, false, pointers, binds);
	unsigned int size = 0;
	mysql_stmt_attr_set(statement, STMT_ATTR_ARRAY_SIZE, &size);
	if ((!binds.empty() && mysql_stmt_bind_param(statement, binds.data()) != 0) || mysql_stmt_execute(statement) != 0) {
		failStatement(worker, statement, result);
		return false;
	}
	return true;
}

bool Connection::fetchSets(Worker& worker, MYSQL_STMT* statement, Result& result) {
	for (;;) {
		ResultSet& set = result.sets.emplace_back();
		set.latin1 = settings_.charset == "latin1";
		MYSQL_RES* meta = mysql_stmt_result_metadata(statement);
		if (meta != nullptr) {
			my_bool update = 1;
			mysql_stmt_attr_set(statement, STMT_ATTR_UPDATE_MAX_LENGTH, &update);
			if (mysql_stmt_store_result(statement) != 0) {
				mysql_free_result(meta);
				failStatement(worker, statement, result);
				return false;
			}
			copyStatementSet(statement, meta, set);
			mysql_free_result(meta);
		}
		set.affectedRows = mysql_stmt_affected_rows(statement);
		set.insertId = mysql_stmt_insert_id(statement);
		set.warnings = mysql_warning_count(worker.mysql);
		mysql_stmt_free_result(statement);
		int next = mysql_stmt_next_result(statement);
		if (next > 0) {
			failStatement(worker, statement, result);
			return false;
		}
		if (next != 0) {
			return true;
		}
	}
}

bool Connection::executeBatch(Worker& worker, MYSQL_STMT* statement, Rows& rows, Result& result) {
	ResultSet& set = result.sets.emplace_back();
	set.latin1 = settings_.charset == "latin1";
	if (rows.count == 0) {
		return true;
	}
	bool bulk = bulkSupported(worker.mysql);
	size_t limit = worker.maxPacket > 4096 ? worker.maxPacket - 1024 : worker.maxPacket;
	size_t header = 16 + rows.types.size() * 2;
	std::vector<std::pair<uint32_t, uint32_t>> chunks;
	uint32_t first = 0;
	size_t bytes = header;
	for (uint32_t row = 0; row != rows.count; ++row) {
		size_t size = rows.rowBytes(row);
		if (row != first && (!bulk || bytes + size > limit)) {
			chunks.emplace_back(first, row - first);
			first = row;
			bytes = header;
		}
		bytes += size;
	}
	chunks.emplace_back(first, rows.count - first);
	bool wrap = chunks.size() > 1 && ownTransaction(worker.mysql);
	if (wrap && mysql_query(worker.mysql, "START TRANSACTION") != 0) {
		fail(worker, result);
		return false;
	}
	std::vector<std::vector<char*>> pointers(rows.types.size());
	for (size_t index = 0; index != rows.types.size(); ++index) {
		if (rows.types[index] != 's') {
			continue;
		}
		pointers[index].resize(rows.count);
		for (uint32_t row = 0; row != rows.count; ++row) {
			pointers[index][row] = rows.text.data() + rows.offsets[index][row];
		}
	}
	std::vector<MYSQL_BIND> binds;
	bool firstChunk = true;
	for (const auto& chunk : chunks) {
		unsigned int size = bulk ? chunk.second : 0;
		mysql_stmt_attr_set(statement, STMT_ATTR_ARRAY_SIZE, &size);
		bindRows(rows, chunk.first, bulk, pointers, binds);
		if ((!binds.empty() && mysql_stmt_bind_param(statement, binds.data()) != 0) || mysql_stmt_execute(statement) != 0) {
			failStatement(worker, statement, result);
			if (wrap && worker.connected) {
				mysql_query(worker.mysql, "ROLLBACK");
			}
			return false;
		}
		set.affectedRows += mysql_stmt_affected_rows(statement);
		if (firstChunk) {
			set.insertId = mysql_stmt_insert_id(statement);
			firstChunk = false;
		}
	}
	if (wrap && mysql_query(worker.mysql, "COMMIT") != 0) {
		fail(worker, result);
		return false;
	}
	set.warnings = mysql_warning_count(worker.mysql);
	return true;
}

}
