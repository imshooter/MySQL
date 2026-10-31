#include <open.mp>
#include <mysql>
#include "test.inc"

static const gTests[][MAX_TEST_NAME] = {
    "RunsOnce",
    "SelectsWithValues",
    "ReadsLongCells",
    "ExecutesLater",
    "SendsBatches",
    "UpdatesInBatches",
    "CopiesValuesAtOnce",
    "SplitsWhatDoesNotFit",
    "SplitBatchIsAllOrNothing",
    "FailedBatchCallsBack",
    "EmptyBatchCallsBack",
    "WritesJSON",
    "RefusesWrongValues",
    "ReportsBadStatements",
    "ClosesStatements",
    "PreparesAgainAfterReconnect",
    "KeepsBatchesApart"
};

/**
 * Prepared statements and batches. The tests make the table mysql_statements_test and drop it
 * when they end. A batch larger than max_allowed_packet of the server is split, and the tests pass
 * either way.
 */

static
    MySQL:gDatabase = INVALID_MYSQL_ID,
    Statement:gInsert = INVALID_STATEMENT_ID,
    Statement:gUpsert = INVALID_STATEMENT_ID,
    gErrors
;

forward OnExecuted(context);
forward OnBatchFailed(context);
forward OnEmptyBatch(context);

public OnGameModeInit() {
    gDatabase = MySQL_Connect(TEST_HOST, TEST_USER, TEST_PASSWORD, TEST_DATABASE, TEST_PORT);
    MySQL_Run(gDatabase, "DROP TABLE IF EXISTS mysql_statements_test");
    MySQL_Run(gDatabase, "CREATE TABLE mysql_statements_test (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64) NOT NULL, amount INT NOT NULL, ratio FLOAT NULL, flags INT UNSIGNED NULL, doc JSON NULL, big TEXT NULL, UNIQUE KEY name (name)) DEFAULT CHARSET=utf8mb4");
    gInsert = MySQL_PrepareStatement(gDatabase, "INSERT INTO mysql_statements_test (name, amount, ratio) VALUES (%q, %i, %f)");
    gUpsert = MySQL_PrepareStatement(gDatabase, "INSERT INTO mysql_statements_test (name, amount) VALUES (%q, %i) ON DUPLICATE KEY UPDATE amount = VALUES(amount)");
    Test_Run(gTests);

    return 1;
}

public OnMySQLError(MySQL:handle, errorid, const error[], const query[], const callback[]) {
    ++gErrors;


    return 1;
}

stock Count(const where[]) {
    MySQL_Run(gDatabase, "SELECT COUNT(*) AS total FROM mysql_statements_test WHERE %s", where);

    return MySQL_GetInt(0, "total");
}

test RunsOnce() {
    ASSERT_NE(gInsert, INVALID_STATEMENT_ID);
    ASSERT(MySQL_RunStatement(gInsert, "alpha", 5, 1.5));
    ASSERT_EQ(MySQL_GetErrno(), 0);
    ASSERT_EQ(MySQL_GetAffectedRows(), 1);
    ASSERT_GT(MySQL_GetInsertID(), 0);
}

test SelectsWithValues() {
    new const
        Statement:select = MySQL_PrepareStatement(gDatabase, "SELECT name, amount, ratio, NULL AS nothing, %i + 1 AS next, %u AS big FROM mysql_statements_test WHERE name = %q")
    ;

    new
        text[16]
    ;

    ASSERT(MySQL_RunStatement(select, 41, -1, "alpha"));
    ASSERT_EQ(MySQL_GetRowCount(), 1);
    ASSERT_EQ(MySQL_GetFieldCount(), 6);
    ASSERT(MySQL_GetString(0, "name", text));
    ASSERT_SAME(text, "alpha");
    ASSERT_EQ(MySQL_GetInt(0, "amount"), 5);
    ASSERT_NEAR(MySQL_GetFloat(0, "ratio"), 1.5, 0.0001);
    ASSERT(MySQL_IsNull(0, "nothing"));
    ASSERT_EQ(MySQL_GetInt(0, "next"), 42);
    MySQL_GetString(0, "big", text);
    ASSERT_SAME(text, "4294967295");
    ASSERT(MySQL_RunStatement(select, 0, 0, "nobody"));
    ASSERT_EQ(MySQL_GetRowCount(), 0);
    ASSERT(MySQL_CloseStatement(select));
}

