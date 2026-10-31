#include <open.mp>
#include <mysql>
#include "test.inc"

static const gTests[][MAX_TEST_NAME] = {
    "RefusesLongQueries",
    "RefusesPastPending",
    "RefusesPastRate",
    "ReadTimeoutGivesUp",
    "ReadsTheEnvironment",
    "RequiredTLSNeverFallsBack"
};

/**
 * Limits, timeouts, environment variables and required TLS, against a server without TLS.
 * ReadsTheEnvironment needs the environment variable MYSQL_TEST_DATABASE set to the name of the
 * test database.
 */

static
    MySQL:gLimited = INVALID_MYSQL_ID
;

forward @Settings_Pending();
forward @Settings_Rate();

MySQL:Open(const options[]) {
    return MySQL_Connect(TEST_HOST, TEST_USER, TEST_PASSWORD, TEST_DATABASE, TEST_PORT, options);
}

test RefusesLongQueries() {
    new const
        MySQL:handle = Open("max_query_length=40")
    ;

    ASSERT(MySQL_Run(handle, "SELECT 1"));
    ASSERT_FALSE(MySQL_Run(handle, "SELECT '%s'", "a string that makes the query too long for the limit"));
    ASSERT_FALSE(MySQL_Send(handle, "SELECT '%s'", "a string that makes the query too long for the limit"));
    ASSERT_FALSE(MySQL_ThreadedQuery(handle, "@Settings_Pending", 0, "SELECT '%s'", "a string that makes the query too long for the limit"));

    new const
        Query:query = MySQL_CreateQuery(handle, "SELECT '%s'", "a string that makes the query too long")
    ;

    ASSERT_FALSE(MySQL_SendQuery(query));
    ASSERT_EQ(MySQL_GetQueryLength(query), -1);
    ASSERT(MySQL_Close(handle));
}

test RefusesPastPending() {
    gLimited = Open("max_pending=2");
    ASSERT(MySQL_Send(gLimited, "SELECT SLEEP(0.3)"));
    ASSERT(MySQL_Send(gLimited, "SELECT SLEEP(0.3)"));
    ASSERT_FALSE(MySQL_Send(gLimited, "SELECT 1"));
    ASSERT_EQ(MySQL_GetPendingCount(gLimited), 2);
    Test_Async(3000);
    SetTimer("@Settings_Pending", 900, false);
}

public @Settings_Pending() {
    ASSERT_EQ(MySQL_GetPendingCount(gLimited), 0);
    ASSERT(MySQL_Send(gLimited, "SELECT 1"));
    MySQL_Close(gLimited);
    Test_Done();

    return 1;
}

test RefusesPastRate() {
    gLimited = Open("max_queries_per_second=5");

    for (new i; i != 5; ++i) {
        ASSERT(MySQL_Send(gLimited, "SELECT %i", i));
    }

    ASSERT_FALSE(MySQL_Send(gLimited, "SELECT 5"));
    ASSERT_FALSE(MySQL_Run(gLimited, "SELECT 6"));
    Test_Async(3000);
    SetTimer("@Settings_Rate", 1100, false);
}

public @Settings_Rate() {
    ASSERT(MySQL_Run(gLimited, "SELECT 7"));
    MySQL_Close(gLimited);
    Test_Done();

    return 1;
}

test ReadTimeoutGivesUp() {
    new const
        MySQL:handle = Open("read_timeout=1"),
        start = GetTickCount()
    ;

    ASSERT_FALSE(MySQL_Run(handle, "SELECT SLEEP(4)"));

    new const
        error = MySQL_GetErrno(),
        waited = GetTickCount() - start
    ;

    Test_Log(error);
    ASSERT_GE(error, 2000);
    ASSERT_LT(waited, 3000);
    ASSERT(MySQL_Run(handle, "SELECT 1 AS one"));
    ASSERT_EQ(MySQL_GetInt(0, "one"), 1);
    MySQL_Close(handle);
}

test ReadsTheEnvironment() {
    new
        File:file = fopen("envtest.ini", io_write),
        text[160],
        name[32]
    ;

    format(text, sizeof (text), "hostname = %s\nusername = %s\npassword = %s\ndatabase = ${MYSQL_TEST_DATABASE}\nport = %i\n", TEST_HOST, TEST_USER, TEST_PASSWORD, TEST_PORT);
    fwrite(file, text);
    fclose(file);
    file = fopen("envmissing.ini", io_write);
    fwrite(file, "hostname = 127.0.0.1\npassword = ${MYSQL_TEST_NOT_SET}\n");
    fclose(file);

    new const
        MySQL:handle = MySQL_ConnectFile("scriptfiles/envtest.ini")
    ;

    ASSERT_NE(handle, INVALID_MYSQL_ID);
    ASSERT(MySQL_IsConnected(handle));
    ASSERT(MySQL_Run(handle, "SELECT DATABASE() AS name"));
    MySQL_GetString(0, "name", name);
    ASSERT_SAME(name, TEST_DATABASE);
    MySQL_Close(handle);
    ASSERT_EQ(MySQL_ConnectFile("scriptfiles/envmissing.ini"), INVALID_MYSQL_ID);
    fremove("envtest.ini");
    fremove("envmissing.ini");
}

test RequiredTLSNeverFallsBack() {
    new
        tls[64]
    ;

    new const
        MySQL:handle = Open("ssl=true")
    ;

    ASSERT_NE(handle, INVALID_MYSQL_ID);
    ASSERT_FALSE(MySQL_IsConnected(handle));
    ASSERT_EQ(MySQL_GetConnectionErrno(handle), 2026);
    ASSERT_FALSE(MySQL_GetConnectionTLS(handle, tls));
    ASSERT_FALSE(MySQL_Run(handle, "SELECT 1"));
    MySQL_Close(handle);

    new const
        MySQL:plain = Open("")
    ;

    ASSERT(MySQL_IsConnected(plain));
    ASSERT_FALSE(MySQL_GetConnectionTLS(plain, tls));
    ASSERT_SAME(tls, "");
    MySQL_Close(plain);
}

public OnGameModeInit() {
    Test_Run(gTests);

    return 1;
}

main() {}
