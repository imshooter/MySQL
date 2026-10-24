#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

typedef struct st_mysql MYSQL;
typedef struct st_mysql_stmt MYSQL_STMT;
struct yyjson_doc;

namespace mysqlcomponent {

using Clock = std::chrono::steady_clock;

/**
 * @brief      How to reach a database: the server, the account, the default database, the character
 *             set of the text the scripts send and read, whether a query may hold several statements,
 *             how many connections answer parallel queries, the connect, read and write timeouts in
 *             seconds, the time in milliseconds from which a query is logged as slow, whether errors
 *             are logged, the limits a query must pass to be sent (its length, the queries waiting,
 *             the queries in the last second, `0` for none), and the TLS of the connection: whether
 *             it is required, whether the certificate of the server is verified, the certificate
 *             authorities and revocation lists, the certificate and key of the client, the ciphers,
 *             the versions and the fingerprint the certificate of the server must have.
 */
struct Settings {
	std::string host = "127.0.0.1";
	std::string user = "root";
	std::string password;
	std::string database;
	unsigned port = 3306;
	std::string charset = "latin1";
	bool multiStatements = false;
	int pool = 0;
	unsigned timeout = 5;
	unsigned readTimeout = 0;
	unsigned writeTimeout = 0;
	double slowQuery = 0.0;
	bool logErrors = true;
	size_t maxQueryLength = 0;
	size_t maxPending = 0;
	size_t maxPerSecond = 0;
	bool ssl = false;
	bool sslVerify = false;
	std::string sslCa;
	std::string sslCaPath;
	std::string sslCert;
	std::string sslKey;
	std::string sslCrl;
	std::string sslCrlPath;
	std::string sslCipher;
	std::string tlsVersion;
	std::string sslFingerprint;

	bool tls() const {
		return ssl || sslVerify || !sslCa.empty() || !sslCaPath.empty() || !sslCert.empty() || !sslKey.empty() || !sslCrl.empty() || !sslCrlPath.empty() || !sslCipher.empty() || !tlsVersion.empty() || !sslFingerprint.empty();
	}
};

/**
 * @brief      A column of a result: its name as the query gave it and its type in the protocol.
 */
struct Column {
	std::string name;
	uint32_t type = 0;
};

/**
 * @brief      A JSON cell parsed when a script first reads it, with the keys of its object in order.
 */
struct JsonCell {
	yyjson_doc* doc = nullptr;
	JsonCell() = default;
	JsonCell(const JsonCell&) = delete;
	JsonCell& operator=(const JsonCell&) = delete;
	~JsonCell();
};

constexpr uint32_t NullLength = UINT32_MAX;

/**
 * @brief      The rows of one statement, copied out of the client library on the worker thread: the
 *             cells of every row one after the other in `data`, each ended by a zero byte, with its
 *             offset and length, or `NullLength` for NULL. `names` takes a lower case column name to
 *             its index. `latin1` tells that the text is in Windows-1252, which JSON is converted
 *             from and to.
 */
struct ResultSet {
	std::vector<Column> columns;
	std::unordered_map<std::string, uint32_t> names;
	uint32_t rows = 0;
	std::vector<uint32_t> offsets;
	std::vector<uint32_t> lengths;
	std::string data;
	uint64_t affectedRows = 0;
	uint64_t insertId = 0;
	uint32_t warnings = 0;
	bool latin1 = false;
	std::vector<std::unique_ptr<JsonCell>> json;

	ResultSet() = default;
	ResultSet(const ResultSet&) = delete;
	ResultSet& operator=(const ResultSet&) = delete;
	ResultSet(ResultSet&&) = default;
	ResultSet& operator=(ResultSet&&) = default;

	int find(std::string_view name) const;
	bool cell(uint32_t row, uint32_t column, const char*& text, uint32_t& length) const;
};

/**
 * @brief      What a query gave: one set for each statement that ran, the set the script reads, the
 *             error of the statement that failed, the text of the query, and the milliseconds it
 *             waited in the queue and took to run. A connect fills `server` and `tls`, the TLS
 *             version and cipher, or empty when the link is not encrypted.
 */
struct Result {
	std::vector<ResultSet> sets;
	uint32_t set = 0;
	int error = 0;
	std::string message;
	std::string sqlstate;
	std::string query;
	double wait = 0.0;
	double time = 0.0;
	int connection = 0;
	std::string server;
	std::string tls;
};

enum class JobKind {
	Query,
	Connect,
	Prepare,
	Statement,
	CloseStatement
};

/**
 * @brief      The values of a prepared statement for any number of rows, one column for each
 *             placeholder. `types` has a letter for each: `i` an integer, `u` an unsigned one, `f` a
 *             float, `s` a text. A column keeps its numbers in `integers` or `floats`, or its texts as
 *             offsets and lengths into `text`, so a batch is bound as arrays.
 */
struct Rows {
	uint32_t count = 0;
	std::string types;
	std::vector<std::vector<int32_t>> integers;
	std::vector<std::vector<float>> floats;
	std::vector<std::vector<uint32_t>> offsets;
	std::vector<std::vector<unsigned long>> lengths;
	std::string text;

