#if defined _INC_MYSQL
    #endinput
#endif
#define _INC_MYSQL

/**
 * mysql, a MySQL and MariaDB component for open.mp.
 *
 * A handle holds one connection that runs its queries in the order they were sent, and a pool of
 * connections for parallel queries. Every connection lives on a thread of its own and connects again
 * when it is lost. A finished query calls its public on the next server tick with the context it was
 * sent with, also when it failed. Inside that public its result is the one the `MySQL_Get` natives
 * read.
 *
 * Queries are formatted by the component, with no length limit. `%i` and `%d` write an integer, `%u`
 * an unsigned one, `%x` one in hexadecimal and `%f` a float, as many digits as it needs, or `%.4f`
 * four decimals. `%s` writes a string as it is, `%e` escapes it for a string literal and `%q` escapes
 * it and puts it between quotes. `%j` writes a string as a JSON string, between double quotes, for
 * JSON built in a query. `%Q` writes the text of a query being built, as it is. `%%` writes a
 * percent sign.
 *
 * A query too long for one call is built a piece at a time with `MySQL_CreateQuery`,
 * `MySQL_AppendQuery` and `MySQL_TrimQuery`, then sent with `MySQL_SendQuery`. A statement made of
 * parts that grow together, such as the rows of several tables, keeps each part in a query of its own
 * and joins them with `%Q`.
 *
 * A prepared statement is sent to the server once and then run with values that are never escaped
 * or parsed again. Its text takes the same specifiers, each one a value: `%i`, `%d` and `%x` an
 * integer, `%u` an unsigned one, `%f` a float, `%s`, `%e`, `%q` and `%j` a string, bound as it is,
 * and `%Q` the text of a query being built, as a string. `MySQL_Execute` runs it once, results
 * included. A batch gathers rows of values with `MySQL_AddRow` and sends them together with
 * `MySQL_ExecuteBatch`. On MariaDB 10.2 or later the rows go to the server as arrays in one command,
 * split to fit `max_allowed_packet`, and on other servers they run one by one. A batch sent in
 * more than one command runs in a transaction of its own, unless one is open already, so it
 * applies all its rows or none. A statement is prepared again when its connection is lost, and
 * when the server asks for it after the tables changed.
 *
 * A JSON cell is parsed the first time a `MySQL_GetJSON` native reads it. A key is a member of the top
 * object, or a JSON Pointer when it starts with a slash, as `/position/0`. With the `latin1` character
 * set, keys and strings are in the code page of the game, escaped ones such as `\u00e7` included.
 *
 * Passwords are hashed and verified with Argon2id or bcrypt on threads of the component, apart from
 * the connections, with `MySQL_HashArgon2`, `MySQL_HashBcrypt` and `MySQL_VerifyPassword`.
 */

/**
 * # Header
 */

#define INVALID_MYSQL_ID                (MySQL:0)
#define INVALID_QUERY_ID                (Query:0)
#define INVALID_RESULT_ID               (Result:0)
#define INVALID_STATEMENT_ID            (Statement:0)
#define INVALID_BATCH_ID                (Batch:0)

/**
 * Tags a value of a query takes: `Float`, `Query` for `%Q`, and the tags a script lists in
 * `CUSTOM_TAG_TYPES`, as open.mp does.
 */
#define MYSQL_TAGS                      {Float, Query, CUSTOM_TAG_TYPES, _}

/**
 * The Argon2id costs of `MySQL_HashArgon2`, the least OWASP recommends: 19 MiB, two passes, one lane.
 * A hash takes some tens of milliseconds with them on a desktop processor.
 */
#define MYSQL_ARGON2_MEMORY             (19456)
#define MYSQL_ARGON2_ITERATIONS         (2)
#define MYSQL_ARGON2_PARALLELISM        (1)

/**
 * The bcrypt cost of `MySQL_HashBcrypt`. Each step up doubles the time a hash takes.
 */
#define MYSQL_BCRYPT_COST               (12)

/**
 * Characters an array needs for a hash, the end included.
 */
#define MYSQL_HASH_LENGTH               (128)

enum MYSQL_JSON {
    MYSQL_JSON_NONE,
    MYSQL_JSON_NULL,
    MYSQL_JSON_BOOL,
    MYSQL_JSON_INT,
    MYSQL_JSON_FLOAT,
    MYSQL_JSON_STRING,
    MYSQL_JSON_ARRAY,
    MYSQL_JSON_OBJECT
};

