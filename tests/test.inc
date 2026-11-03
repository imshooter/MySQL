#if defined _INC_MYSQL_TEST
    #endinput
#endif
#define _INC_MYSQL_TEST

/**
 * A small runner for the tests of the component. A test is a function declared with `test`, and
 * `Test_Run` runs the names it is given one after another, then prints the result and shuts the
 * server down. A test that waits for a callback calls `Test_Async` and ends with `Test_Done`.
 *
 * The database the tests use is set with `TEST_HOST`, `TEST_USER`, `TEST_PASSWORD`,
 * `TEST_DATABASE` and `TEST_PORT`, defined before this include. The port can also be given to the
 * compiler, as `TEST_PORT=3307`.
 */

#if !defined TEST_HOST
    #define TEST_HOST "127.0.0.1"
#endif

#if !defined TEST_USER
    #define TEST_USER "root"
#endif

#if !defined TEST_PASSWORD
    #define TEST_PASSWORD ""
#endif

#if !defined TEST_DATABASE
    #define TEST_DATABASE "mysql_test"
#endif

#if !defined TEST_PORT
    #define TEST_PORT (3306)
#endif

#define MAX_TESTS (64)
#define MAX_TEST_NAME (32)
#define MAX_TEST_LOG (32)

#define test%0(%1) forward%0();public%0()

#define ASSERT(%0) Test_Check(bool:(%0), __line)
#define ASSERT_FALSE(%0) Test_Check(!(%0), __line)
#define ASSERT_EQ(%0,%1) Test_Compare(_:(%0), _:(%1), 0, __line)
#define ASSERT_NE(%0,%1) Test_Compare(_:(%0), _:(%1), 1, __line)
#define ASSERT_GT(%0,%1) Test_Compare(_:(%0), _:(%1), 2, __line)
#define ASSERT_GE(%0,%1) Test_Compare(_:(%0), _:(%1), 3, __line)
#define ASSERT_LT(%0,%1) Test_Compare(_:(%0), _:(%1), 4, __line)
#define ASSERT_SAME(%0,%1) Test_Same(%0, %1, __line)
#define ASSERT_NEAR(%0,%1,%2) Test_Near(Float:(%0), Float:(%1), Float:(%2), __line)
#define ASSERT_LOG(%0) Test_CheckLog(%0, __line)

static
    gTestNames[MAX_TESTS][MAX_TEST_NAME],
    gTestCount,
    gTestIndex,
    gTestChecks,
    gTestFails,
    gTestStart,
    gTestTimer,
    bool:gTestWaiting,
    gTestLog[MAX_TEST_LOG],
    gTestLogged
;

forward @Test_Next();
forward @Test_Timeout();
forward OnTestsFinished(tests, checks, fails);

/**
 * @brief      Run tests one after another, by name.
 *
 * @param      names  Names of the tests.
 * @param      count  How many there are.
 */
stock Test_Run(const names[][], count = sizeof (names)) {
    gTestCount = count > MAX_TESTS ? MAX_TESTS : count;

    for (new i; i != gTestCount; ++i) {
        strcopy(gTestNames[i], names[i]);
    }

    gTestStart = GetTickCount();
    SetTimer("@Test_Next", 0, false);
}

/**
 * @brief      Wait for the test to call `Test_Done`, at most some milliseconds.
 *
 * @param      milliseconds  How long to wait before the test fails.
 */
stock Test_Async(milliseconds) {
    gTestWaiting = true;
    gTestTimer = SetTimer("@Test_Timeout", milliseconds, false);
}

/**
 * @brief      End a test that waits, and go on to the next one.
 */
stock Test_Done() {
    if (!gTestWaiting) {
        return;
    }

    gTestWaiting = false;
    KillTimer(gTestTimer);
    SetTimer("@Test_Next", 0, false);
}

/**
 * @brief      Note a value, for `ASSERT_LOG` to compare with the values expected.
 *
 * @param      value  The value.
 */
stock Test_Log(value) {
    if (gTestLogged < MAX_TEST_LOG) {
        gTestLog[gTestLogged++] = value;
    }
}

