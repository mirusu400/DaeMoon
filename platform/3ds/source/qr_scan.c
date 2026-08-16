/* Reading a pairing code off the 3DS camera.
 *
 * The rules call the software keyboard painful and they are right, which is why
 * this exists: a pairing code is six digits and a server address is a URL, and
 * scanning both off a screen is the difference between a minute of thumbing at a
 * touchscreen and pointing the console at a monitor.
 *
 * The first version of this had no preview, and that was the whole failure. A scan
 * with nothing on screen cannot be aimed, and when it does not work there is no way
 * to tell a camera that never started from a code held too close - both are a
 * console doing nothing. The preview is not decoration; it is the only feedback
 * loop the person holding the console has.
 *
 * Three things about the camera are load bearing and easy to get wrong:
 *
 *   - It hands back RGB565 and quirc wants one byte of luma per pixel. The
 *     conversion is here rather than left to quirc, which has no idea what a 3DS
 *     frame looks like.
 *   - The frame is linear and a texture is tiled. GX_DisplayTransfer does that
 *     conversion on the GPU, which is why the frame buffer has to be linear memory
 *     rather than an ordinary array.
 *   - The receive is a blocking service call with its own event. A timeout is not
 *     optional: a camera that never delivers a frame would otherwise be an
 *     application that never returns, on a screen with no way out.
 */
#include "daemoon_3ds.h"
#include "icons.h"

#include <3ds.h>
#include <quirc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The camera's top screen size. A QR code on a monitor at arm's length is a large
 * fraction of the frame, and every pixel costs quirc time on a 268 MHz ARM11. */
#define CAM_W 400
#define CAM_H 240

#define CAM_PIXELS ((size_t)CAM_W * (size_t)CAM_H)
#define CAM_BYTES  (CAM_PIXELS * 2)

/* Smallest power of two that holds a 400x240 frame. */
#define TEX_W 512
#define TEX_H 256

static u16 *g_frame;   /* linear: the GPU reads it, and so does the camera */
static u8  *g_luma;    /* what quirc is given */

static C3D_Tex          g_preview;
static Tex3DS_SubTexture g_preview_sub;
static C2D_Image        g_preview_image;

/* RGB565 to luma, in integer arithmetic.
 *
 * quirc thresholds what it is given, so the coefficients only have to preserve the
 * contrast between a dark module and a light one. These are the usual BT.601
 * weights scaled to 8 bits, which keep a red or blue background from reading as
 * black. The mean is returned because it is the one number that tells a camera
 * which never started from a room which is simply dark - and those need different
 * things done about them. */
static unsigned frame_to_luma(const u16 *src, u8 *dst, size_t pixels)
{
    unsigned long long total = 0;
    size_t i;

    for (i = 0; i < pixels; ++i) {
        u16 p = src[i];
        unsigned r = (p >> 11) & 0x1fu;
        unsigned g = (p >> 5) & 0x3fu;
        unsigned b = p & 0x1fu;
        unsigned y;

        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        y = (r * 77u + g * 150u + b * 29u) >> 8;
        dst[i] = (u8)y;
        total += y;
    }
    return (unsigned)(total / (unsigned long long)pixels);
}

/* Linear frame to tiled texture, on the GPU. Doing this with the CPU would be a
 * hundred thousand pixel swizzles per frame on a processor that has quirc to run. */