/**
 * # Connections
 */

/**
 * @brief      Connect to a database.
 *
 * The connection is made before the native returns. When it fails, the handle is still given and
 * every query tries to connect again, half a second after the first failure and twice as long after
 * each next one, up to five seconds. `options` takes settings as `key=value` separated by
 * semicolons:
 *
 * - `charset`, `latin1` by default, which is the code page of the game.
 * - `multi_statements`, `false` by default, and `pool`, the connections for parallel queries, `0` by
 *   default.
 * - `connect_timeout`, `read_timeout` and `write_timeout`, in seconds. A query whose answer takes
 *   longer than `read_timeout` fails, and the connection opens again.
 * - `slow_query`, the milliseconds from which a query is logged, and `log_errors`, `true` by default.
 * - `max_query_length` in bytes, `max_pending`, the queries waiting or running, and
 *   `max_queries_per_second`. A query past one of them is refused: it is not sent, the native returns
 *   `false`, and a line is logged, at most one a second. `0`, the default, sets no limit.
 * - `ssl` to require TLS. The connection then never runs without it, and fails when the server
 *   answers without TLS. `ssl_verify` also verifies the certificate of the server and its name,
 *   against the certificate authorities of `ssl_ca`, a file, or `ssl_capath`, a folder, and the
 *   revocation lists of `ssl_crl` or `ssl_crlpath`. On `localhost`, `127.0.0.1` and `::1` the name
 *   is not checked, and the authority only when `ssl_ca` or `ssl_capath` is given.
 *   `ssl_fingerprint` requires the certificate of the server to have that SHA1 to SHA512
 *   fingerprint, in hexadecimal with or without colons, which suits a certificate signed by no
 *   authority. `ssl_cert` and `ssl_key` give the certificate of the client, `ssl_cipher` the
 *   ciphers allowed and `tls_version` the versions, as `TLSv1.2,TLSv1.3`. Any of these settings
 *   requires TLS too. Without them the connection does not use TLS, as a database on the same
 *   machine needs none.
 *
 * @param      host      Name or address of the server.
 * @param      user      Account to log in with.
 * @param      password  Password of the account.
 * @param      database  Database the queries use.
 * @param      port      Port of the server.
 * @param      options   Settings of the connection.
 *
 * @return     - the handle
 *             - `INVALID_MYSQL_ID` an option is not valid
 */
native MySQL:MySQL_Connect(const host[], const user[], const password[], const database[], port = 3306, const options[] = "");

/**
 * @brief      Connect to a database with the settings of a file.
 *
 * The file has a `key = value` setting on each line, as the `mysql.ini` of the R41 plugin:
 * `hostname`, `username`, `password`, `database`, `server_port`, `multi_statements`, `pool_size`,
 * `ssl_enable`, `ssl_ca_file`, `ssl_ca_path`, `ssl_cert_file`, `ssl_key_file` and `ssl_cipher`,
 * besides the options of `MySQL_Connect`. A value takes `${NAME}` from the environment of the server,
 * so a password needs not be written in the file:
 *
 *     password = ${MYSQL_PASSWORD}
 *
 * A variable that is not set makes the file not valid.
 *
 * @param      file  Path of the file, from the folder of the server.
 *
 * @return     - the handle
 *             - `INVALID_MYSQL_ID` the file cannot be read or has a setting that is not valid
 */
native MySQL:MySQL_ConnectFile(const file[] = "mysql.ini");

/**
 * @brief      Close a handle after running every query still waiting.
 *
 * The publics of those queries are not called. The script that connected closes its handles itself
 * when it unloads.
 *
 * @param      handle  Handle to close.
 *
 * @return     - `true` it was closed
 *             - `false` it does not exist
 */
native bool:MySQL_Close(MySQL:handle);

/**
 * @brief      Whether the connection of the ordered queries is open.
 *
 * @param      handle  Handle to check.
 */
native bool:MySQL_IsConnected(MySQL:handle);

/**
 * @brief      The error of the last time the handle failed to connect.
 *
 * @param      handle  Handle to check.
 *
 * @return     - the error number of MySQL
 *             - `0` it did not fail since it last connected
 *             - `-1` the handle does not exist
 */
native MySQL_GetConnectionErrno(MySQL:handle);

