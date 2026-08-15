/* Reads shared/fixtures/payload_digest.json.
 *
 * The tests parse the fixture with the same jsmn wrapper core uses. Pulling in a
 * second JSON parser just for tests would mean the test suite and the code under
 * test disagree about what a document says, which is exactly the class of bug this
 * fixture exists to catch.
 */
#ifndef DAEMOON_TEST_FIXTURE_JSON_H
#define DAEMOON_TEST_FIXTURE_JSON_H

#include <stddef.h>

size_t fixture_digest_case_count(const char *json, size_t len);

/* Case i: its name, expected digest, expected total size and entry count.
 * Returns 0 on success. */
int fixture_digest_case(const char *json, size_t len, size_t i, char *name, size_t name_cap,
                        char *sha256, unsigned long long *out_size, size_t *out_nentries);

/* Entry e of case i. Returns 0 on success. */
int fixture_digest_entry(const char *json, size_t len, size_t i, size_t e, char *path,
                         size_t path_cap, char *content, size_t content_cap);

#endif /* DAEMOON_TEST_FIXTURE_JSON_H */