static void frame_to_texture(void)
{
    GSPGPU_FlushDataCache(g_frame, CAM_BYTES);
    GX_DisplayTransfer((u32 *)g_frame, GX_BUFFER_DIM(CAM_W, CAM_H),
                       (u32 *)g_preview.data, GX_BUFFER_DIM(TEX_W, TEX_H),
                       GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
                           GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                           GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    gspWaitForPPF();
}

static int preview_open(void)
{
    if (!C3D_TexInit(&g_preview, TEX_W, TEX_H, GPU_RGB565)) {
        return 0;
    }
    C3D_TexSetFilter(&g_preview, GPU_LINEAR, GPU_LINEAR);
    memset(g_preview.data, 0, g_preview.size);

    /* The camera frame lands rotated: what the sensor calls a row is a column on
     * screen. The subtexture is what turns it back, so the person sees what the
     * camera sees rather than a picture on its side. */
    g_preview_sub.width = CAM_H;
    g_preview_sub.height = CAM_W;
    g_preview_sub.left = 0.0f;
    g_preview_sub.top = 1.0f;
    g_preview_sub.right = (float)CAM_H / (float)TEX_W;
    g_preview_sub.bottom = 1.0f - (float)CAM_W / (float)TEX_H;

    g_preview_image.tex = &g_preview;
    g_preview_image.subtex = &g_preview_sub;
    return 1;
}

const C2D_Image *daemoon_3ds_qr_preview(void)
{
    return g_preview_image.tex != NULL ? &g_preview_image : NULL;
}

/* Everything the last scan learned, so a run that failed says which of the three
 * ways it failed rather than only that it did. */
static daemoon_3ds_qr_stats_t g_stats;

const daemoon_3ds_qr_stats_t *daemoon_3ds_qr_last_stats(void)
{
    return &g_stats;
}

static daemoon_result_t start_camera(u32 select, u32 *transfer)
{
    if (R_FAILED(CAMU_SetSize(select, SIZE_CTR_TOP_LCD, CONTEXT_A)) ||
        R_FAILED(CAMU_SetOutputFormat(select, OUTPUT_RGB_565, CONTEXT_A)) ||
        R_FAILED(CAMU_SetFrameRate(select, FRAME_RATE_30)) ||
        R_FAILED(CAMU_SetNoiseFilter(select, true)) ||
        R_FAILED(CAMU_SetAutoExposure(select, true)) ||
        R_FAILED(CAMU_SetAutoWhiteBalance(select, true)) ||
        /* Without this the port may hand back a cropped frame whose stride does
         * not match what was asked for, which decodes as noise. */
        R_FAILED(CAMU_SetTrimming(PORT_CAM1, false)) ||
        R_FAILED(CAMU_Activate(select))) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }
    if (R_FAILED(CAMU_GetMaxBytes(transfer, CAM_W, CAM_H)) ||
        R_FAILED(CAMU_SetTransferBytes(PORT_CAM1, *transfer, CAM_W, CAM_H))) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_IN1);
    CAMU_StartCapture(PORT_CAM1);
    return DAEMOON_OK;
}