/**
 * @brief      The message of the last time the handle failed to connect.
 *
 * @param      handle  Handle to check.
 * @param      error   Array for the message.
 * @param      size    Size of the array.
 *
 * @return     - `true` the message was written
 *             - `false` the handle does not exist
 */
native bool:MySQL_GetConnectionError(MySQL:handle, error[], size = sizeof (error));

/**
 * @brief      The TLS version and cipher of the connection of the ordered queries.
 *
 * @param      handle  Handle to check.
 * @param      tls     Array for the version and the cipher, as `TLSv1.3 TLS_AES_256_GCM_SHA384`, or
 *                     empty.
 * @param      size    Size of the array.
 *
 * @return     - `true` the connection is encrypted
 *             - `false` it is not, it is not open, or the handle does not exist
 */
native bool:MySQL_GetConnectionTLS(MySQL:handle, tls[], size = sizeof (tls));

/**
 * @brief      How many queries of a handle wait or run.
 *
 * @param      handle  Handle to check.
 *
 * @return     - the number of queries
 *             - `-1` the handle does not exist
 */
native MySQL_GetPendingCount(MySQL:handle);

/**
 * @brief      What a handle did since it connected.
 *
 * @param      handle   Handle to check.
 * @param      sent     Queries sent.
 * @param      failed   Queries that failed.
 * @param      average  Milliseconds a query took to run, on average.
 * @param      longest  Milliseconds the longest query took to run.
 *
 * @return     - `true` the numbers were written
 *             - `false` the handle does not exist
 */
native bool:MySQL_GetStats(MySQL:handle, &sent, &failed, &Float:average, &Float:longest);

/**
 * # Queries
 */

/**
 * @brief      Send a query that needs no answer.
 *
 * @param      handle  Handle to send it through.
 * @param      query   The query, formatted with the values after it.
 *
 * @return     - `true` it was sent
 *             - `false` the handle does not exist, the format is not valid, or a limit of the
 *               connection refused it
 */
native bool:MySQL_Send(MySQL:handle, const query[], MYSQL_TAGS:...);

/**
 * @brief      Send a query, and call a public with `context` when it is done.
 *
 * The queries of a handle run in the order they were sent. The public is called also when the query
 * failed, and `MySQL_GetErrno` gives its error.
 *
 * @param      handle    Handle to send it through.
 * @param      callback  Public to call.
 * @param      context   Value the public receives.
 * @param      query     The query, formatted with the values after it.
 *
 * @return     - `true` it was sent
 *             - `false` the handle does not exist, the format is not valid, or a limit of the
 *               connection refused it, and the public is not called
 */
native bool:MySQL_Query(MySQL:handle, const callback[], context, const query[], MYSQL_TAGS:...);

/**
 * @brief      Send a query that any connection of the pool may run, out of order with the rest.
 *
 * Without a pool, it runs in order as `MySQL_Query` does.
 *
 * @param      handle    Handle to send it through.
 * @param      callback  Public to call.
 * @param      context   Value the public receives.
 * @param      query     The query, formatted with the values after it.
 *
 * @return     - `true` it was sent
 *             - `false` the handle does not exist, the format is not valid, or a limit of the
 *               connection refused it, and the public is not called
 */
native bool:MySQL_ParallelQuery(MySQL:handle, const callback[], context, const query[], MYSQL_TAGS:...);

/**
 * @brief      Run a query and wait for it, after the queries sent before it.
 *
 * Its result is the one read until the next `MySQL_Run`, or until the public that ran it returns.
 * The server stops while it waits, so it suits the tables made at start up.
 *
 * @param      handle  Handle to run it through.
 * @param      query   The query, formatted with the values after it.
 *
 * @return     - `true` it succeeded
 *             - `false` it failed, the handle does not exist, the format is not valid, or a limit
 *               of the connection refused it
 */
native bool:MySQL_Run(MySQL:handle, const query[], MYSQL_TAGS:...);

/**
 * @brief      Format a query into an array.
 *
 * @param      handle  Handle whose escaping is used.
 * @param      output  Array for the query.
 * @param      size    Size of the array.
 * @param      format  The query, formatted with the values after it.
 *
 * @return     - the length of the query
 *             - `-1` it does not fit, and nothing is written, or the format is not valid
 */
native MySQL_Format(MySQL:handle, output[], size, const format[], MYSQL_TAGS:...);

