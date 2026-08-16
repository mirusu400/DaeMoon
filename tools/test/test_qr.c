/* The QR codes the server encodes, decoded by the library the console will use.
 *
 * The two sides of the pairing flow are written by different code, in different
 * languages, from different readings of the same specification. A Go encoder that
 * passes its own tests proves that it agrees with itself; quirc reading what it
 * produced proves something worth having, and it is the reason the encoder was
 * written here instead of pulled in.
 *
 * The fixtures come from server/internal/qr/qr_test.go and are committed, so a
 * change on either side that breaks the other shows up as a diff and a failure
 * rather than as a console that cannot be paired.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include "quirc.h"

#include <daemoon/util/strbuf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QR_MAX_SIZE 128
/* quirc needs a light border around the code to find it. A real camera image has
 * one; a bitmap that is exactly the code does not, so one is added here. */
#define QR_QUIET 8
/* And it needs more than one pixel per module.
 *
 * quirc is written for camera frames: it locates the finders, fits a grid, and
 * samples the centre of each cell. At one pixel per module that sampling lands on
 * cell boundaries and reads a scattering of neighbours, which comes back as a
 * data ECC failure - a code it can see and cannot read. Rendering at four pixels
 * per module is both what fixes it and what a camera actually delivers. */
#define QR_SCALE 4

typedef struct {
    int  size;
    char text[512];
    unsigned char modules[QR_MAX_SIZE * QR_MAX_SIZE];
} qr_fixture_t;

/* Reads the ASCII PBM the Go side writes. The comment line carries the text the
 * code is supposed to hold, so the fixture is self describing: nothing here has
 * to be kept in step with a table somewhere else. */
static int read_fixture(const char *name, qr_fixture_t *out)
{
    char path[512];
    char line[QR_MAX_SIZE + 8];
    FILE *fp;
    int w = 0;
    int h = 0;
    int y;

    (void)snprintf(path, sizeof(path), "%s/shared/fixtures/qr/%s.pbm",
                   daemoon_test_root(), name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  cannot open %s\n", path);
        return -1;
    }

    if (fgets(line, sizeof(line), fp) == NULL || strncmp(line, "P1", 2) != 0) {
        (void)fclose(fp);
        return -1;
    }
    if (fgets(line, sizeof(line), fp) == NULL || line[0] != '#') {
        (void)fclose(fp);
        return -1;
    }
    {
        char *text = line + 1;
        size_t len;

        while (*text == ' ') {
            ++text;
        }
        len = strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
            text[--len] = '\0';
        }
        (void)daemoon_strlcpy(out->text, sizeof(out->text), text);
    }
    if (fgets(line, sizeof(line), fp) == NULL || sscanf(line, "%d %d", &w, &h) != 2) {
        (void)fclose(fp);
        return -1;
    }
    if (w != h || w <= 0 || w > QR_MAX_SIZE) {
        (void)fclose(fp);
        return -1;
    }
    out->size = w;

    for (y = 0; y < h; ++y) {
        int x;

        if (fgets(line, sizeof(line), fp) == NULL || (int)strlen(line) < w) {
            (void)fclose(fp);
            return -1;
        }
        for (x = 0; x < w; ++x) {
            out->modules[y * w + x] = (unsigned char)(line[x] == '1');
        }
    }
    (void)fclose(fp);
    return 0;
}

static void check_fixture(const char *name)
{
    static qr_fixture_t fx;
    struct quirc *q;
    uint8_t *image;
    int iw = 0;
    int ih = 0;
    int side;
    int x;
    int y;

    if (read_fixture(name, &fx) != 0) {
        CHECK(0);
        return;
    }

    side = (fx.size + QR_QUIET * 2) * QR_SCALE;
    q = quirc_new();
    CHECK(q != NULL);
    if (q == NULL) {
        return;
    }
    if (quirc_resize(q, side, side) < 0) {
        CHECK(0);
        quirc_destroy(q);
        return;
    }

    image = quirc_begin(q, &iw, &ih);
    CHECK_EQ_INT(iw, side);
    memset(image, 0xff, (size_t)side * (size_t)side);
    for (y = 0; y < fx.size; ++y) {
        for (x = 0; x < fx.size; ++x) {
            int sy;

            if (!fx.modules[y * fx.size + x]) {
                continue;
            }
            for (sy = 0; sy < QR_SCALE; ++sy) {
                int row = (y + QR_QUIET) * QR_SCALE + sy;

                memset(image + (size_t)row * (size_t)side +
                           (size_t)(x + QR_QUIET) * QR_SCALE,
                       0x00, QR_SCALE);
            }
        }
    }
    quirc_end(q);

    /* Exactly one code, or the encoder has drawn something a decoder reads as
     * two - which has happened to people, and is worth failing on rather than
     * taking the first result. */
    CHECK_EQ_INT(quirc_count(q), 1);
    if (quirc_count(q) == 1) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_decode_error_t err;

        quirc_extract(q, 0, &code);
        err = quirc_decode(&code, &data);
        if (err != QUIRC_SUCCESS) {
            printf("  %s: quirc says %s\n", name, quirc_strerror(err));
        }
        CHECK_EQ_INT((int)err, (int)QUIRC_SUCCESS);
        if (err == QUIRC_SUCCESS) {
            CHECK_EQ_INT((int)data.payload_len, (int)strlen(fx.text));
            CHECK_STR((const char *)data.payload, fx.text);
        }
    }
    quirc_destroy(q);
}

TEST_CASE(quirc_reads_what_the_server_encodes)
{
    check_fixture("short");
    check_fixture("pair-url");
    check_fixture("punctuation");
    check_fixture("long");
}

void test_qr(void)
{
    printf("qr (server encodes, quirc decodes)\n");
    RUN(quirc_reads_what_the_server_encodes);
}