test ReadsLongCells() {
    new const
        Statement:select = MySQL_PrepareStatement(gDatabase, "SELECT REPEAT(%q, %i) AS long_text")
    ;

    new
        text[400]
    ;

    ASSERT(MySQL_RunStatement(select, "abc", 100));
    ASSERT(MySQL_GetString(0, "long_text", text));
    ASSERT_EQ(strlen(text), 300);
    ASSERT_EQ(text[299], 'c');
    MySQL_CloseStatement(select);
}

test ExecutesLater() {
    Test_Async(3000);
    ASSERT(MySQL_ExecuteStatement(gInsert, "OnExecuted", 7, "beta", 6, 2.5));
}

public OnExecuted(context) {
    ASSERT_EQ(context, 7);
    ASSERT_EQ(MySQL_GetErrno(), 0);
    ASSERT_EQ(MySQL_GetAffectedRows(), 1);
    Test_Done();

    return 1;
}

test SendsBatches() {
    new const
        Batch:batch = MySQL_CreateBatch(gInsert)
    ;

    new
        name[32]
    ;

    for (new i; i != 2000; ++i) {
        format(name, sizeof (name), "row %i", i);
        ASSERT(MySQL_AddBatchRow(batch, name, i, float(i) / 2.0));
    }

    ASSERT_EQ(MySQL_GetBatchRows(batch), 2000);
    ASSERT(MySQL_RunBatch(batch));
    ASSERT_EQ(MySQL_GetAffectedRows(), 2000);
    ASSERT_EQ(MySQL_GetBatchRows(batch), -1);
    ASSERT_EQ(Count("name LIKE 'row %%'"), 2000);
    MySQL_Run(gDatabase, "SELECT SUM(amount) AS total, MAX(ratio) AS top FROM mysql_statements_test WHERE name LIKE 'row %%'");
    ASSERT_EQ(MySQL_GetInt(0, "total"), 1999000);
    ASSERT_NEAR(MySQL_GetFloat(0, "top"), 999.5, 0.001);
}

test UpdatesInBatches() {
    new
        Batch:batch = MySQL_CreateBatch(gUpsert),
        name[32]
    ;

    for (new i; i != 100; ++i) {
        format(name, sizeof (name), "row %i", i);
        MySQL_AddBatchRow(batch, name, 1);
    }

    ASSERT(MySQL_RunBatch(batch));
    MySQL_Run(gDatabase, "SELECT SUM(amount) AS total FROM mysql_statements_test WHERE name LIKE 'row %%'");
    ASSERT_EQ(MySQL_GetInt(0, "total"), 1999000 - 4950 + 100);
}

test CopiesValuesAtOnce() {
    new
        Batch:batch = MySQL_CreateBatch(gUpsert),
        name[32]
    ;

    for (new i; i != 300; ++i) {
        format(name, sizeof (name), "copy %i", i);
        MySQL_AddBatchRow(batch, name, i);
    }

    name = "changed afterwards";
    ASSERT(MySQL_RunBatch(batch));
    ASSERT_EQ(Count("name LIKE 'copy %%'"), 300);
    MySQL_Run(gDatabase, "SELECT COUNT(DISTINCT amount) AS total FROM mysql_statements_test WHERE name LIKE 'copy %%'");
    ASSERT_EQ(MySQL_GetInt(0, "total"), 300);
}