/**
 * # Queries built in pieces
 */

/**
 * @brief      Start a query to build a piece at a time.
 *
 * @param      handle  Handle to send it through.
 * @param      text    First piece, formatted with the values after it.
 *
 * @return     - the query
 *             - `INVALID_QUERY_ID` the handle does not exist or the format is not valid
 */
native Query:MySQL_CreateQuery(MySQL:handle, const text[] = "", MYSQL_TAGS:...);

/**
 * @brief      Add a piece to a query.
 *
 * @param      query  Query to add to.
 * @param      text   The piece, formatted with the values after it.
 *
 * @return     - `true` it was added
 *             - `false` the query does not exist or the format is not valid, and nothing is added
 */
native bool:MySQL_AppendQuery(Query:query, const text[], MYSQL_TAGS:...);

/**
 * @brief      Remove characters from the end of a query, such as the comma after the last row.
 *
 * @param      query       Query to cut.
 * @param      characters  How many characters to remove.
 *
 * @return     - `true` they were removed
 *             - `false` the query does not exist
 */
native bool:MySQL_TrimQuery(Query:query, characters);

/**
 * @brief      The length of a query being built.
 *
 * @param      query  Query to measure.
 *
 * @return     - the length
 *             - `-1` the query does not exist
 */
native MySQL_GetQueryLength(Query:query);

/**
 * @brief      Send a query built in pieces, and forget it, also when it is refused.
 *
 * @param      query     Query to send.
 * @param      callback  Public to call when it is done, or none.
 * @param      context   Value the public receives.
 * @param      parallel  Whether any connection of the pool may run it.
 *
 * @return     - `true` it was sent
 *             - `false` the query does not exist, or a limit of the connection refused it, and
 *               the public is not called
 */
native bool:MySQL_SendQuery(Query:query, const callback[] = "", context = 0, bool:parallel = false);

/**
 * @brief      Run a query built in pieces and wait for it, as `MySQL_Run` does, and forget it.
 *
 * @param      query  Query to run.
 *
 * @return     - `true` it succeeded
 *             - `false` it failed, the query does not exist, or a limit of the connection
 *               refused it
 */
native bool:MySQL_RunQuery(Query:query);

/**
 * @brief      Forget a query being built without sending it.
 *
 * @param      query  Query to forget.
 *
 * @return     - `true` it was forgotten
 *             - `false` it does not exist
 */
native bool:MySQL_DestroyQuery(Query:query);

/**
 * @brief      Empty a query being built, to build another in it.
 *
 * @param      query  Query to empty.
 *
 * @return     - `true` it was emptied
 *             - `false` it does not exist
 */
native bool:MySQL_ClearQuery(Query:query);

/**
 * # Prepared statements
 */

/**
 * @brief      Prepare a statement, with a specifier for each value.
 *
 * It is sent to the server at once and runs on the connection of the handle that runs queries in
 * order. An error in it is logged, and reported through `OnMySQLError`, before it first runs.
 *
 * @param      handle  Handle to run it through.
 * @param      query   The statement, as `INSERT INTO items (uuid, slot) VALUES (%q, %i)`.
 *
 * @return     - the statement
 *             - `INVALID_STATEMENT_ID` the handle does not exist or a specifier is not valid
 */
native Statement:MySQL_Prepare(MySQL:handle, const query[]);

/**
 * @brief      Run a statement once, after the queries sent before it.
 *
 * Inside the public, the result is the one the `MySQL_Get` natives read, as for a query.
 *
 * @param      statement  The statement.
 * @param      callback   Public to call when it is done, or none.
 * @param      context    Value the public receives.
 * @param      ...        A value for each specifier.
 *
 * @return     - `true` it was sent
 *             - `false` the statement does not exist, the values do not match it, or a limit of
 *               the connection refused it
 */
native bool:MySQL_Execute(Statement:statement, const callback[] = "", context = 0, MYSQL_TAGS:...);

/**
 * @brief      Run a statement once and wait for it, as `MySQL_Run` does.
 *
 * @param      statement  The statement.
 * @param      ...        A value for each specifier.
 *
 * @return     - `true` it succeeded
 *             - `false` it failed, the statement does not exist, the values do not match it, or a
 *               limit of the connection refused it
 */
native bool:MySQL_RunStatement(Statement:statement, MYSQL_TAGS:...);

