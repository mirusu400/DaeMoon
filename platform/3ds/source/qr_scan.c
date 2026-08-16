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

/* How often the decoder runs, in captured frames.
 *
 * quirc on a 400x240 frame is a good fraction of a second here, and running it on
 * every frame is what made the preview look frozen. A code that is in shot stays in
 * shot for longer than a person can hold still, so ten looks a second is generous. */
#define QUIRC_EVERY_N_FRAMES 3

/* Smallest power of two that holds a 400x240 frame. */
#define TEX_W 512
#define TEX_H 256

/* Two frames, alternating.
 *
 * The camera is a DMA engine with nowhere to spill: while no receive is armed it
 * has no buffer to write into, and the port overruns and stops. This loop spends
 * about seventy milliseconds tiling, drawing and decoding, which was long enough
 * that almost every capture afterwards timed out - measured at 250 ms a frame,
 * which was exactly the timeout, on a camera that runs at thirty.
 *
 * So the next frame is armed before the last one is touched, and the camera always
 * has somewhere to put a picture. */
static u16 *g_frame[2];
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

/* Everything the last scan learned, so a run that failed says which of the three
 * ways it failed rather than only that it did. */
static daemoon_3ds_qr_stats_t g_stats;

const daemoon_3ds_qr_stats_t *daemoon_3ds_qr_last_stats(void)
{
    return &g_stats;
}

/* Frame to texture, on the CPU.
 *
 * GX_DisplayTransfer would do this on the GPU and is what most homebrew uses. It
 * drew noise here, and the reason it was not worth chasing is that three guesses -
 * the buffer dimensions, the orientation, the subtexture - all fail the same way,
 * and the only way to look at the result was to photograph a console. The tiling
 * this uses instead has a desktop test that checks specific pixels against a table
 * written out by hand.
 *
 * Ninety six thousand pixels a frame, which is far less than quirc costs on the
 * same frame. */
static void frame_to_texture(const u16 *frame)
{
    daemoon_3ds_cam_to_tiled(frame, CAM_W, CAM_H, (u16 *)g_preview.data,
                             TEX_W, TEX_H, g_stats.layout);
    C3D_TexFlush(&g_preview);
}

static int preview_open(void)
{
    if (!C3D_TexInit(&g_preview, TEX_W, TEX_H, GPU_RGB565)) {
        return 0;
    }
    C3D_TexSetFilter(&g_preview, GPU_LINEAR, GPU_LINEAR);
    memset(g_preview.data, 0, g_preview.size);

    /* The rotation is handled while tiling, so what is in the texture is already
     * the right way up: a 400x240 image at the origin of a 512x256 texture. */
    g_preview_sub.width = CAM_W;
    g_preview_sub.height = CAM_H;
    g_preview_sub.left = 0.0f;
    g_preview_sub.top = 1.0f;
    g_preview_sub.right = (float)CAM_W / (float)TEX_W;
    g_preview_sub.bottom = 1.0f - (float)CAM_H / (float)TEX_H;

    g_preview_image.tex = &g_preview;
    g_preview_image.subtex = &g_preview_sub;
    return 1;
}

const C2D_Image *daemoon_3ds_qr_preview(void)
{
    return g_preview_image.tex != NULL ? &g_preview_image : NULL;
}