static void stop_camera(void)
{
    CAMU_StopCapture(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
}

daemoon_result_t daemoon_3ds_qr_scan(daemoon_3ds_qr_frame_cb frame_cb, void *user,
                                     char *out, size_t cap)
{
    struct quirc *q = NULL;
    Handle receive = 0;
    daemoon_result_t result = DAEMOON_ERR_NOT_FOUND;
    u32 transfer = 0;
    u32 select = SELECT_OUT1;
    int cancelled = 0;
    int want_flip = 0;

    memset(&g_stats, 0, sizeof(g_stats));
    if (out == NULL || cap == 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    out[0] = '\0';

    /* Linear memory: the camera writes here and the GPU reads it, and neither can
     * reach an ordinary array. */
    g_frame = (u16 *)linearAlloc(CAM_BYTES);
    g_luma = (u8 *)malloc(CAM_PIXELS);
    if (g_frame == NULL || g_luma == NULL) {
        free(g_luma);
        linearFree(g_frame);
        g_frame = NULL;
        g_luma = NULL;
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    memset(g_frame, 0, CAM_BYTES);

    if (R_FAILED(camInit())) {
        free(g_luma);
        linearFree(g_frame);
        g_frame = NULL;
        g_luma = NULL;
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    if (!preview_open()) {
        camExit();
        free(g_luma);
        linearFree(g_frame);
        g_frame = NULL;
        g_luma = NULL;
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    if (start_camera(select, &transfer) != DAEMOON_OK) {
        C3D_TexDelete(&g_preview);
        g_preview_image.tex = NULL;
        camExit();
        free(g_luma);
        linearFree(g_frame);
        g_frame = NULL;
        g_luma = NULL;
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    q = quirc_new();
    if (q == NULL || quirc_resize(q, CAM_W, CAM_H) < 0) {
        if (q != NULL) {
            quirc_destroy(q);
        }
        stop_camera();
        C3D_TexDelete(&g_preview);
        g_preview_image.tex = NULL;
        camExit();
        free(g_luma);
        linearFree(g_frame);
        g_frame = NULL;
        g_luma = NULL;
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    while (aptMainLoop()) {
        Result res;
        int action;

        action = frame_cb != NULL ? frame_cb(user) : 1;
        if (action == 0) {
            cancelled = 1;
            break;
        }
        if (action == 2) {
            /* The other camera. A person who cannot get a code to read will try
             * the front one, and refusing to let them is a worse answer than
             * letting them find out. */
            want_flip = 1;
        }
        if (want_flip) {
            want_flip = 0;
            stop_camera();
            select = (select == SELECT_OUT1) ? SELECT_IN1 : SELECT_OUT1;
            g_stats.camera = (select == SELECT_IN1);
            if (start_camera(select, &transfer) != DAEMOON_OK) {
                result = DAEMOON_ERR_BACKEND_ERROR;
                break;
            }
            continue;
        }

        res = CAMU_SetReceiving(&receive, g_frame, PORT_CAM1, CAM_BYTES,
                                (s16)transfer);
        if (R_FAILED(res)) {
            ++g_stats.receive_failures;
            continue;
        }
        /* Bounded, always. A camera that stops delivering must not become an
         * application that never returns, on a screen whose only way out is the
         * frame callback. */
        res = svcWaitSynchronization(receive, 500000000LL);
        svcCloseHandle(receive);
        receive = 0;
        if (R_FAILED(res)) {
            ++g_stats.timeouts;
            continue;
        }
        ++g_stats.frames;

        g_stats.mean_luma = frame_to_luma(g_frame, g_luma, CAM_PIXELS);
        frame_to_texture();

        {
            int w = 0;
            int h = 0;
            u8 *image = quirc_begin(q, &w, &h);

            memcpy(image, g_luma, CAM_PIXELS);
            quirc_end(q);
        }

        g_stats.codes_seen = quirc_count(q);
        if (g_stats.codes_seen > 0) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_decode_error_t err;

            quirc_extract(q, 0, &code);
            err = quirc_decode(&code, &data);
            g_stats.last_decode_error = (int)err;
            if (err == QUIRC_SUCCESS) {
                size_t len = (size_t)data.payload_len;

                if (len >= cap) {
                    len = cap - 1;
                }
                memcpy(out, data.payload, len);
                out[len] = '\0';
                result = DAEMOON_OK;
                break;
            }
            ++g_stats.decode_failures;
        }
    }

    if (receive != 0) {
        svcCloseHandle(receive);
    }
    stop_camera();
    quirc_destroy(q);
    camExit();

    C3D_TexDelete(&g_preview);
    g_preview_image.tex = NULL;
    free(g_luma);
    linearFree(g_frame);
    g_luma = NULL;
    g_frame = NULL;

    {
        char line[192];

        (void)snprintf(line, sizeof(line),
                       "frames=%u luma=%u codes=%d decfail=%u recvfail=%u "
                       "timeout=%u cam=%s err=%d",
                       g_stats.frames, g_stats.mean_luma, g_stats.codes_seen,
                       g_stats.decode_failures, g_stats.receive_failures,
                       g_stats.timeouts, g_stats.camera ? "inner" : "outer",
                       g_stats.last_decode_error);
        daemoon_3ds_trace("qr/stats", line);
    }

    if (cancelled) {
        return DAEMOON_ERR_USER_CANCELLED;
    }
    return result;
}