/**
 * @brief      Start a batch of rows for a statement.
 *
 * @param      statement  The statement.
 *
 * @return     - the batch
 *             - `INVALID_BATCH_ID` the statement does not exist
 */
native Batch:MySQL_CreateBatch(Statement:statement);

/**
 * @brief      Add a row of values to a batch. The values are copied at once.
 *
 * @param      batch  The batch.
 * @param      ...    A value for each specifier of its statement.
 *
 * @return     - `true` the row was added
 *             - `false` the batch does not exist or the values do not match its statement, and
 *               nothing is added
 */
native bool:MySQL_AddRow(Batch:batch, MYSQL_TAGS:...);

/**
 * @brief      The rows added to a batch.
 *
 * @param      batch  The batch.
 *
 * @return     - the number of rows
 *             - `-1` the batch does not exist
 */
native MySQL_GetBatchRows(Batch:batch);

/**
 * @brief      Send the rows of a batch after the queries sent before it, and forget it, also when
 *             it is refused.
 *
 * Inside the public, `MySQL_GetAffectedRows` reads the rows of all of them. A batch with no rows is
 * not sent to the server. Its public is still called, in order with the rest, and without one it
 * is only forgotten.
 *
 * @param      batch     The batch.
 * @param      callback  Public to call when it is done, or none.
 * @param      context   Value the public receives.
 *
 * @return     - `true` it was sent
 *             - `false` the batch or its statement does not exist, or a limit of the connection
 *               refused it, and the public is not called
 */
native bool:MySQL_ExecuteBatch(Batch:batch, const callback[] = "", context = 0);

/**
 * @brief      Send the rows of a batch and wait for them, as `MySQL_Run` does, and forget it.
 *
 * @param      batch  The batch.
 *
 * @return     - `true` it succeeded
 *             - `false` it failed, the batch or its statement does not exist, or a limit of the
 *               connection refused it
 */
native bool:MySQL_RunBatch(Batch:batch);

/**
 * @brief      Forget a batch without sending it.
 *
 * @param      batch  The batch.
 *
 * @return     - `true` it was forgotten
 *             - `false` it does not exist
 */
native bool:MySQL_DestroyBatch(Batch:batch);

/**
 * @brief      Close a statement after what was sent before it, and forget its batches.
 *
 * @param      statement  The statement.
 *
 * @return     - `true` it was closed
 *             - `false` it does not exist
 */
native bool:MySQL_CloseStatement(Statement:statement);

/**
 * # Results
 */

/**
 * @brief      The error of the query read.
 *
 * @return     - the error number of MySQL
 *             - `0` it succeeded, or no query is read
 */
native MySQL_GetErrno();

/**
 * @brief      The message of the error of the query read.
 *
 * @param      error  Array for the message, empty when it succeeded.
 * @param      size   Size of the array.
 *
 * @return     - `true` a query is read
 *             - `false` none is
 */
native bool:MySQL_GetError(error[], size = sizeof (error));

/**
 * @brief      The text of the query read.
 *
 * @param      query  Array for the text.
 * @param      size   Size of the array.
 *
 * @return     - `true` a query is read
 *             - `false` none is
 */
native bool:MySQL_GetQuery(query[], size = sizeof (query));

/**
 * @brief      Milliseconds the query read took to run, without the time it waited.
 */
native Float:MySQL_GetQueryTime();

/**
 * @brief      How many statements of the query read gave a result.
 *
 * A query with several statements has a result for each. The first one is read until
 * `MySQL_SelectResultSet` chooses another.
 */
native MySQL_GetResultSetCount();

/**
 * @brief      Choose which statement of the query read the natives read.
 *
 * @param      index  The statement, from `0`.
 *
 * @return     - `true` it was chosen
 *             - `false` the query has no such statement
 */
native bool:MySQL_SelectResultSet(index);

/**
 * @brief      How many rows the result read has.
 */
native MySQL_GetRowCount();

/**
 * @brief      How many fields the result read has.
 */
native MySQL_GetFieldCount();

/**
 * @brief      The name of a field of the result read.
 *
 * @param      field  The field, from `0`.
 * @param      name   Array for the name.
 * @param      size   Size of the array.
 *
 * @return     - `true` the name was written
 *             - `false` the result has no such field
 */
native bool:MySQL_GetFieldName(field, name[], size = sizeof (name));

