/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_memory/jr_memory_guard.h"

#include <stdint.h>
#include <string.h>

static size_t bounded_len(const char *s, size_t cap)
{
    size_t len = 0U;
    if (s == NULL) {
        return 0U;
    }
    while (len < cap && s[len] != '\0') {
        ++len;
    }
    return len;
}

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool ascii_alnum(unsigned char c)
{
    c = ascii_lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool ci_equal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0U; i < n; ++i) {
        if (a[i] == '\0' || b[i] == '\0' ||
            ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

static const char *ci_find(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || *needle == '\0') {
        return NULL;
    }
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p) {
        if (ci_equal_n(p, needle, nlen)) {
            return p;
        }
    }
    return NULL;
}

static bool key_has_secret_marker(const char *key)
{
    static const char *const markers[] = {
        "password", "passwd", "passcode", "secret", "token", "apikey",
        "authorization", "credential", "privatekey", "clientsecret",
        "accesskey", "sessionkey", "sessiontoken", "sessionid",
        "sessioncookie", "authcookie", "wifikey", "seedphrase",
        "recoveryphrase", "mnemonic", "bearer", "oauth", "otp",
    };
    char normalized[JR_MEMORY_KEY_MAX + 1U];
    size_t used = 0U;
    for (const unsigned char *p = (const unsigned char *)key;
         p != NULL && *p != '\0' && used < JR_MEMORY_KEY_MAX; ++p) {
        if (ascii_alnum(*p)) {
            normalized[used++] = (char)ascii_lower(*p);
        }
    }
    normalized[used] = '\0';

    if (strcmp(normalized, "pin") == 0 || strcmp(normalized, "pwd") == 0 ||
        strcmp(normalized, "auth") == 0 || strcmp(normalized, "cookie") == 0) {
        return true;
    }
    for (size_t i = 0U; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        if (strstr(normalized, markers[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static bool value_has_assignment(const char *value, const char *label,
                                 bool allow_natural_is)
{
    const size_t label_len = strlen(label);
    const char *at = value;
    while ((at = ci_find(at, label)) != NULL) {
        const unsigned char before = at == value ? 0U : (unsigned char)at[-1];
        const char *p = at + label_len;
        const unsigned char after = (unsigned char)*p;
        const bool before_boundary = before == 0U || !ascii_alnum(before);
        const bool after_boundary = after == 0U || !ascii_alnum(after);
        if (before_boundary && after_boundary) {
            while (*p == ' ' || *p == '\t' || *p == '\"' || *p == '\'') {
                ++p;
            }
            if (*p == ':' || *p == '=') {
                return true;
            }
            if (allow_natural_is && ci_equal_n(p, "is", 2U) &&
                !ascii_alnum((unsigned char)p[2])) {
                return true;
            }
        }
        ++at;
    }
    return false;
}

static bool token_prefix_at_boundary(const char *value, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    for (const char *p = value; *p != '\0'; ++p) {
        const unsigned char before = p == value ? 0U : (unsigned char)p[-1];
        if ((before == 0U || !ascii_alnum(before)) && ci_equal_n(p, prefix, prefix_len)) {
            return true;
        }
    }
    return false;
}

static bool is_base64url(unsigned char c);

static bool looks_like_aws_access_key(const char *value)
{
    for (const char *p = value; *p != '\0'; ++p) {
        const unsigned char before = p == value ? 0U : (unsigned char)p[-1];
        if (before != 0U && ascii_alnum(before)) {
            continue;
        }
        if (strncmp(p, "AKIA", 4U) != 0 && strncmp(p, "ASIA", 4U) != 0) {
            continue;
        }
        size_t suffix = 0U;
        while (suffix < 16U) {
            const unsigned char c = (unsigned char)p[4U + suffix];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                break;
            }
            ++suffix;
        }
        if (suffix == 16U && !ascii_alnum((unsigned char)p[20])) {
            return true;
        }
    }
    return false;
}

static bool looks_like_google_api_key(const char *value)
{
    for (const char *p = value; *p != '\0'; ++p) {
        const unsigned char before = p == value ? 0U : (unsigned char)p[-1];
        if ((before == 0U || !ascii_alnum(before)) && strncmp(p, "AIza", 4U) == 0) {
            size_t token_len = 4U;
            while (is_base64url((unsigned char)p[token_len])) {
                ++token_len;
            }
            if (token_len >= 24U) {
                return true;
            }
        }
    }
    return false;
}

static bool is_base64url(unsigned char c)
{
    return ascii_alnum(c) || c == '-' || c == '_';
}

static bool looks_like_jwt(const char *value)
{
    for (const char *p = value; *p != '\0'; ++p) {
        const char *q = p;
        size_t a = 0U;
        size_t b = 0U;
        size_t c = 0U;
        while (is_base64url((unsigned char)*q)) {
            ++a;
            ++q;
        }
        if (a < 8U || *q++ != '.') {
            continue;
        }
        while (is_base64url((unsigned char)*q)) {
            ++b;
            ++q;
        }
        if (b < 8U || *q++ != '.') {
            continue;
        }
        while (is_base64url((unsigned char)*q)) {
            ++c;
            ++q;
        }
        if (c >= 8U) {
            return true;
        }
    }
    return false;
}

static bool looks_like_opaque_token(const char *value)
{
    size_t chars = 0U;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (*p == ' ') {
            continue;
        }
        if (!is_base64url(*p) && *p != '+' && *p != '/' && *p != '=') {
            return false;
        }
        ++chars;
    }
    return chars >= 40U;
}

static bool has_credential_url(const char *value)
{
    const char *scheme = strstr(value, "://");
    if (scheme == NULL) {
        return false;
    }
    const char *authority = scheme + 3;
    const char *end = authority;
    while (*end != '\0' && *end != '/' && *end != '?' && *end != '#') {
        ++end;
    }
    for (const char *p = authority; p < end; ++p) {
        if (*p == '@') {
            return true;
        }
    }
    return false;
}

static bool valid_utf8_line(const char *value, size_t len, bool *non_space)
{
    *non_space = false;
    for (size_t i = 0U; i < len;) {
        const uint8_t c = (uint8_t)value[i];
        if (c < 0x20U || c == 0x7FU) {
            return false;
        }
        if (c < 0x80U) {
            if (c != ' ') {
                *non_space = true;
            }
            ++i;
            continue;
        }

        size_t need = 0U;
        uint32_t codepoint = 0U;
        if (c >= 0xC2U && c <= 0xDFU) {
            need = 1U;
            codepoint = c & 0x1FU;
        } else if (c >= 0xE0U && c <= 0xEFU) {
            need = 2U;
            codepoint = c & 0x0FU;
        } else if (c >= 0xF0U && c <= 0xF4U) {
            need = 3U;
            codepoint = c & 0x07U;
        } else {
            return false;
        }
        if (i + need >= len) {
            return false;
        }
        for (size_t j = 1U; j <= need; ++j) {
            const uint8_t continuation = (uint8_t)value[i + j];
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        if ((need == 2U && codepoint < 0x800U) ||
            (need == 3U && codepoint < 0x10000U) ||
            (codepoint >= 0x80U && codepoint <= 0x9FU) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU) {
            return false;
        }
        *non_space = true;
        i += need + 1U;
    }
    return true;
}

bool jr_memory_is_secret_like(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return false;
    }
    if (key_has_secret_marker(key)) {
        return true;
    }

    static const char *const labels[] = {
        "password", "passwd", "passcode", "secret", "token", "api_key",
        "api-key", "apikey", "authorization", "credential", "private_key",
        "private-key", "client_secret", "client-secret", "access_key",
        "access-key", "access_token", "access-token", "session_key",
        "session-key", "session_token", "session-token", "session_id",
        "session-id", "wifi_key", "wifi-key", "seed_phrase",
        "seed-phrase", "mnemonic",
    };
    for (size_t i = 0U; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        if (value_has_assignment(value, labels[i], true)) {
            return true;
        }
    }
    if (value_has_assignment(value, "cookie", false)) {
        return true;
    }

    static const char *const prefixes[] = {
        "bearer ", "sk-", "ghp_", "gho_", "ghu_", "ghs_", "github_pat_",
        "xoxb-", "xoxp-", "xoxa-",
    };
    for (size_t i = 0U; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (token_prefix_at_boundary(value, prefixes[i])) {
            return true;
        }
    }

    return ci_find(value, "-----BEGIN PRIVATE KEY-----") != NULL ||
           ci_find(value, "-----BEGIN RSA PRIVATE KEY-----") != NULL ||
           ci_find(value, "-----BEGIN EC PRIVATE KEY-----") != NULL ||
           ci_find(value, "-----BEGIN ENCRYPTED PRIVATE KEY-----") != NULL ||
           ci_find(value, "-----BEGIN OPENSSH PRIVATE KEY-----") != NULL ||
           has_credential_url(value) || looks_like_aws_access_key(value) ||
           looks_like_google_api_key(value) || looks_like_jwt(value) ||
           looks_like_opaque_token(value);
}

jr_memory_validation_t jr_memory_validate_fact(const char *key,
                                                const char *value)
{
    if (key == NULL || value == NULL) {
        return JR_MEMORY_INVALID_ARGUMENT;
    }
    const size_t key_len = bounded_len(key, JR_MEMORY_KEY_MAX + 1U);
    const size_t value_len = bounded_len(value, JR_MEMORY_VALUE_MAX + 1U);
    if (key_len == 0U) {
        return JR_MEMORY_KEY_EMPTY;
    }
    if (key_len > JR_MEMORY_KEY_MAX) {
        return JR_MEMORY_KEY_TOO_LONG;
    }
    if (value_len == 0U) {
        return JR_MEMORY_VALUE_EMPTY;
    }
    if (value_len > JR_MEMORY_VALUE_MAX) {
        return JR_MEMORY_VALUE_TOO_LONG;
    }
    if (!ascii_alnum((unsigned char)key[0]) ||
        !ascii_alnum((unsigned char)key[key_len - 1U])) {
        return JR_MEMORY_KEY_INVALID;
    }
    for (size_t i = 0U; i < key_len; ++i) {
        const unsigned char c = (unsigned char)key[i];
        if (!ascii_alnum(c) && c != ' ' && c != '_' && c != '-' && c != '.') {
            return JR_MEMORY_KEY_INVALID;
        }
    }
    bool non_space = false;
    if (!valid_utf8_line(value, value_len, &non_space)) {
        return JR_MEMORY_VALUE_INVALID;
    }
    if (!non_space) {
        return JR_MEMORY_VALUE_EMPTY;
    }
    if (jr_memory_is_secret_like(key, value)) {
        return JR_MEMORY_SECRET_REJECTED;
    }
    return JR_MEMORY_VALID;
}

const char *jr_memory_validation_name(jr_memory_validation_t result)
{
    static const char *const names[] = {
        "valid", "invalid_argument", "key_empty", "key_too_long",
        "key_invalid", "value_empty", "value_too_long", "value_invalid",
        "secret_rejected",
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    return (size_t)result < count ? names[result] : "unknown";
}
