/* Enough of libctru to compile and run platform/3ds/source/save_backend.c on a
 * desktop.
 *
 * This does not test libctru, and it is not an emulator. It tests the code in the
 * 3DS backend: how paths are built, how the tree is walked, whether a truncated
 * conversion is caught, whether remove_all clears everything, whether a handle
 * opened read only refuses to write. All of that is ours, all of it is where the
 * mistakes were, and none of it needs a console to be wrong.
 *
 * What it cannot tell you is whether the FS service behaves like this. That is
 * what docs/phase1-hardware.md is for, and the conformance suite runs on both.
 *
 * Deliberately strict where the real service is strict: the converters truncate
 * and report the length they would have needed, and reading a directory whose
 * entries are being deleted is not something this promises to survive either.
 */
#ifndef DAEMOON_CTRU_STUB_3DS_H
#define DAEMOON_CTRU_STUB_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* unsigned long long rather than uint64_t: on the 3DS uint64_t *is* unsigned long
 * long, and on a 64-bit desktop it is unsigned long. Matching the console keeps a
 * pointer to a u64 compatible with the same declarations there and here, which is
 * the whole point of compiling this code twice. */
typedef unsigned long long u64;
typedef int32_t  Result;
typedef u32      Handle;

#define R_SUCCEEDED(res) ((res) >= 0)
#define R_FAILED(res)    ((res) < 0)

/* The real macros pull the fields out of a packed result code. The stub encodes
 * the same fields so the backend's mapping runs for real. */
#define R_SUMMARY(res)     (((res) >> 21) & 0x3f)
#define R_DESCRIPTION(res) ((res) & 0x3ff)

#define DAEMOON_STUB_RESULT(summary, description) \
    ((Result)(0x80000000u | ((u32)(summary) << 21) | ((u32)(description) & 0x3ff)))

/* libctru's own numbering. The stub had its own, which meant the backend's
 * mapping from a summary to a wire code was exercised against values the console
 * never produces - a test that agreed with itself and with nothing else. */
enum {
    RS_SUCCESS = 0,
    RS_NOP = 1,
    RS_WOULDBLOCK = 2,
    RS_OUTOFRESOURCE = 3,
    RS_NOTFOUND = 4,
    RS_INVALIDSTATE = 5,
    RS_NOTSUPPORTED = 6,
    RS_INVALIDARG = 7,
    RS_WRONGARG = 8,
    RS_CANCELED = 9,
    RS_STATUSCHANGED = 10,
    RS_INTERNAL = 11
};

enum {
    RD_SUCCESS = 0,
    RD_NOT_FOUND = 120,
    RD_ALREADY_EXISTS = 190,
    RD_OUT_OF_MEMORY = 1011,
    RD_INVALID_ARGUMENT = 1009,
    RD_NOT_AUTHORIZED = 1014
};

typedef enum {
    PATH_INVALID = 0,
    PATH_EMPTY = 1,
    PATH_BINARY = 2,
    PATH_ASCII = 3,
    PATH_UTF16 = 4
} FS_PathType;

typedef struct {
    FS_PathType type;
    u32         size;
    const void *data;
} FS_Path;

typedef u64 FS_Archive;

typedef enum {
    /* The caller's own save, opened with an empty path. Only this one can be
     * formatted, which is the service's rule and the reason the backend has a
     * separate path for creating its own archive. */
    ARCHIVE_SAVEDATA = 0x00000004,
    /* Another title's save, opened with a binary path carrying the media type and
     * title id. This is the one that needs the rights in app.rsf. */
    ARCHIVE_USER_SAVEDATA = 0x567890B2,
    /* The title's own content as well as its save, which is where the SMDH with
     * the name the HOME menu shows lives. */
    ARCHIVE_SAVEDATA_AND_CONTENT = 0x2345678A
} FS_ArchiveID;

typedef enum {
    ARCHIVE_ACTION_COMMIT_SAVE_DATA = 0
} FS_ArchiveAction;

typedef enum {
    MEDIATYPE_NAND = 0,
    MEDIATYPE_SD = 1,
    MEDIATYPE_GAME_CARD = 2
} FS_MediaType;

typedef enum {
    SYSTEM_MEDIATYPE_SD = 0
} FS_SystemMediaType;

typedef enum {
    SECUREVALUE_SLOT_SD = 0x1000
} FS_SecureValueSlot;