/**
 * @brief      The index of a field of the result read, found by its name in any case.
 *
 * @param      name  Name of the field.
 *
 * @return     - the index
 *             - `-1` the result has no such field
 */
native MySQL_GetFieldIndex(const name[]);

/**
 * @brief      Rows the statement read changed, or the rows it returned.
 */
native MySQL_GetAffectedRows();

/**
 * @brief      The id `AUTO_INCREMENT` gave the row the statement read inserted.
 */
native MySQL_GetInsertID();

/**
 * @brief      Warnings the statement read gave.
 */
native MySQL_GetWarningCount();

/**
 * @brief      Whether a cell of the result read is NULL.
 *
 * A field the result does not have is logged once, and counts as NULL.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 */
native bool:MySQL_IsNull(row, const field[]);

/**
 * @brief      A cell of the result read as an integer, `0` when it is NULL.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 */
native MySQL_GetInt(row, const field[]);

/**
 * @brief      A cell of the result read as a float, `0.0` when it is NULL.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 */
native Float:MySQL_GetFloat(row, const field[]);

/**
 * @brief      A cell of the result read as a bool, `false` when it is NULL.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 */
native bool:MySQL_GetBool(row, const field[]);

/**
 * @brief      A cell of the result read as a string.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 * @param      value  Array for the text, empty when it is NULL.
 * @param      size   Size of the array.
 *
 * @return     - `true` the text was written
 *             - `false` the cell is NULL or does not exist
 */
native bool:MySQL_GetString(row, const field[], value[], size = sizeof (value));

/**
 * @brief      Read the cells of a row into variables in one call, field after field.
 *
 * Each letter of `format` reads the next field: `i` an integer, `f` a float, `b` a bool and `s[32]`
 * a string with the size of its array. `-` skips a field.
 *
 *     MySQL_GetRow(row, "s[37]is[64]f", uuid, slot, name, price);
 *
 * @param      row     The row, from `0`.
 * @param      format  The fields to read.
 *
 * @return     How many fields were read.
 */
native MySQL_GetRow(row, const format[], {Float, _}:...);

/**
 * @brief      Keep the result read after its public returns.
 *
 * @return     - the kept result, which `MySQL_UseResult` reads again and `MySQL_DestroyResult` frees
 *             - `INVALID_RESULT_ID` no result is read
 */
native Result:MySQL_KeepResult();

/**
 * @brief      Read a kept result, or go back to the result of the public with `INVALID_RESULT_ID`.
 *
 * @param      result  Kept result to read.
 *
 * @return     - `true` it is read
 *             - `false` it does not exist
 */
native bool:MySQL_UseResult(Result:result);

/**
 * @brief      Free a kept result.
 *
 * @param      result  Kept result to free.
 *
 * @return     - `true` it was freed
 *             - `false` it does not exist, or a public running reads it
 */
native bool:MySQL_DestroyResult(Result:result);

/**
 * # JSON cells
 */

/**
 * @brief      The type of a value of a JSON cell.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 * @param      key    Member of the top object, a JSON Pointer, or empty for the whole value.
 *
 * @return     - the type
 *             - `MYSQL_JSON_NONE` the value does not exist or the cell is not JSON
 */
native MYSQL_JSON:MySQL_GetJSONType(row, const field[], const key[] = "");

/**
 * @brief      How many members an object of a JSON cell has, or how many elements an array has.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 * @param      key    Member of the top object, a JSON Pointer, or empty for the whole value.
 */
native MySQL_GetJSONCount(row, const field[], const key[] = "");

/**
 * @brief      The name of a member of an object of a JSON cell, by its position.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 * @param      index  The member, from `0`.
 * @param      key    Array for the name.
 * @param      size   Size of the array.
 * @param      path   JSON Pointer of the object, or empty for the top one.
 *
 * @return     - `true` the name was written
 *             - `false` the object has no such member
 */
native bool:MySQL_GetJSONKey(row, const field[], index, key[], size = sizeof (key), const path[] = "");

/**
 * @brief      A value of a JSON cell as an integer.
 *
 * @param      row       The row, from `0`.
 * @param      field     Name of the field.
 * @param      key       Member of the top object or a JSON Pointer.
 * @param      fallback  What it gives when the value does not exist, is null, or is an object or
 *                       an array.
 */