test SplitsWhatDoesNotFit() {
    new const
        Statement:big = MySQL_PrepareStatement(gDatabase, "INSERT INTO mysql_statements_test (name, amount, big) VALUES (%q, %i, %q)")
    ;

    new
        Batch:batch = MySQL_CreateBatch(big),
        name[32],
        text[601]
    ;

    for (new i; i != 600; ++i) {
        text[i] = 'a' + i % 26;
    }

    for (new i; i != 3000; ++i) {
        format(name, sizeof (name), "big %i", i);
        MySQL_AddBatchRow(batch, name, i, text);
    }

    ASSERT(MySQL_RunBatch(batch));
    ASSERT_EQ(MySQL_GetAffectedRows(), 3000);
    ASSERT_EQ(Count("name LIKE 'big %%' AND LENGTH(big) = 600"), 3000);
    MySQL_CloseStatement(big);
}

test SplitBatchIsAllOrNothing() {
    new const
        Statement:big = MySQL_PrepareStatement(gDatabase, "INSERT INTO mysql_statements_test (name, amount, big) VALUES (%q, %i, %q)")
    ;

    new
        Batch:batch = MySQL_CreateBatch(big),
        name[32],
        text[601]
    ;

    for (new i; i != 600; ++i) {
        text[i] = 'z';
    }

    for (new i; i != 3000; ++i) {
        format(name, sizeof (name), "whole %i", i);
        MySQL_AddBatchRow(batch, name, i, text);
    }

    MySQL_AddBatchRow(batch, "whole 0", 0, text);
    gErrors = 0;
    ASSERT_FALSE(MySQL_RunBatch(batch));
    ASSERT_EQ(MySQL_GetErrno(), 1062);
    ASSERT_EQ(gErrors, 1);
    ASSERT_EQ(Count("name LIKE 'whole %%'"), 0);
    MySQL_CloseStatement(big);
}

test FailedBatchCallsBack() {
    new
        Batch:batch = MySQL_CreateBatch(gInsert)
    ;

    MySQL_AddBatchRow(batch, "alpha", 1, 1.0);
    Test_Async(3000);
    ASSERT(MySQL_ExecuteBatch(batch, "OnBatchFailed", 9));
}

public OnBatchFailed(context) {
    ASSERT_EQ(context, 9);
    ASSERT_EQ(MySQL_GetErrno(), 1062);
    Test_Done();

    return 1;
}

test EmptyBatchCallsBack() {
    Test_Async(3000);
    ASSERT(MySQL_ExecuteBatch(MySQL_CreateBatch(gInsert), "OnEmptyBatch", 3));
}

public OnEmptyBatch(context) {
    ASSERT_EQ(context, 3);
    ASSERT_EQ(MySQL_GetErrno(), 0);
    ASSERT_EQ(MySQL_GetAffectedRows(), 0);
    Test_Done();

    return 1;
}

test WritesJSON() {
    new
        text[64]
    ;

    new const
        written[] = "\"a\\\"b\\\\c\\n\\t\"",
        said[] = "he said \"hi\" \\ bye"
    ;

    MySQL_Format(gDatabase, text, sizeof (text), "%j", "a\"b\\c\n\t");
    ASSERT_SAME(text, written);

    new const
        Statement:json = MySQL_PrepareStatement(gDatabase, "INSERT INTO mysql_statements_test (name, amount, doc) VALUES (%q, %i, %Q)"),
        Query:doc = MySQL_CreateQuery(gDatabase)
    ;

    MySQL_AppendQuery(doc, "{%j: %j, %j: %i}", "say", said, "n", 3);
    ASSERT(MySQL_RunStatement(json, "json", 1, doc));
    MySQL_ClearQuery(doc);
    ASSERT_EQ(MySQL_GetQueryLength(doc), 0);
    MySQL_DestroyQuery(doc);

    ASSERT(MySQL_Run(gDatabase, "SELECT doc FROM mysql_statements_test WHERE name = 'json'"));
    ASSERT(MySQL_GetJSONString(0, "doc", "say", text));
    ASSERT_SAME(text, said);
    ASSERT_EQ(MySQL_GetJSONInt(0, "doc", "n"), 3);
    MySQL_CloseStatement(json);
}

