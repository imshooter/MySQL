#include <open.mp>
#include <mysql>
#include "test.inc"

static const gTests[][MAX_TEST_NAME] = {
    "Connects",
    "ReadsCells",
    "TruncatesStrings",
    "EscapesText",
    "KeepsLatin1",
    "RunsInOrder",
    "FailureCallsBack",
    "RunsInParallel",
    "ReadsResultSets",
    "BuildsLongQueries",
    "JoinsBuiltQueries",
    "ReadsRows",
    "ReadsJSON",
    "ReadsLatin1JSON",
    "KeepsResults",
    "UsesKeptResults",
    "RunsInsideCallbacks",
    "FormatsIntoArrays",
    "RefusesBadQueries",
    "CloseRunsWhatWaits",
    "CountsQueries"
};

/**
 * Queries, results, JSON cells and built queries. The tests make the table mysql_queries_test
 * and drop it when they end.
 */

static
    MySQL:gDatabase = INVALID_MYSQL_ID,
    gErrors,
    gLastError,
    gAnswered,
    Result:gKept = INVALID_RESULT_ID
;

forward OnOrdered(step);
forward OnFailed(context);
forward OnParallel(step);
forward OnBatch(rows);
forward OnKept(context);
forward OnRunInside(context);
forward OnCounted(expected);

public OnGameModeInit() {
    gDatabase = MySQL_Connect(TEST_HOST, TEST_USER, TEST_PASSWORD, TEST_DATABASE, TEST_PORT, "multi_statements=true;pool=2");
    MySQL_Run(gDatabase, "DROP TABLE IF EXISTS mysql_queries_test");
    MySQL_Run(gDatabase, "CREATE TABLE mysql_queries_test (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64) NOT NULL, amount INT NULL) DEFAULT CHARSET=utf8mb4");
    Test_Run(gTests);

    return 1;
}

public OnMySQLError(MySQL:handle, errorid, const error[], const query[], const callback[]) {
    ++gErrors;
    gLastError = errorid;

    return 1;
}

test Connects() {
    ASSERT_NE(gDatabase, INVALID_MYSQL_ID);
    ASSERT(MySQL_IsConnected(gDatabase));
    ASSERT_EQ(MySQL_GetConnectionErrno(gDatabase), 0);
    ASSERT_EQ(MySQL_Connect(TEST_HOST, TEST_USER, TEST_PASSWORD, TEST_DATABASE, TEST_PORT, "nonsense=1"), INVALID_MYSQL_ID);
}

test ReadsCells() {
    new
        text[16]
    ;

    ASSERT(MySQL_Run(gDatabase, "SELECT %i + %i AS sum, 'text' AS word, NULL AS nothing, %f AS half, %.2f AS fixed", 1, 2, 1.5, 2.345));
    ASSERT_EQ(MySQL_GetErrno(), 0);
    ASSERT_EQ(MySQL_GetRowCount(), 1);
    ASSERT_EQ(MySQL_GetFieldCount(), 5);
    ASSERT_EQ(MySQL_GetInt(0, "sum"), 3);
    ASSERT(MySQL_GetString(0, "word", text));
    ASSERT_SAME(text, "text");
    ASSERT(MySQL_IsNull(0, "nothing"));
    ASSERT_FALSE(MySQL_GetString(0, "nothing", text));
    ASSERT_SAME(text, "");
    ASSERT_NEAR(MySQL_GetFloat(0, "half"), 1.5, 0.0001);
    MySQL_GetString(0, "fixed", text);
    ASSERT_SAME(text, "2.35");
    MySQL_GetFieldName(1, text);
    ASSERT_SAME(text, "word");
    ASSERT_EQ(MySQL_GetFieldIndex("WORD"), 1);
    ASSERT_EQ(MySQL_GetFieldIndex("missing"), -1);
}

test TruncatesStrings() {
    new
        text[4]
    ;

    ASSERT(MySQL_Run(gDatabase, "SELECT 'abcdef' AS word"));
    ASSERT(MySQL_GetString(0, "word", text));
    ASSERT_SAME(text, "abc");
}

test EscapesText() {
    new const
        name[] = "O'Brien \"%d\" back\\slash"
    ;

    new
        stored[64]
    ;

    ASSERT(MySQL_Run(gDatabase, "INSERT INTO mysql_queries_test (name, amount) VALUES ('%e', 1)", name));
    ASSERT_GT(MySQL_GetInsertID(), 0);
    ASSERT_EQ(MySQL_GetAffectedRows(), 1);
    ASSERT(MySQL_Run(gDatabase, "SELECT name FROM mysql_queries_test WHERE name = %q", name));
    ASSERT_EQ(MySQL_GetRowCount(), 1);
    MySQL_GetString(0, "name", stored);
    ASSERT_SAME(stored, name);
}