native MySQL_GetJSONInt(row, const field[], const key[], fallback = 0);

/**
 * @brief      A value of a JSON cell as a float.
 *
 * @param      row       The row, from `0`.
 * @param      field     Name of the field.
 * @param      key       Member of the top object or a JSON Pointer.
 * @param      fallback  What it gives when the value does not exist, is null, or is an object or
 *                       an array.
 */
native Float:MySQL_GetJSONFloat(row, const field[], const key[], Float:fallback = 0.0);

/**
 * @brief      A value of a JSON cell as a bool.
 *
 * @param      row       The row, from `0`.
 * @param      field     Name of the field.
 * @param      key       Member of the top object or a JSON Pointer.
 * @param      fallback  What it gives when the value does not exist, is null, or is an object or
 *                       an array.
 */
native bool:MySQL_GetJSONBool(row, const field[], const key[], bool:fallback = false);

/**
 * @brief      A value of a JSON cell as a string. An object or an array is written as JSON.
 *
 * @param      row    The row, from `0`.
 * @param      field  Name of the field.
 * @param      key    Member of the top object or a JSON Pointer.
 * @param      value  Array for the text.
 * @param      size   Size of the array.
 *
 * @return     - `true` the text was written
 *             - `false` the value does not exist or is null
 */
native bool:MySQL_GetJSONString(row, const field[], const key[], value[], size = sizeof (value));

/**
 * # Passwords
 */

/**
 * @brief      Hash a password with Argon2id on a thread of the component, and call a public with
 *             `context` and the hash.
 *
 * The public takes `(context, const hash[])`. The hash is empty when it failed, and the reason is
 * logged. It holds its salt and costs, so `MySQL_VerifyPassword` needs nothing else, and a hash made
 * with other costs still verifies.
 *
 * @param      password     The password.
 * @param      callback     Public to call.
 * @param      context      Value the public receives.
 * @param      memory       Memory, in KiB, from `8192` to `1048576`.
 * @param      iterations   Passes, from `1` to `16`.
 * @param      parallelism  Lanes, from `1` to `8`.
 *
 * @return     - `true` the work started
 *             - `false` a cost is out of range, or `callback` is not a public of the script
 */
native bool:MySQL_HashArgon2(const password[], const callback[], context = 0, memory = MYSQL_ARGON2_MEMORY, iterations = MYSQL_ARGON2_ITERATIONS, parallelism = MYSQL_ARGON2_PARALLELISM);

/**
 * @brief      Hash a password with bcrypt, as `$2b$`, on a thread of the component, and call a public
 *             with `context` and the hash.
 *
 * The public takes `(context, const hash[])`, as with `MySQL_HashArgon2`. Only the first 72 bytes of
 * the password count, as bcrypt reads no more.
 *
 * @param      password  The password.
 * @param      callback  Public to call.
 * @param      context   Value the public receives.
 * @param      cost      Cost, from `4` to `20`, each step doubling the time.
 *
 * @return     - `true` the work started
 *             - `false` the cost is out of range, or `callback` is not a public of the script
 */
native bool:MySQL_HashBcrypt(const password[], const callback[], context = 0, cost = MYSQL_BCRYPT_COST);

/**
 * @brief      Verify a password against an Argon2 or bcrypt hash on a thread of the component, and
 *             call a public with `context` and whether they match.
 *
 * The public takes `(context, bool:match)`. A hash that cannot be read never matches, and the reason
 * is logged. Verifying costs what the hash cost.
 *
 * @param      password  The password.
 * @param      hash      The hash, from `MySQL_HashArgon2`, `MySQL_HashBcrypt` or any other Argon2 or
 *                       bcrypt library.
 * @param      callback  Public to call.
 * @param      context   Value the public receives.
 *
 * @return     - `true` the work started
 *             - `false` `callback` is not a public of the script
 */
native bool:MySQL_VerifyPassword(const password[], const hash[], const callback[], context = 0);

/**
 * # Callbacks
 */

/**
 * @brief      A query failed, before its public is called.
 *
 * @param      handle    Handle it was sent through.
 * @param      errorid   Error number of MySQL.
 * @param      error     Message of the error.
 * @param      query     Start of the query, up to 1024 characters.
 * @param      callback  Public of the query, empty when it has none.
 */
forward OnMySQLError(MySQL:handle, errorid, const error[], const query[], const callback[]);
