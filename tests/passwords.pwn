#include <open.mp>
#include <mysql>
#include "test.inc"

static const gTests[][MAX_TEST_NAME] = {
    "HashesWithArgon2",
    "VerifiesArgon2",
    "HashesWithBcrypt",
    "VerifiesBcrypt",
    "ReadsKnownHashes",
    "RefusesBadInput"
};

/**
 * Password hashing: hashes made by the component, verified right and wrong, known Argon2 and bcrypt
 * hashes, and the costs it refuses. Needs no database.
 */

static
    gArgon2[MYSQL_HASH_LENGTH],
    gBcrypt[MYSQL_HASH_LENGTH],
    gAnswers
;

forward OnArgon2Hashed(context, const hash[]);
forward OnBcryptHashed(context, const hash[]);
forward OnChecked(context, bool:match);
forward OnNever(context, const hash[]);

test HashesWithArgon2() {
    Test_Async(5000);
    ASSERT(MySQL_HashArgon2("secret123", "OnArgon2Hashed"));
}

public OnArgon2Hashed(context, const hash[]) {
    new const
        prefix[] = "$argon2id$v=19$m=19456,t=2,p=1$"
    ;

    ASSERT_EQ(strfind(hash, prefix), 0);
    strcopy(gArgon2, hash);
    Test_Done();

    return 1;
}

test VerifiesArgon2() {
    gAnswers = 0;
    Test_Async(5000);
    ASSERT(MySQL_VerifyPassword("secret123", gArgon2, "OnChecked", 1));
    ASSERT(MySQL_VerifyPassword("secret124", gArgon2, "OnChecked", 0));
}

public OnChecked(context, bool:match) {
    ASSERT_EQ(match, bool:context);

    if (++gAnswers == 2) {
        Test_Done();
    }

    return 1;
}

test HashesWithBcrypt() {
    Test_Async(5000);
    ASSERT(MySQL_HashBcrypt("secret123", "OnBcryptHashed"));
}

public OnBcryptHashed(context, const hash[]) {
    new const
        prefix[] = "$2b$12$"
    ;

    ASSERT_EQ(strfind(hash, prefix), 0);
    ASSERT_EQ(strlen(hash), 60);
    strcopy(gBcrypt, hash);
    Test_Done();

    return 1;
}

test VerifiesBcrypt() {
    gAnswers = 0;
    Test_Async(5000);
    ASSERT(MySQL_VerifyPassword("secret123", gBcrypt, "OnChecked", 1));
    ASSERT(MySQL_VerifyPassword("secret124", gBcrypt, "OnChecked", 0));
}

test ReadsKnownHashes() {
    new const
        bcrypt[] = "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW",
        argon2i[] = "$argon2i$v=19$m=65536,t=2,p=4$c29tZXNhbHQ$RdescudvJCsgt3ub+b+dWRWJTmaaJObG"
    ;

    gAnswers = 0;
    Test_Async(5000);
    ASSERT(MySQL_VerifyPassword("U*U", bcrypt, "OnChecked", 1));
    ASSERT(MySQL_VerifyPassword("password", argon2i, "OnChecked", 1));
}

test RefusesBadInput() {
    gAnswers = 1;
    ASSERT_FALSE(MySQL_HashArgon2("x", "OnMissing"));
    ASSERT_FALSE(MySQL_HashArgon2("x", "OnNever", 0, 100));
    ASSERT_FALSE(MySQL_HashArgon2("x", "OnNever", 0, MYSQL_ARGON2_MEMORY, 0));
    ASSERT_FALSE(MySQL_HashBcrypt("x", "OnNever", 0, 3));
    ASSERT_FALSE(MySQL_HashBcrypt("x", "OnNever", 0, 21));
    Test_Async(5000);
    ASSERT(MySQL_VerifyPassword("x", "not a hash", "OnChecked", 0));
}

public OnNever(context, const hash[]) {
    return 1;
}

public OnGameModeInit() {
    Test_Run(gTests);

    return 1;
}

main() {}
