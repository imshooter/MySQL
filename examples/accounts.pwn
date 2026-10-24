#include <open.mp>
#include <mysql>

/**
 * Accounts kept in MySQL. /register and /login take a password hashed with Argon2id on the threads
 * of the component, the lookups and the inserts are prepared statements, and the score of every
 * logged in player is saved in one batch every minute and when they leave.
 *
 * The settings of the connection are read from mysql.ini in the folder of the server:
 *
 *     hostname = 127.0.0.1
 *     username = root
 *     password = ${MYSQL_PASSWORD}
 *     database = game
 */

static
    MySQL:gDatabase = INVALID_MYSQL_ID,
    Statement:gFindAccount = INVALID_STATEMENT_ID,
    Statement:gAddAccount = INVALID_STATEMENT_ID,
    Statement:gSaveScore = INVALID_STATEMENT_ID,
    gAccount[MAX_PLAYERS],
    gHash[MAX_PLAYERS][MYSQL_HASH_LENGTH],
    bool:gLoggedIn[MAX_PLAYERS]
;

forward OnAccountFound(playerid);
forward OnRegisterHashed(playerid, const hash[]);
forward OnAccountAdded(playerid);
forward OnLoginChecked(playerid, bool:match);
forward OnScoresSaved(players);
forward SaveScores();

main() {}

public OnGameModeInit() {
    gDatabase = MySQL_ConnectFile();
    MySQL_Run(gDatabase, "\
        CREATE TABLE IF NOT EXISTS accounts ( \
            id INT AUTO_INCREMENT PRIMARY KEY, \
            name VARCHAR(24) NOT NULL UNIQUE, \
            hash VARCHAR(128) NOT NULL, \
            score INT NOT NULL DEFAULT 0 \
        )\
    ");

    gFindAccount = MySQL_Prepare(gDatabase, "SELECT id, hash, score FROM accounts WHERE name = %q");
    gAddAccount = MySQL_Prepare(gDatabase, "INSERT INTO accounts (name, hash) VALUES (%q, %q)");
    gSaveScore = MySQL_Prepare(gDatabase, "UPDATE accounts SET score = %i WHERE id = %i");

    SetTimer("SaveScores", 60000, true);

    return 1;
}

public OnPlayerConnect(playerid) {
    new
        name[MAX_PLAYER_NAME + 1]
    ;

    gAccount[playerid] = 0;
    gLoggedIn[playerid] = false;
    gHash[playerid][0] = EOS;
    GetPlayerName(playerid, name, sizeof (name));
    MySQL_Execute(gFindAccount, "OnAccountFound", playerid, name);

    return 1;
}

public OnAccountFound(playerid) {
    if (MySQL_GetRowCount() == 0) {
        SendClientMessage(playerid, -1, "Register with /register <password>.");

        return 1;
    }

    MySQL_GetString(0, "hash", gHash[playerid]);
    SetPlayerScore(playerid, MySQL_GetInt(0, "score"));
    gAccount[playerid] = MySQL_GetInt(0, "id");
    SendClientMessage(playerid, -1, "Log in with /login <password>.");

    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[]) {
    if (strcmp(cmdtext, "/register ", true, 10) == 0 && cmdtext[10] != EOS) {
        if (gHash[playerid][0] != EOS) {
            return SendClientMessage(playerid, -1, "This name has an account. Use /login.");
        }

        MySQL_HashArgon2(cmdtext[10], "OnRegisterHashed", playerid);

        return 1;
    }

    if (strcmp(cmdtext, "/login ", true, 7) == 0 && cmdtext[7] != EOS) {
        if (gAccount[playerid] == 0 || gLoggedIn[playerid]) {
            return SendClientMessage(playerid, -1, "There is nothing to log in to.");
        }

        MySQL_VerifyPassword(cmdtext[7], gHash[playerid], "OnLoginChecked", playerid);

        return 1;
    }

    return 0;
}

public OnRegisterHashed(playerid, const hash[]) {
    new
        name[MAX_PLAYER_NAME + 1]
    ;

    if (hash[0] == EOS) {
        return SendClientMessage(playerid, -1, "The password could not be secured.");
    }

    GetPlayerName(playerid, name, sizeof (name));
    strcopy(gHash[playerid], hash);
    MySQL_Execute(gAddAccount, "OnAccountAdded", playerid, name, hash);

    return 1;
}

public OnAccountAdded(playerid) {
    if (MySQL_GetErrno() != 0) {
        return SendClientMessage(playerid, -1, "The account could not be made.");
    }

    gAccount[playerid] = MySQL_GetInsertID();
    gLoggedIn[playerid] = true;
    SendClientMessage(playerid, -1, "Registered and logged in.");

    return 1;
}

public OnLoginChecked(playerid, bool:match) {
    if (!match) {
        return SendClientMessage(playerid, -1, "Wrong password.");
    }

    gLoggedIn[playerid] = true;
    SendClientMessage(playerid, -1, "Logged in.");

    return 1;
}

public SaveScores() {
    new const
        Batch:batch = MySQL_CreateBatch(gSaveScore)
    ;

    for (new i; i != MAX_PLAYERS; ++i) {
        if (IsPlayerConnected(i) && gLoggedIn[i]) {
            MySQL_AddRow(batch, GetPlayerScore(i), gAccount[i]);
        }
    }

    MySQL_ExecuteBatch(batch, "OnScoresSaved", MySQL_GetBatchRows(batch));

    return 1;
}

public OnScoresSaved(players) {
    if (MySQL_GetErrno() == 0 && players != 0) {
        printf("Saved the score of %i players.", players);
    }

    return 1;
}

public OnPlayerDisconnect(playerid, reason) {
    if (gLoggedIn[playerid]) {
        MySQL_Execute(gSaveScore, "", 0, GetPlayerScore(playerid), gAccount[playerid]);
    }

    gLoggedIn[playerid] = false;

    return 1;
}