stock Test_Fail(line, const detail[]) {
    ++gTestFails;
    printf("[test] FAIL %s line %i: %s", gTestNames[gTestIndex - 1], line, detail);
}

stock Test_Check(bool:ok, line) {
    ++gTestChecks;

    if (!ok) {
        Test_Fail(line, "the condition does not hold");
    }
}

stock Test_Compare(got, want, kind, line) {
    new
        bool:ok
    ;

    ++gTestChecks;

    switch (kind) {
        case 0: {
            ok = (got == want);
        }
        case 1: {
            ok = (got != want);
        }
        case 2: {
            ok = (got > want);
        }
        case 3: {
            ok = (got >= want);
        }
        default: {
            ok = (got < want);
        }
    }

    if (!ok) {
        new
            detail[48]
        ;

        format(detail, sizeof (detail), "got %i, want %i", got, want);
        Test_Fail(line, detail);
    }
}

stock Test_Same(const got[], const want[], line) {
    ++gTestChecks;

    if (strlen(got) == strlen(want) && (got[0] == EOS || strcmp(got, want) == 0)) {
        return;
    }

    new
        detail[160]
    ;

    format(detail, sizeof (detail), "got \"%s\", want \"%s\"", got, want);
    Test_Fail(line, detail);
}

stock Test_Near(Float:got, Float:want, Float:within, line) {
    ++gTestChecks;

    if (floatabs(got - want) > within) {
        new
            detail[48]
        ;

        format(detail, sizeof (detail), "got %f, want %f", got, want);
        Test_Fail(line, detail);
    }
}

stock Test_CheckLog(const expected[], line, count = sizeof (expected)) {
    ++gTestChecks;

    new
        bool:ok = (gTestLogged == count)
    ;

    for (new i; ok && i != count; ++i) {
        ok = (gTestLog[i] == expected[i]);
    }

    if (!ok) {
        Test_Fail(line, "the values logged are not the ones expected");
    }
}

public @Test_Timeout() {
    gTestWaiting = false;
    Test_Fail(0, "it timed out");
    SetTimer("@Test_Next", 0, false);

    return 1;
}

public @Test_Next() {
    while (gTestIndex != gTestCount) {
        gTestLogged = 0;
        CallLocalFunction(gTestNames[gTestIndex++], "");

        if (gTestWaiting) {
            return 1;
        }
    }

    if (funcidx("OnTestsFinished") != -1) {
        CallLocalFunction("OnTestsFinished", "iii", gTestCount, gTestChecks, gTestFails);
    }

    printf("[test] DONE tests %i checks %i fails %i in %i ms", gTestCount, gTestChecks, gTestFails, GetTickCount() - gTestStart);
    SendRconCommand("exit");

    return 1;
}

/**
 * @brief      A cell of the result read as an integer, checking that it was read.
 */
stock CellInt(row, const field[]) {
    new
        value
    ;

    Test_Check(MySQL_GetValueInt(row, field, value), __line);

    return value;
}

/**
 * @brief      A cell of the result read as a float, checking that it was read.
 */
stock Float:CellFloat(row, const field[]) {
    new
        Float:value
    ;

    Test_Check(MySQL_GetValueFloat(row, field, value), __line);

    return value;
}

/**
 * @brief      A value of a JSON cell read as an integer, checking that it was read.
 */
stock JSONInt(row, const field[], const key[]) {
    new
        value
    ;

    Test_Check(MySQL_GetJSONValueInt(row, field, key, value), __line);

    return value;
}

/**
 * @brief      A value of a JSON cell read as a float, checking that it was read.
 */
stock Float:JSONFloat(row, const field[], const key[]) {
    new
        Float:value
    ;

    Test_Check(MySQL_GetJSONValueFloat(row, field, key, value), __line);

    return value;
}

/**
 * @brief      A value of a JSON cell read as a bool, checking that it was read.
 */
stock bool:JSONBool(row, const field[], const key[]) {
    new
        bool:value
    ;

    Test_Check(MySQL_GetJSONValueBool(row, field, key, value), __line);

    return value;
}
