# MySQL

> [!NOTE]
> MySQL is experimental and already runs in production. Its natives may still change from one version to the next. It was made with Claude Opus 5.5, a model by [Anthropic](https://www.anthropic.com).

An open.mp component for MySQL and MariaDB. Queries run on threads of their own and answer in a public on the next server tick, prepared statements send rows in bulk, JSON cells are read without a parser in Pawn, and passwords are hashed with Argon2id or bcrypt off the game thread.

## Features

- **Queries** in order on one connection, in parallel on a pool, or waited for. The component formats them, with no length limit, and escapes strings for the connection.
- **Queries built in pieces**, of any length, joined into one with `%Q`.
- **Prepared statements** with the same specifiers as queries. A batch gathers rows and sends them to MariaDB as arrays in one command, split to fit `max_allowed_packet`, inside a transaction when it takes more than one.
- **Results** read by field name or position, several result sets, results kept past their callback, and `MySQL_GetRow` to read a row into variables in one call.
- **JSON cells** read by key or JSON Pointer, parsed once when first read. Text in the code page of the game is converted to and from UTF-8.
- **Passwords** hashed with Argon2id or bcrypt and verified on threads of the component.
- **Limits** on the length of a query, the queries waiting and the queries per second, and **timeouts** for connecting, reading and writing.
- **TLS** that never falls back to plain text once asked for, with the certificate checked against an authority or a fingerprint.
- Settings in a file, with `${NAME}` taken from the environment, so a password stays out of it.

## Installation

1. Download the latest release from [Releases](https://github.com/imshooter/MySQL/releases).
2. Place `mysql.dll` in the `components` folder of the server.
3. Place `mysql.inc` in the include folder of the compiler.

No plugin is needed. Tested with open.mp 1.6.7 and MariaDB 10.4 on Windows. Only a Windows build is available for now.

## A first script

```pawn
#include <open.mp>
#include <mysql>

new MySQL:gDatabase;

forward OnScoreRead(playerid);

public OnGameModeInit() {
    gDatabase = MySQL_ConnectFile("mysql.ini");

    return 1;
}

public OnPlayerConnect(playerid) {
    new
        name[MAX_PLAYER_NAME + 1]
    ;

    GetPlayerName(playerid, name, sizeof (name));
    MySQL_ThreadedQuery(gDatabase, "OnScoreRead", playerid, "SELECT score FROM players WHERE name = %q", name);

    return 1;
}

public OnScoreRead(playerid) {
    if (MySQL_GetRowCount() != 0) {
        SetPlayerScore(playerid, MySQL_GetInt(0, "score"));
    }

    return 1;
}
```

`mysql.ini`, in the folder of the server:

```ini
hostname = 127.0.0.1
username = game
password = ${MYSQL_PASSWORD}
database = game
```

[`examples/accounts.pwn`](examples/accounts.pwn) registers and logs players in with Argon2id hashes, looks them up with prepared statements and saves every score in one batch.

## Prepared statements

```pawn
new
    MySQL:gDatabase,
    Statement:gSaveScore,
    gPlayerId[MAX_PLAYERS]
;

stock PrepareStatements() {
    gSaveScore = MySQL_PrepareStatement(gDatabase, "UPDATE players SET score = %i WHERE id = %i");
}

stock SaveScores() {
    new const
        Batch:batch = MySQL_CreateBatch(gSaveScore)
    ;

    for (new i; i != MAX_PLAYERS; ++i) {
        if (IsPlayerConnected(i)) {
            MySQL_AddBatchRow(batch, GetPlayerScore(i), gPlayerId[i]);
        }
    }

    MySQL_ExecuteBatch(batch);
}
```

The values are copied when a row is added, and never escaped or parsed by the server again. A statement is prepared again when its connection is lost.

## Performance

Measured on one computer against MariaDB 10.4 on the same machine, with scripts compiled with `-d3`.

| | R41 with pawn-json | MySQL |
| --- | --- | --- |
| Six cells of a row | 1.2 µs | 0.7 µs |
| Four attributes of a JSON cell | 5.4 µs | 0.9 µs |
| A formatted query sent | 1.6 µs | 0.6 µs |
| A 1000 row insert built in pieces | 2.75 ms | 0.95 ms |

Saving 2000 items of a gamemode, each a row with JSON attributes:

| | Game thread | Until written |
| --- | --- | --- |
| R41 with pawn-json | 57 ms | 98 ms |
| MySQL, one query built in pieces | 17 ms | 63 ms |
| MySQL, prepared statements in batches | 16 ms | 45 ms |

An Argon2id hash of 19 MiB and two passes takes about 36 ms. See [Performance](https://github.com/imshooter/MySQL/wiki/Performance) in the wiki for the rest.

## Documentation

The [wiki](https://github.com/imshooter/MySQL/wiki) has a page for every native, the settings and guides to queries, results, JSON cells, prepared statements, passwords and TLS.

## Tests

The tests in [`tests`](tests) run as a gamemode against a database, and shut the server down when they end. Set the database at the top of [`tests/test.inc`](tests/test.inc), or give the port to the compiler as `TEST_PORT=3307`.

- [`tests/queries.pwn`](tests/queries.pwn): queries, results, JSON cells and built queries.
- [`tests/statements.pwn`](tests/statements.pwn): prepared statements and batches.
- [`tests/passwords.pwn`](tests/passwords.pwn): hashes and known vectors of Argon2 and bcrypt.
- [`tests/settings.pwn`](tests/settings.pwn): limits, timeouts, environment variables and required TLS, against a server without TLS.

## Building

Windows with Visual Studio 2022 or its Build Tools, and CMake 3.19 or later. CMake fetches the open.mp SDK, the Pawn headers and MariaDB Connector/C 3.4.11.

```
cmake -S . -B build -A Win32
cmake --build build --config Release --target mysql
```

The component is written to `build/Release/mysql.dll`. The server is 32 bit, so the component must be too.

## Thanks to

- [@mariadb-corporation](https://github.com/mariadb-corporation) for [MariaDB Connector/C](https://github.com/mariadb-corporation/mariadb-connector-c), linked into the component under the LGPL 2.1.
- [@ibireme](https://github.com/ibireme) for [yyjson](https://github.com/ibireme/yyjson), which reads the JSON cells.
- [@P-H-C](https://github.com/P-H-C) for the reference implementation of [Argon2](https://github.com/P-H-C/phc-winner-argon2).
- [@solardiz](https://github.com/solardiz) for [crypt_blowfish](https://www.openwall.com/crypt/).
- [@openmultiplayer](https://github.com/openmultiplayer) for the [SDK](https://github.com/openmultiplayer/open.mp-sdk) and the [Pawn compiler](https://github.com/openmultiplayer/compiler).

## License

Released under the [MIT License](LICENSE). The libraries in [`external`](external) keep their own licenses, and MariaDB Connector/C is under the LGPL 2.1.