test RefusesWrongValues() {
    new
        Batch:batch = MySQL_CreateBatch(gInsert)
    ;

    ASSERT_FALSE(MySQL_RunStatement(gInsert, "only a name"));
    ASSERT_FALSE(MySQL_AddBatchRow(batch, "two", 2));
    ASSERT_EQ(MySQL_GetBatchRows(batch), 0);
    ASSERT_FALSE(MySQL_ExecuteStatement(gInsert, "", 0, "too", 1, 1.0, 4));
    ASSERT_EQ(MySQL_PrepareStatement(gDatabase, "SELECT %z"), INVALID_STATEMENT_ID);
    ASSERT_EQ(MySQL_PrepareStatement(INVALID_MYSQL_ID, "SELECT 1"), INVALID_STATEMENT_ID);
    ASSERT(MySQL_DestroyBatch(batch));
    ASSERT_FALSE(MySQL_DestroyBatch(batch));
}

test ReportsBadStatements() {
    new const
        Statement:bad = MySQL_PrepareStatement(gDatabase, "SELEC nothing FROM %i")
    ;

    ASSERT_NE(bad, INVALID_STATEMENT_ID);
    gErrors = 0;
    ASSERT_FALSE(MySQL_RunStatement(bad, 1));
    ASSERT_EQ(MySQL_GetErrno(), 1064);
    ASSERT_GE(gErrors, 1);

    new const
        Statement:own = MySQL_PrepareStatement(gDatabase, "SELECT ? + %i AS sum")
    ;

    ASSERT_FALSE(MySQL_RunStatement(own, 1));
    ASSERT_EQ(MySQL_GetErrno(), 2034);
    MySQL_CloseStatement(bad);
    MySQL_CloseStatement(own);
}

test ClosesStatements() {
    new const
        Statement:select = MySQL_PrepareStatement(gDatabase, "SELECT %i AS one"),
        Batch:batch = MySQL_CreateBatch(select)
    ;

    ASSERT(MySQL_RunStatement(select, 1));
    ASSERT(MySQL_CloseStatement(select));
    ASSERT_FALSE(MySQL_RunStatement(select, 1));
    ASSERT_EQ(MySQL_GetBatchRows(batch), -1);
    ASSERT_FALSE(MySQL_CloseStatement(select));
}

test PreparesAgainAfterReconnect() {
    new const
        Statement:select = MySQL_PrepareStatement(gDatabase, "SELECT %i * 2 AS twice")
    ;

    ASSERT(MySQL_RunStatement(select, 4));
    ASSERT_EQ(MySQL_GetInt(0, "twice"), 8);
    MySQL_Run(gDatabase, "KILL CONNECTION_ID()");
    ASSERT(MySQL_RunStatement(select, 5));
    ASSERT_EQ(MySQL_GetInt(0, "twice"), 10);
    MySQL_CloseStatement(select);
}

test KeepsBatchesApart() {
    new
        Batch:first = MySQL_CreateBatch(gUpsert),
        Batch:second = MySQL_CreateBatch(gUpsert)
    ;

    MySQL_AddBatchRow(first, "apart 1", 1);
    MySQL_AddBatchRow(second, "apart 2", 2);
    MySQL_AddBatchRow(first, "apart 3", 3);
    ASSERT_EQ(MySQL_GetBatchRows(first), 2);
    ASSERT_EQ(MySQL_GetBatchRows(second), 1);
    ASSERT(MySQL_RunBatch(second));
    ASSERT_EQ(Count("name LIKE 'apart %%'"), 1);
    ASSERT(MySQL_RunBatch(first));
    MySQL_Run(gDatabase, "SELECT SUM(amount) AS total FROM mysql_statements_test WHERE name LIKE 'apart %%'");
    ASSERT_EQ(MySQL_GetInt(0, "total"), 6);
}


public OnTestsFinished(tests, checks, fails) {
    MySQL_Run(gDatabase, "DROP TABLE IF EXISTS mysql_statements_test");
    MySQL_Close(gDatabase);

    return 1;
}

main() {}