static void release_buffers(void)
{
    free(g_luma);
    linearFree(g_frame[0]);
    linearFree(g_frame[1]);
    g_luma = NULL;
    g_frame[0] = NULL;
    g_frame[1] = NULL;
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

    CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_IN1);
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
    Handle buffer_error = 0;
    int cur = 0;
    daemoon_result_t result = DAEMOON_ERR_NOT_FOUND;
    u32 transfer = 0;
    u32 select = SELECT_OUT1;
    int cancelled = 0;
    unsigned tick = 0;

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.layout = DAEMOON_3DS_CAM_LAYOUT_DEFAULT;
    if (out == NULL || cap == 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    out[0] = '\0';

    /* Linear memory: the camera writes here and the GPU reads it, and neither can
     * reach an ordinary array. */
    g_frame[0] = (u16 *)linearAlloc(CAM_BYTES);
    g_frame[1] = (u16 *)linearAlloc(CAM_BYTES);
    g_luma = (u8 *)malloc(CAM_PIXELS);
    if (g_frame[0] == NULL || g_frame[1] == NULL || g_luma == NULL) {
        release_buffers();
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }
    memset(g_frame[0], 0, CAM_BYTES);
    memset(g_frame[1], 0, CAM_BYTES);

    if (R_FAILED(camInit())) {
        release_buffers();
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    if (!preview_open()) {
        camExit();
        release_buffers();
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    if (start_camera(select, &transfer) != DAEMOON_OK) {
        C3D_TexDelete(&g_preview);
        g_preview_image.tex = NULL;
        camExit();
        release_buffers();
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    /* Rows of the frame width, which is what the buffer holds, so quirc is given
     * the picture the right way round rather than a skewed one it has to solve
     * for. */
    q = quirc_new();
    if (q == NULL || quirc_resize(q, CAM_W, CAM_H) < 0) {
        if (q != NULL) {
            quirc_destroy(q);
        }
        stop_camera();
        C3D_TexDelete(&g_preview);
        g_preview_image.tex = NULL;
        camExit();
        release_buffers();
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    /* Capture, draw, and decode - in that order, and not at the same rate.
     *
     * The first version drew before capturing and ran quirc on every frame, which
     * made the preview show the previous picture and hold it for as long as a
     * decode took. On a 268 MHz ARM11 a decode is a good fraction of a second, so
     * the preview updated perhaps twice a second and read as frozen.
     *
     * A person aiming a camera needs the picture at the camera's rate. quirc needs
     * a frame every so often, because a code that is in shot is in shot for longer
     * than a person can hold still. Those are different rates and this loop runs
     * them separately.
     */
    /* Arm the first receive before capture starts, so there is never a moment when
     * the camera has a frame and nowhere to put it. */
    CAMU_GetBufferErrorInterruptEvent(&buffer_error, PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    if (R_FAILED(CAMU_SetReceiving(&receive, g_frame[cur], PORT_CAM1, CAM_BYTES,
                                   (s16)transfer))) {
        result = DAEMOON_ERR_BACKEND_ERROR;
    } else {
        CAMU_StartCapture(PORT_CAM1);
    }

    while (result != DAEMOON_ERR_BACKEND_ERROR && aptMainLoop()) {
        Handle events[2];
        s32 which = -1;
        Result res;
        int action;
        const u16 *ready = NULL;
        u64 t0;

        events[0] = receive;
        events[1] = buffer_error;

        t0 = osGetTime();
        res = svcWaitSynchronizationN(&which, events, 2, false, 250000000LL);
        g_stats.ms_capture = (unsigned)(osGetTime() - t0);

        if (R_FAILED(res)) {
            ++g_stats.timeouts;
        } else if (which == 1) {
            /* The port overran. That is what happens when a frame arrives with no
             * receive armed, and it stays stopped until it is cleared - so a single
             * slow pass used to wedge the camera for good rather than drop one
             * frame. */
            ++g_stats.buffer_errors;
            svcCloseHandle(receive);
            receive = 0;
            CAMU_StopCapture(PORT_CAM1);
            CAMU_ClearBuffer(PORT_CAM1);
            if (R_FAILED(CAMU_SetReceiving(&receive, g_frame[cur], PORT_CAM1,
                                           CAM_BYTES, (s16)transfer))) {
                result = DAEMOON_ERR_BACKEND_ERROR;
                break;
            }
            CAMU_StartCapture(PORT_CAM1);
        } else {
            svcCloseHandle(receive);
            receive = 0;
            ++g_stats.frames;
            ready = g_frame[cur];
            cur ^= 1;

            /* Re-armed before a single pixel of the frame just delivered is read.
             * Everything below takes about seventy milliseconds and the camera runs
             * at thirty a second; without this it has nowhere to write for two
             * frames out of every three. */
            if (R_FAILED(CAMU_SetReceiving(&receive, g_frame[cur], PORT_CAM1,
                                           CAM_BYTES, (s16)transfer))) {
                result = DAEMOON_ERR_BACKEND_ERROR;
                break;
            }
        }

        if (ready != NULL) {
            t0 = osGetTime();
            /* The previous frame may still be in the GPU's hands, and this texture
             * is what it is drawing from. Overwriting it mid draw is a picture with
             * a band of the next frame in it, or none at all - which is the black
             * flicker a console showed. citro2d's own frame start waits for the
             * previous draw, but that happens inside the callback, after this. */
            C3D_FrameSync();
            frame_to_texture(ready);
            g_stats.ms_tile = (unsigned)(osGetTime() - t0);
        }

        /* Drawn after the texture is fresh, and called even when a capture failed,
         * because the callback is the only way out of this screen. */
        t0 = osGetTime();
        action = frame_cb != NULL ? frame_cb(user) : 1;
        g_stats.ms_draw = (unsigned)(osGetTime() - t0);

        if (action == 0) {
            cancelled = 1;
            break;
        }
        if (action == 3) {
            g_stats.layout = (g_stats.layout + 1) % DAEMOON_3DS_CAM_LAYOUTS;
            continue;
        }
        if (action == 2) {
            /* The other camera. A person who cannot get a code to read will try
             * the front one, and refusing to let them is a worse answer than
             * letting them find out. */
            if (receive != 0) {
                svcCloseHandle(receive);
                receive = 0;
            }
            stop_camera();
            select = (select == SELECT_OUT1) ? SELECT_IN1 : SELECT_OUT1;
            g_stats.camera = (select == SELECT_IN1);
            if (start_camera(select, &transfer) != DAEMOON_OK) {
                result = DAEMOON_ERR_BACKEND_ERROR;
                break;
            }
            CAMU_ClearBuffer(PORT_CAM1);
            if (R_FAILED(CAMU_SetReceiving(&receive, g_frame[cur], PORT_CAM1,
                                           CAM_BYTES, (s16)transfer))) {
                result = DAEMOON_ERR_BACKEND_ERROR;
                break;
            }
            CAMU_StartCapture(PORT_CAM1);
            continue;
        }

        if (ready == NULL) {
            continue;
        }

        /* Decoding runs at its own rate. It is cheap - measured at thirty
         * milliseconds - but it is still work the preview does not need. */
        if (++tick % QUIRC_EVERY_N_FRAMES != 0) {
            continue;
        }

        t0 = osGetTime();
        g_stats.mean_luma = frame_to_luma(ready, g_luma, CAM_PIXELS);
        {
            int w = 0;
            int h = 0;
            u8 *image = quirc_begin(q, &w, &h);

            memcpy(image, g_luma, CAM_PIXELS);
            quirc_end(q);
        }

        g_stats.codes_seen = quirc_count(q);
        g_stats.ms_decode = (unsigned)(osGetTime() - t0);
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

    if (buffer_error != 0) {
        svcCloseHandle(buffer_error);
    }
    if (receive != 0) {
        svcCloseHandle(receive);
    }
    stop_camera();
    quirc_destroy(q);
    camExit();

    C3D_TexDelete(&g_preview);
    g_preview_image.tex = NULL;
    release_buffers();

    {
        char line[192];

        (void)snprintf(line, sizeof(line),
                       "frames=%u luma=%u codes=%d decfail=%u recvfail=%u "
                       "timeout=%u buferr=%u cam=%s layout=%s err=%d "
                       "cap=%u tile=%u draw=%u dec=%u",
                       g_stats.frames, g_stats.mean_luma, g_stats.codes_seen,
                       g_stats.decode_failures, g_stats.receive_failures,
                       g_stats.timeouts, g_stats.buffer_errors,
                       g_stats.camera ? "inner" : "outer",
                       daemoon_3ds_cam_layout_name(g_stats.layout),
                       g_stats.last_decode_error, g_stats.ms_capture,
                       g_stats.ms_tile, g_stats.ms_draw, g_stats.ms_decode);
        daemoon_3ds_trace("qr/stats", line);
    }

    if (cancelled) {
        return DAEMOON_ERR_USER_CANCELLED;
    }
    return result;
}