test KeepsLatin1() {
    new const
        name[] = "a\xE7;\xE3;\x80;b"
    ;

    new
        stored[16]
    ;

    ASSERT(MySQL_Run(gDatabase, "INSERT INTO mysql_queries_test (name) VALUES (%q)", name));
    ASSERT(MySQL_Run(gDatabase, "SELECT name, CHAR_LENGTH(name) AS characters FROM mysql_queries_test WHERE id = %i", MySQL_GetInsertID()));
    MySQL_GetString(0, "name", stored);
    ASSERT_SAME(stored, name);
    ASSERT_EQ(MySQL_GetInt(0, "characters"), 5);
}

test RunsInOrder() {
    Test_Async(3000);
    ASSERT(MySQL_Query(gDatabase, "OnOrdered", 1, "SELECT SLEEP(%f)", 0.2));
    ASSERT(MySQL_Query(gDatabase, "OnOrdered", 2, "SELECT 2"));
    ASSERT(MySQL_Query(gDatabase, "OnOrdered", 3, "SELECT 3"));
}

public OnOrdered(step) {
    ASSERT_EQ(MySQL_GetErrno(), 0);
    Test_Log(step);

    if (step == 3) {
        ASSERT_LOG({1, 2, 3});
        Test_Done();
    }

    return 1;
}

test FailureCallsBack() {
    gErrors = 0;
    Test_Async(3000);
    ASSERT(MySQL_Query(gDatabase, "OnFailed", 7, "SELEKT 1"));
}

public OnFailed(context) {
    new
        error[128]
    ;

    ASSERT_EQ(context, 7);
    ASSERT_EQ(MySQL_GetErrno(), 1064);
    ASSERT(MySQL_GetError(error));
    ASSERT_NE(error[0], EOS);
    ASSERT_EQ(MySQL_GetRowCount(), 0);
    ASSERT_EQ(gErrors, 1);
    ASSERT_EQ(gLastError, 1064);
    Test_Done();

    return 1;
}

test RunsInParallel() {
    gAnswered = 0;
    Test_Async(3000);
    ASSERT(MySQL_ParallelQuery(gDatabase, "OnParallel", 1, "SELECT SLEEP(0.3)"));
    ASSERT(MySQL_ParallelQuery(gDatabase, "OnParallel", 2, "SELECT 2"));
}

public OnParallel(step) {
    Test_Log(step);

    if (++gAnswered == 2) {
        ASSERT_LOG({2, 1});
        Test_Done();
    }

    return 1;
}

test ReadsResultSets() {
    ASSERT(MySQL_Run(gDatabase, "SELECT 1 AS a; SELECT 2 AS b, 3 AS c"));
    ASSERT_EQ(MySQL_GetResultSetCount(), 2);
    ASSERT_EQ(MySQL_GetInt(0, "a"), 1);
    ASSERT(MySQL_SelectResultSet(1));
    ASSERT_EQ(MySQL_GetFieldCount(), 2);
    ASSERT_EQ(MySQL_GetInt(0, "b"), 2);
    ASSERT_EQ(MySQL_GetInt(0, "c"), 3);
    ASSERT_FALSE(MySQL_SelectResultSet(2));
}

test BuildsLongQueries() {
    new const
        Query:query = MySQL_CreateQuery(gDatabase, "INSERT INTO mysql_queries_test (name, amount) VALUES ")
    ;

    ASSERT_NE(query, INVALID_QUERY_ID);

    for (new i; i != 2000; ++i) {
        MySQL_AppendQuery(query, "('row %i it''s', %i), ", i, i);
    }

    ASSERT(MySQL_TrimQuery(query, 2));
    ASSERT_GT(MySQL_GetQueryLength(query), 30000);
    Test_Async(5000);
    ASSERT(MySQL_SendQuery(query, "OnBatch", 2000));
    ASSERT_EQ(MySQL_GetQueryLength(query), -1);
}

public OnBatch(rows) {
    ASSERT_EQ(MySQL_GetErrno(), 0);
    ASSERT_EQ(MySQL_GetAffectedRows(), rows);
    Test_Done();

    return 1;
}

test JoinsBuiltQueries() {
    new const
        Query:rows = MySQL_CreateQuery(gDatabase, "SELECT 1 AS a"),
        Query:whole = MySQL_CreateQuery(gDatabase)
    ;

    MySQL_AppendQuery(rows, " UNION ALL SELECT %i", 2);
    ASSERT(MySQL_AppendQuery(whole, "SELECT SUM(a) AS total FROM (%Q) AS joined", rows));
    ASSERT(MySQL_DestroyQuery(rows));
    ASSERT_FALSE(MySQL_AppendQuery(whole, " %Q", rows));
    ASSERT(MySQL_RunQuery(whole));
    ASSERT_EQ(MySQL_GetInt(0, "total"), 3);
}

