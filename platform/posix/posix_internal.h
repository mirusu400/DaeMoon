/* Shared between the posix backend translation units. Not installed anywhere and
 * not visible to core, which must not know that a filesystem exists. */
#ifndef DAEMOON_POSIX_INTERNAL_H
#define DAEMOON_POSIX_INTERNAL_H

#include <daemoon/backend.h>

/* errno to the closest wire code. Anything unrecognised becomes io_error rather
 * than something more specific and wrong. */
daemoon_result_t daemoon_posix_errno(int e);

/* Opens a file as a seekable stream. Creates parent directories on write, which is
 * what daemoon_archive_unpack relies on for nested save paths. */
daemoon_result_t daemoon_posix_open_stream(const char *path, daemoon_open_mode_t mode,
                                           unsigned *write_counter, daemoon_stream_t **out);

daemoon_result_t daemoon_posix_mkdir_p(const char *path);
daemoon_result_t daemoon_posix_mkdir_parents(const char *path);

#endif /* DAEMOON_POSIX_INTERNAL_H */
