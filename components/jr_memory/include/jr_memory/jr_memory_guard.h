/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure validation and secret-rejection policy for jr_memory.  This header has
 * no ESP-IDF dependencies so the guard can be exercised on a host compiler.
 */
#ifndef JR_MEMORY_GUARD_H
#define JR_MEMORY_GUARD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_MEMORY_KEY_MAX    32U
#define JR_MEMORY_VALUE_MAX  192U

typedef enum {
    JR_MEMORY_VALID = 0,
    JR_MEMORY_INVALID_ARGUMENT,
    JR_MEMORY_KEY_EMPTY,
    JR_MEMORY_KEY_TOO_LONG,
    JR_MEMORY_KEY_INVALID,
    JR_MEMORY_VALUE_EMPTY,
    JR_MEMORY_VALUE_TOO_LONG,
    JR_MEMORY_VALUE_INVALID,
    JR_MEMORY_SECRET_REJECTED,
} jr_memory_validation_t;

/* Conservative credential guard.  It intentionally rejects credential-like
 * field names, assignments, private-key material, bearer/JWT tokens, common
 * provider token prefixes, credential-bearing URLs, and opaque token-shaped
 * values.  It is defense in depth, not a substitute for caller policy. */
bool jr_memory_is_secret_like(const char *key, const char *value);

/* Valid facts use a short ASCII key and a one-line, valid UTF-8 value. */
jr_memory_validation_t jr_memory_validate_fact(const char *key,
                                                const char *value);
const char *jr_memory_validation_name(jr_memory_validation_t result);

#ifdef __cplusplus
}
#endif

#endif /* JR_MEMORY_GUARD_H */