test ReadsRows() {
    new
        uuid[16],
        slot,
        Float:price,
        bool:flag,
        name[8]
    ;

    ASSERT(MySQL_Run(gDatabase, "SELECT 'uuid-1' AS uuid, 7 AS slot, 2.5 AS price, 1 AS flag, 'skip' AS skipped, 'named' AS name"));
    ASSERT_EQ(MySQL_GetRow(0, "s[16] i f b - s[8]", uuid, slot, price, flag, name), 5);
    ASSERT_SAME(uuid, "uuid-1");
    ASSERT_EQ(slot, 7);
    ASSERT_NEAR(price, 2.5, 0.0001);
    ASSERT(flag);
    ASSERT_SAME(name, "named");
}

test ReadsJSON() {
    new
        text[32]
    ;

    ASSERT(MySQL_Run(gDatabase, "SELECT '{\"name\":\"Bag\",\"amount\":3,\"price\":1.5,\"ok\":true,\"pos\":[1,2,3],\"inner\":{\"x\":9}}' AS data, 'not json' AS plain"));
    ASSERT_EQ(MySQL_GetJSONType(0, "data"), MYSQL_JSON_OBJECT);
    ASSERT_EQ(MySQL_GetJSONCount(0, "data"), 6);
    ASSERT(MySQL_GetJSONKey(0, "data", 0, text));
    ASSERT_SAME(text, "name");
    ASSERT(MySQL_GetJSONKey(0, "data", 5, text));
    ASSERT_SAME(text, "inner");
    ASSERT_FALSE(MySQL_GetJSONKey(0, "data", 6, text));
    ASSERT(MySQL_GetJSONString(0, "data", "name", text));
    ASSERT_SAME(text, "Bag");
    ASSERT_EQ(MySQL_GetJSONInt(0, "data", "amount"), 3);
    ASSERT_NEAR(MySQL_GetJSONFloat(0, "data", "price"), 1.5, 0.0001);
    ASSERT(MySQL_GetJSONBool(0, "data", "ok"));
    ASSERT_EQ(MySQL_GetJSONType(0, "data", "pos"), MYSQL_JSON_ARRAY);
    ASSERT_EQ(MySQL_GetJSONCount(0, "data", "pos"), 3);
    ASSERT_EQ(MySQL_GetJSONInt(0, "data", "/pos/1"), 2);
    ASSERT_EQ(MySQL_GetJSONInt(0, "data", "/inner/x"), 9);
    ASSERT(MySQL_GetJSONString(0, "data", "inner", text));
    ASSERT_SAME(text, "{\"x\":9}");
    ASSERT_EQ(MySQL_GetJSONType(0, "data", "missing"), MYSQL_JSON_NONE);
    ASSERT_EQ(MySQL_GetJSONType(0, "plain"), MYSQL_JSON_NONE);
    ASSERT_EQ(MySQL_GetJSONInt(0, "data", "missing", 7), 7);
    ASSERT_NEAR(MySQL_GetJSONFloat(0, "data", "missing", 2.5), 2.5, 0.0001);
    ASSERT(MySQL_GetJSONBool(0, "data", "missing", true));
    ASSERT_EQ(MySQL_GetJSONInt(0, "data", "amount", 7), 3);
}

test ReadsLatin1JSON() {
    new const
        name[] = "a\xE7;\xE3;\x80;b",
        key[] = "ma\xE7;\xE3;",
        json[] = "{\"ma\xE7;\xE3;\":\"a\xE7;\xE3;\x80;b\",\"escaped\":\"\\u00e7\\u20ac\"}"
    ;

    new
        text[64]
    ;

    ASSERT(MySQL_Run(gDatabase, "SELECT %q AS data", json));
    ASSERT(MySQL_GetJSONString(0, "data", key, text));
    ASSERT_SAME(text, name);
    ASSERT(MySQL_GetJSONString(0, "data", "escaped", text));
    ASSERT_SAME(text, "\xE7;\x80;");
    ASSERT(MySQL_GetJSONKey(0, "data", 0, text));
    ASSERT_SAME(text, key);

    ASSERT(MySQL_Run(gDatabase, "CREATE TEMPORARY TABLE mysql_json_test (data JSON NOT NULL) DEFAULT CHARSET = utf8mb4"));
    ASSERT(MySQL_Run(gDatabase, "INSERT INTO mysql_json_test VALUES (JSON_OBJECT('name', %q, 'flag', TRUE, 'x', %f))", name, 1.5));
    ASSERT(MySQL_Run(gDatabase, "SELECT data, CHAR_LENGTH(JSON_VALUE(data, '$.name')) AS characters FROM mysql_json_test"));
    ASSERT_EQ(MySQL_GetInt(0, "characters"), 5);
    ASSERT(MySQL_GetJSONString(0, "data", "name", text));
    ASSERT_SAME(text, name);
    ASSERT(MySQL_GetJSONBool(0, "data", "flag"));
    ASSERT_NEAR(MySQL_GetJSONFloat(0, "data", "x"), 1.5, 0.0001);
    ASSERT(MySQL_Run(gDatabase, "DROP TEMPORARY TABLE mysql_json_test"));
}