enum {
    FS_OPEN_READ = 1,
    FS_OPEN_WRITE = 2,
    FS_OPEN_CREATE = 4
};

enum {
    FS_WRITE_FLUSH = 1
};

enum {
    FS_ATTRIBUTE_DIRECTORY = 1
};

typedef struct {
    u16  name[0x106];
    char shortName[0x0A];
    char shortExt[0x04];
    u8   valid;
    u8   reserved;
    u32  attributes;
    u64  fileSize;
} FS_DirectoryEntry;

typedef struct {
    u32 sectorSize;
    u32 clusterSize;
    u32 totalClusters;
    u32 freeClusters;
} FS_ArchiveResource;

/* ------------------------------------------------------------------ the api */

FS_Path fsMakePath(FS_PathType type, const void *path);

Result FSUSER_OpenArchive(FS_Archive *archive, FS_ArchiveID id, FS_Path path);
Result FSUSER_FormatSaveData(FS_ArchiveID archiveId, FS_Path path, u32 blocks,
                             u32 directories, u32 files, u32 directoryBuckets,
                             u32 fileBuckets, bool duplicateData);
Result APT_GetProgramID(u64 *pProgramID);

/* The title the stub pretends to be, for APT_GetProgramID. */
void daemoon_stub_set_own_title(u64 title_id);
Result FSUSER_CloseArchive(FS_Archive archive);
Result FSUSER_ControlArchive(FS_Archive archive, FS_ArchiveAction action, void *input,
                             u32 inputSize, void *output, u32 outputSize);
Result FSUSER_OpenFile(Handle *out, FS_Archive archive, FS_Path path, u32 openFlags,
                       u32 attributes);
Result FSUSER_OpenDirectory(Handle *out, FS_Archive archive, FS_Path path);
Result FSUSER_OpenFileDirectly(Handle *out, FS_ArchiveID archiveId, FS_Path archivePath,
                               FS_Path filePath, u32 openFlags, u32 attributes);

/* Gives a title an SMDH the stub will serve, so the name lookup is exercised
 * rather than only compiled. */
void daemoon_stub_set_title_name(u64 title_id, int lang, const char *name);
Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
Result FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes);
Result FSUSER_GetArchiveResource(FS_ArchiveResource *out, FS_SystemMediaType mediaType);
Result FSUSER_GetSaveDataSecureValue(bool *exists, u64 *value, FS_SecureValueSlot slot,
                                     u32 titleUniqueId, u8 titleVariation);
Result FSUSER_SetSaveDataSecureValue(u64 value, FS_SecureValueSlot slot, u32 titleUniqueId,
                                     u8 titleVariation);

Result FSFILE_Read(Handle handle, u32 *bytesRead, u64 offset, void *buffer, u32 size);
Result FSFILE_Write(Handle handle, u32 *bytesWritten, u64 offset, const void *buffer,
                    u32 size, u32 flags);
Result FSFILE_GetSize(Handle handle, u64 *size);
Result FSFILE_SetSize(Handle handle, u64 size);
Result FSFILE_Flush(Handle handle);
Result FSFILE_Close(Handle handle);

Result FSDIR_Read(Handle handle, u32 *entriesRead, u32 entryCount, FS_DirectoryEntry *entries);
Result FSDIR_Close(Handle handle);

Result AM_GetTitleCount(FS_MediaType mediatype, u32 *count);
Result AM_GetTitleList(u32 *titlesRead, FS_MediaType mediatype, u32 titleCount, u64 *titleIds);
Result AM_GetTitleProductCode(FS_MediaType mediatype, u64 titleId, char *productCode);

ssize_t utf8_to_utf16(u16 *out, const u8 *in, size_t len);
ssize_t utf16_to_utf8(u8 *out, const u16 *in, size_t len);

/* ------------------------------------------------------------ stub controls */

/* Everything the stub stores lives under root. */
void daemoon_stub_init(const char *root);
void daemoon_stub_reset(void);

/* Titles the stub reports from AM. */
void daemoon_stub_add_title(u64 title_id, const char *product_code);

/* Counts, so a test can assert a commit happened rather than assuming. */
unsigned daemoon_stub_commits(void);

/* Fault injection: the next commit fails. */
void daemoon_stub_fail_next_commit(void);

/* Open handles, so a test can prove nothing is leaked. */
int daemoon_stub_open_handles(void);

#endif /* DAEMOON_CTRU_STUB_3DS_H */