	void init(const std::string& kinds);
	size_t bytes() const;
	size_t rowBytes(uint32_t row) const;
};

/**
 * @brief      A query on its way: the script that sent it, the public to call with `context` when it
 *             is done, whether any connection of the pool may run it, and whether the game thread waits
 *             for it. A prepared statement carries the key its connection keeps it by, in `statement`,
 *             its text in `query`, its values in `rows`, and whether they are a batch.
 */
struct Job {
	JobKind kind = JobKind::Query;
	AMX* amx = nullptr;
	uint64_t owner = 0;
	int connection = 0;
	std::string query;
	std::string callback;
	cell context = 0;
	bool parallel = false;
	bool sync = false;
	bool finished = false;
	uint64_t statement = 0;
	bool batch = false;
	std::unique_ptr<Rows> rows;
	Clock::time_point queued;
	Clock::time_point started;
	Clock::time_point ended;
	std::unique_ptr<Result> result;
};

/**
 * @brief      A database handle: one connection that runs the ordered queries one after another, and a
 *             pool of connections that share the parallel ones. Every connection lives on a thread of
 *             its own and connects again when it was lost. Finished queries wait in `finished_` until
 *             the game thread collects them.
 */
class Connection {
public:
	Connection(int id, Settings settings);
	~Connection();
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	void start();
	std::unique_ptr<Job> run(std::unique_ptr<Job> job);
	void submit(std::unique_ptr<Job> job);
	void collect(std::vector<std::unique_ptr<Job>>& jobs);
	void stop();

	int id() const { return id_; }
	const Settings& settings() const { return settings_; }
	bool backslashEscapes() const { return backslash_.load(std::memory_order_relaxed); }
	bool connected() const { return connected_.load(std::memory_order_relaxed); }
	size_t pending() const;
	int lastError() const;
	std::string lastMessage() const;
	std::string tls() const;
	bool admit(size_t length, std::string& reason);

	uint64_t sent = 0;
	uint64_t done = 0;
	uint64_t failed = 0;
	uint64_t refused = 0;
	double totalTime = 0.0;
	double longest = 0.0;
	Clock::time_point refusalLogged;
	uint64_t refusalsUnlogged = 0;

private:
	struct Worker {
		std::thread thread;
		MYSQL* mysql = nullptr;
		bool connected = false;
		bool ordered = false;
		Clock::time_point lastUsed;
		Clock::time_point retryAt;
		int failures = 0;
		unsigned long maxPacket = 1048576;
		std::unordered_map<uint64_t, MYSQL_STMT*> statements;
	};

	void work(Worker& worker);
	void execute(Worker& worker, Job& job);
	bool ensure(Worker& worker, Result& result);
	bool open(Worker& worker, Result& result);
	bool failOpen(Worker& worker, Result& result);
	void close(Worker& worker);
	void fail(Worker& worker, Result& result);
	MYSQL_STMT* prepared(Worker& worker, Job& job, Result& result);
	void dropStatement(Worker& worker, uint64_t key);
	void runStatement(Worker& worker, Job& job, Result& result);
	bool executeRow(Worker& worker, MYSQL_STMT* statement, Rows& rows, uint32_t row, Result& result);
	bool executeBatch(Worker& worker, MYSQL_STMT* statement, Rows& rows, Result& result);
	bool fetchSets(Worker& worker, MYSQL_STMT* statement, Result& result);
	void failStatement(Worker& worker, MYSQL_STMT* statement, Result& result);