test KeepsResults() {
    Test_Async(3000);
    ASSERT(MySQL_Query(gDatabase, "OnKept", 0, "SELECT 41 + 1 AS answer"));
}

public OnKept(context) {
    gKept = MySQL_KeepResult();
    ASSERT_NE(gKept, INVALID_RESULT_ID);
    ASSERT_EQ(MySQL_KeepResult(), gKept);
    ASSERT_EQ(MySQL_GetInt(0, "answer"), 42);
    ASSERT_FALSE(MySQL_DestroyResult(gKept));
    Test_Done();

    return 1;
}

test UsesKeptResults() {
    ASSERT(MySQL_Run(gDatabase, "SELECT 1 AS other"));
    ASSERT(MySQL_UseResult(gKept));
    ASSERT_EQ(MySQL_GetInt(0, "answer"), 42);
    ASSERT(MySQL_UseResult(INVALID_RESULT_ID));
    ASSERT_EQ(MySQL_GetInt(0, "other"), 1);
    ASSERT(MySQL_DestroyResult(gKept));
    ASSERT_FALSE(MySQL_UseResult(gKept));
}

test RunsInsideCallbacks() {
    Test_Async(3000);
    ASSERT(MySQL_Query(gDatabase, "OnRunInside", 0, "SELECT 1 AS outside"));
}

public OnRunInside(context) {
    ASSERT_EQ(MySQL_GetInt(0, "outside"), 1);
    ASSERT(MySQL_Run(gDatabase, "SELECT 5 AS five"));
    ASSERT_EQ(MySQL_GetInt(0, "five"), 5);
    Test_Done();

    return 1;
}

test FormatsIntoArrays() {
    new const
        word[] = "it's",
        expected[] = "SELECT 'it\\'s'"
    ;

    new
        query[32],
        tiny[8]
    ;

    new const
        length = MySQL_Format(gDatabase, query, sizeof (query), "SELECT %q", word),
        cut = MySQL_Format(gDatabase, tiny, sizeof (tiny), "SELECT %q", word)
    ;

    ASSERT_EQ(length, 14);
    ASSERT_SAME(query, expected);
    ASSERT_EQ(cut, -1);
    ASSERT_SAME(tiny, "");
}

test RefusesBadQueries() {
    ASSERT_FALSE(MySQL_Send(MySQL:99, "SELECT 1"));
    ASSERT_FALSE(MySQL_Query(MySQL:99, "OnOrdered", 0, "SELECT 1"));
    ASSERT_FALSE(MySQL_Send(gDatabase, "SELECT %z", 1));
    ASSERT_FALSE(MySQL_Send(gDatabase, "SELECT %i"));
    ASSERT_FALSE(MySQL_Run(gDatabase, "SELECT * FROM no_such_table"));
    ASSERT_EQ(MySQL_GetErrno(), 1146);
}

test CloseRunsWhatWaits() {
    new const
        MySQL:other = MySQL_Connect(TEST_HOST, TEST_USER, TEST_PASSWORD, TEST_DATABASE, TEST_PORT)
    ;

    ASSERT(MySQL_IsConnected(other));

    for (new i; i != 50; ++i) {
        MySQL_Send(other, "INSERT INTO mysql_queries_test (name, amount) VALUES ('closing', %i)", i);
    }

    ASSERT(MySQL_Close(other));
    ASSERT_FALSE(MySQL_Close(other));
    ASSERT(MySQL_Run(gDatabase, "SELECT COUNT(*) AS total FROM mysql_queries_test WHERE name = 'closing'"));
    ASSERT_EQ(MySQL_GetInt(0, "total"), 50);
}

test CountsQueries() {
    new
        sent,
        failed,
        Float:average,
        Float:longest
    ;

    ASSERT(MySQL_GetStats(gDatabase, sent, failed, average, longest));
    ASSERT_GT(sent, 20);
    ASSERT_GE(failed, 2);
    ASSERT(longest > 0.0);
    ASSERT_EQ(MySQL_GetPendingCount(gDatabase), 0);
}

public OnTestsFinished(tests, checks, fails) {
    MySQL_Run(gDatabase, "DROP TABLE IF EXISTS mysql_queries_test");
    MySQL_Close(gDatabase);

    return 0;
}

main() {}
