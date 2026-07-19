#include "jr_memory/jr_memory_guard.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void accepts_ordinary_facts(void)
{
    assert(jr_memory_validate_fact("preferred drink", "Earl Grey, hot.") ==
           JR_MEMORY_VALID);
    assert(jr_memory_validate_fact("home.city", "Fort Lauderdale") ==
           JR_MEMORY_VALID);
    assert(jr_memory_validate_fact("greeting", "Bonjour, Pascal \xE2\x80\x94 welcome back.") ==
           JR_MEMORY_VALID);
    assert(!jr_memory_is_secret_like("project", "The password manager is 1Password."));
    assert(jr_memory_validate_fact("favorite cookie", "Chocolate chip") ==
           JR_MEMORY_VALID);
    assert(jr_memory_validate_fact("travel region", "Asia") == JR_MEMORY_VALID);
}

static void rejects_secret_keys(void)
{
    static const char *const keys[] = {
        "password", "WiFi Password", "github_token", "llm-api-key",
        "client secret", "private_key", "recovery phrase", "session_id",
        "cookie", "otp",
    };
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        assert(jr_memory_validate_fact(keys[i], "not printed") ==
               JR_MEMORY_SECRET_REJECTED);
    }
}

static void rejects_secret_values(void)
{
    static const char *const values[] = {
        "password=hunter2",
        "The token is abcdefghijklmnop",
        "Bearer abcdefghijklmnop",
        "sk-proj-abcdefghijklmnop",
        "ghp_abcdefghijklmnopqrstuvwxyz",
        "AKIAABCDEFGHIJKLMNOP",
        "AIzaabcdefghijklmnopqrstuvwxyz012345678",
        "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abcdefghijklmnopqrstuvwxyz",
        "postgres://user:pass@example.test/db",
        "cookie=sessionvalue",
        "-----BEGIN PRIVATE KEY----- material",
        "-----BEGIN EC PRIVATE KEY----- material",
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/",
    };
    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
        assert(jr_memory_validate_fact("note", values[i]) ==
               JR_MEMORY_SECRET_REJECTED);
    }
}

static void rejects_malformed_facts(void)
{
    char long_key[JR_MEMORY_KEY_MAX + 2U];
    memset(long_key, 'a', sizeof(long_key) - 1U);
    long_key[sizeof(long_key) - 1U] = '\0';
    assert(jr_memory_validate_fact(long_key, "value") == JR_MEMORY_KEY_TOO_LONG);
    assert(jr_memory_validate_fact("bad/key", "value") == JR_MEMORY_KEY_INVALID);
    assert(jr_memory_validate_fact("empty", "   ") == JR_MEMORY_VALUE_EMPTY);
    assert(jr_memory_validate_fact("line", "first\nsecond") == JR_MEMORY_VALUE_INVALID);
    assert(jr_memory_validate_fact("utf8", "\xC0\xAF") == JR_MEMORY_VALUE_INVALID);
}

int main(void)
{
    accepts_ordinary_facts();
    rejects_secret_keys();
    rejects_secret_values();
    rejects_malformed_facts();
    puts("jr_memory guard tests passed");
    return 0;
}