	int id_;
	Settings settings_;
	Worker ordered_;
	std::vector<std::unique_ptr<Worker>> pool_;
	mutable std::mutex mutex_;
	std::condition_variable orderedWake_;
	std::condition_variable poolWake_;
	std::condition_variable finishedWake_;
	std::deque<std::unique_ptr<Job>> orderedQueue_;
	std::deque<std::unique_ptr<Job>> poolQueue_;
	std::vector<std::unique_ptr<Job>> finished_;
	std::vector<std::unique_ptr<Job>> waited_;
	size_t running_ = 0;
	bool stopping_ = false;
	bool started_ = false;
	std::atomic<bool> backslash_{ true };
	std::atomic<bool> connected_{ false };
	int lastError_ = 0;
	std::string lastMessage_;
	std::string tls_;
	std::deque<Clock::time_point> recent_;
};

/**
 * @brief      The result a callback reads: its own, or one `MySQL_Run` or `MySQL_UseResult` chose
 *             while it runs. `owner` is the pointer that owns it, so `MySQL_KeepResult` can take it
 *             over, and `original` is the result of the callback itself.
 */
struct ActiveResult {
	Result* result = nullptr;
	std::unique_ptr<Result>* owner = nullptr;
	Result* original = nullptr;
	std::unique_ptr<Result>* originalOwner = nullptr;
};

/**
 * @brief      A query a script builds a piece at a time before sending it.
 */
struct Builder {
	int connection = 0;
	std::string text;
};

/**
 * @brief      A prepared statement of a script: its connection, the key the connection keeps it by,
 *             its text with a `?` for each value, and the type of each value.
 */
struct PreparedStatement {
	int connection = 0;
	uint64_t key = 0;
	std::string sql;
	std::string types;
};

/**
 * @brief      Rows of values a script gathers for one prepared statement before sending them.
 */
struct Batch {
	int statement = 0;
	Rows rows;
};

/**
 * @brief      What the component keeps for a script: the results of the callbacks running, the result
 *             of its last `MySQL_Run`, the result `MySQL_UseResult` chose, its kept results, its
 *             queries being built, its prepared statements and the batches it gathers.
 */
struct Script {
	IPawnScript* api = nullptr;
	uint64_t serial = 0;
	std::vector<ActiveResult> active;
	std::unique_ptr<Result> last;
	Result* used = nullptr;
	std::unordered_map<int, std::unique_ptr<Result>> kept;
	std::unordered_map<int, Builder> builders;
	std::unordered_map<int, PreparedStatement> statements;
	std::unordered_map<int, Batch> batches;
	int nextResult = 1;
	int nextBuilder = 1;
	int nextStatement = 1;
	int nextBatch = 1;
	std::unordered_set<std::string> missing;
};

enum class PasswordWork {
	Argon2,
	Bcrypt,
	Verify
};

/**
 * @brief      A password to hash or verify on a thread of its own: the script that asked, the public
 *             to call with `context`, the costs of the hash, and what came back, the hash, whether it
 *             matched, or the error.
 */
struct PasswordJob {
	PasswordWork work = PasswordWork::Argon2;
	AMX* amx = nullptr;
	uint64_t owner = 0;
	std::string password;
	std::string hash;
	std::string callback;
	cell context = 0;
	uint32_t memory = 0;
	uint32_t iterations = 0;
	uint32_t parallelism = 0;
	uint32_t cost = 0;
	bool match = false;
	std::string error;
};

/**
 * @brief      The threads that hash and verify passwords, started with the first password. Finished
 *             work waits in `finished_` until the game thread collects it.
 */
class Passwords {
public:
	Passwords() = default;
	~Passwords();
	Passwords(const Passwords&) = delete;
	Passwords& operator=(const Passwords&) = delete;

	void submit(std::unique_ptr<PasswordJob> job);
	void collect(std::vector<std::unique_ptr<PasswordJob>>& jobs);
	void stop();

private:
	void work();

	std::vector<std::thread> threads_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::unique_ptr<PasswordJob>> queue_;
	std::vector<std::unique_ptr<PasswordJob>> finished_;
	bool stopping_ = false;
};

struct State {
	ICore* core = nullptr;
	IPawnComponent* pawn = nullptr;
	std::vector<std::unique_ptr<Connection>> connections;
	std::vector<AMX*> owners;
	std::unordered_map<AMX*, Script> scripts;
	AMX* lastAmx = nullptr;
	Script* lastScript = nullptr;
	uint64_t serial = 0;
	uint64_t statementSerial = 0;
	Passwords passwords;
};

State& state();
Script* scriptOf(AMX* amx);
bool findPublic(Script& script, const char* name, int& index);
Connection* connectionOf(cell handle);
Result* currentResult(AMX* amx);
ResultSet* currentSet(AMX* amx);
void log(const char* format, ...);
void logError(const char* format, ...);
void report(Connection& connection, Job& job);
void reportRefusal(Connection& connection, const std::string& reason);
void callError(Script& script, Job& job);

int openConnection(AMX* amx, Settings settings);
bool closeConnection(int id);
void dispatch(std::vector<std::unique_ptr<Job>>& jobs);
void dispatchPasswords(std::vector<std::unique_ptr<PasswordJob>>& jobs);
void forgetScript(AMX* amx);
void closeAll();

bool readString(AMX* amx, cell address, std::string& output);
int writeString(AMX* amx, cell address, std::string_view text, cell size);
cell* addressOf(AMX* amx, cell address);
bool formatQuery(AMX* amx, const cell* params, int format, int first, bool backslash, std::string& output, std::string& error);
bool parseStatement(std::string_view pattern, std::string& sql, std::string& types, std::string& error);
void appendEscaped(std::string& output, std::string_view text, bool backslash);
void appendJson(std::string& output, std::string_view text);
bool readSettingsFile(const std::string& path, Settings& settings, std::string& error);
bool readOptions(std::string_view options, Settings& settings, std::string& error);

JsonCell* jsonOf(ResultSet& set, uint32_t row, uint32_t column);
std::string_view latin1ToUtf8(std::string_view text, std::string& buffer);
std::string_view utf8ToLatin1(std::string_view text, std::string& buffer);

void registerNatives(IPawnScript& script);

}
