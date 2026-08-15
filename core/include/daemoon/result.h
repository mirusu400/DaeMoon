/* result.h - the one error type used everywhere in core.
 *
 * Every core function that can fail returns daemoon_result_t. DAEMOON_OK is 0 and
 * every failure is a code from shared/errors.json, so a failure that came off the
 * wire and a failure that happened locally are the same type and render through the
 * same language table.
 *
 * Never swallow an error. A libctru or libnx Result that is not checked and
 * propagated is a review blocker; convert it with DAEMOON_FROM_BACKEND at the
 * platform boundary.
 */
#ifndef DAEMOON_RESULT_H
#define DAEMOON_RESULT_H

#include <stddef.h>

#include <daemoon/error_codes.h> /* generated from shared/errors.json */
#include <daemoon/str_ids.h>     /* generated from shared/lang/en.json */

#ifdef __cplusplus
extern "C" {
#endif

typedef int daemoon_result_t;

/* The wire code, e.g. "version_conflict". Never NULL. */
const char *daemoon_result_code(daemoon_result_t r);

/* Parse a wire code. len may be 0 for a NUL terminated string. An unrecognised
 * code maps to DAEMOON_ERR_INTERNAL_ERROR and never to DAEMOON_OK: a newer server
 * must not be able to make an old client believe a failure succeeded. */
daemoon_result_t daemoon_result_from_code(const char *code, size_t len);

/* HTTP status plus the code from the response body. The body wins; the status is
 * only used when there is no usable body. */
daemoon_result_t daemoon_result_from_http(int status, const char *code, size_t len);

/* The language table entry for this failure ("err.<code>"). */
daemoon_str_id_t daemoon_result_str_id(daemoon_result_t r);

/* Whether retrying the same request can plausibly succeed. Retrying still needs a
 * ceiling and a timeout; there are no unbounded waits anywhere in this codebase. */
int daemoon_result_retryable(daemoon_result_t r);

/* Propagate a failure unchanged. */
#define DAEMOON_TRY(expr)                        \
    do {                                         \
        daemoon_result_t daemoon_try_r_ = (expr); \
        if (daemoon_try_r_ != DAEMOON_OK) {      \
            return daemoon_try_r_;               \
        }                                        \
    } while (0)

/* Propagate, running a cleanup step first. Use this at any point where a partial
 * write would be left behind. */
#define DAEMOON_TRY_CLEANUP(expr, cleanup)       \
    do {                                         \
        daemoon_result_t daemoon_try_r_ = (expr); \
        if (daemoon_try_r_ != DAEMOON_OK) {      \
            { cleanup; }                         \
            return daemoon_try_r_;               \
        }                                        \
    } while (0)

#define DAEMOON_REQUIRE(cond, err)               \
    do {                                         \
        if (!(cond)) {                           \
            return (err);                        \
        }                                        \
    } while (0)

/* A platform Result that failed. The numeric value is lost on purpose: it is
 * platform specific and belongs in a log line, not in core control flow. */
#define DAEMOON_FROM_BACKEND(res) \
    ((void)(res), DAEMOON_ERR_BACKEND_ERROR)

#ifdef __cplusplus
}
#endif

#endif /* DAEMOON_RESULT_H */
