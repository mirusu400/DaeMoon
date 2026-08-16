/* Reading a pairing code off the 3DS camera.
 *
 * The rules call the software keyboard painful and they are right, which is why
 * this exists: a pairing code is six digits and a server address is a URL, and
 * scanning both off a screen is the difference between a minute of thumbing at a
 * touchscreen and pointing the console at a monitor.
 *
 * Three things about the camera are load bearing and easy to get wrong:
 *
 *   - It hands back RGB565 and quirc wants one byte of luma per pixel. The
 *     conversion is here rather than left to quirc, which has no idea what a 3DS
 *     frame looks like.
 *   - `camuInit` is not enough. The camera has to be selected, configured, its
 *     buffer set, and activation is asynchronous - taking a frame before it has
 *     settled gives a black image, which decodes as nothing and looks exactly like
 *     a code that will not scan.
 *   - The receive is a blocking service call with its own event. A timeout is not
 *     optional: a camera that never delivers a frame would otherwise be an
 *     application that never returns, on a screen with no way out.
 *
 * quirc's own work is the expensive part, so it runs on a downscaled frame at a
 * rate a person can hold a console still for, rather than on every frame.
 */
#include "daemoon_3ds.h"

#include <3ds.h>
#include <quirc.h>

#include <stdlib.h>
#include <string.h>

/* The camera's smallest useful size. A QR code on a monitor at arm's length is a
 * large fraction of the frame, and every pixel here costs quirc time on a 268 MHz
 * ARM11. */
#define CAM_W 400
#define CAM_H 240

#define CAM_PIXELS ((size_t)CAM_W * (size_t)CAM_H)
#define CAM_BYTES  (CAM_PIXELS * 2)

/* One frame is 192 KB and the luma copy another 96 KB. Both are static rather than
 * on the heap: they live for one screen, the 3DS heap fragments, and this is the
 * one place in the application that wants a quarter of a megabyte at once. */
static u16 g_frame[CAM_PIXELS];
static u8  g_luma[CAM_PIXELS];

/* RGB565 to luma, in integer arithmetic.
 *
 * quirc thresholds what it is given, so the coefficients only have to preserve the
 * contrast between a dark module and a light one. These are the usual ITU-R BT.601
 * weights scaled to 8 bits, which cost two multiplies per pixel and keep a red or
 * blue background from reading as black. */
static void frame_to_luma(const u16 *src, u8 *dst, size_t pixels)
{
    size_t i;

    for (i = 0; i < pixels; ++i) {
        u16 p = src[i];
        unsigned r = (p >> 11) & 0x1fu;
        unsigned g = (p >> 5) & 0x3fu;
        unsigned b = p & 0x1fu;

        /* 5 and 6 bit channels widened to 8, then weighted 0.299 / 0.587 / 0.114. */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        dst[i] = (u8)((r * 77u + g * 150u + b * 29u) >> 8);
    }
}

daemoon_result_t daemoon_3ds_qr_scan(daemoon_3ds_qr_frame_cb frame_cb, void *user,
                                     char *out, size_t cap)
{
    struct quirc *q = NULL;
    Handle receive = 0;
    daemoon_result_t result = DAEMOON_ERR_NOT_FOUND;
    Result res;
    u32 transfer = 0;
    int cancelled = 0;

    if (out == NULL || cap == 0) {
        return DAEMOON_ERR_INVALID_REQUEST;
    }
    out[0] = '\0';

    if (R_FAILED(camInit())) {
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    /* The outer camera. The inner one points at the person holding the console,
     * which is not where the code is. */
    if (R_FAILED(CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A)) ||
        R_FAILED(CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A)) ||
        R_FAILED(CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30)) ||
        R_FAILED(CAMU_SetNoiseFilter(SELECT_OUT1, true)) ||
        R_FAILED(CAMU_SetAutoExposure(SELECT_OUT1, true)) ||
        R_FAILED(CAMU_SetAutoWhiteBalance(SELECT_OUT1, true)) ||
        R_FAILED(CAMU_Activate(SELECT_OUT1))) {
        camExit();
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    if (R_FAILED(CAMU_GetMaxBytes(&transfer, CAM_W, CAM_H)) ||
        R_FAILED(CAMU_SetTransferBytes(PORT_CAM1, transfer, CAM_W, CAM_H))) {
        CAMU_Activate(SELECT_NONE);
        camExit();
        return DAEMOON_ERR_BACKEND_ERROR;
    }

    q = quirc_new();
    if (q == NULL || quirc_resize(q, CAM_W, CAM_H) < 0) {
        if (q != NULL) {
            quirc_destroy(q);
        }
        CAMU_Activate(SELECT_NONE);
        camExit();
        return DAEMOON_ERR_OUT_OF_MEMORY;
    }

    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_OUT1);
    CAMU_StartCapture(PORT_CAM1);

    while (aptMainLoop()) {
        if (frame_cb != NULL && !frame_cb(user)) {
            cancelled = 1;
            break;
        }

        res = CAMU_SetReceiving(&receive, g_frame, PORT_CAM1, CAM_BYTES,
                                (s16)transfer);
        if (R_FAILED(res)) {
            result = DAEMOON_ERR_BACKEND_ERROR;
            break;
        }
        /* Bounded, always. A camera that stops delivering must not become an
         * application that never returns, on a screen whose only way out is the
         * frame callback. */
        res = svcWaitSynchronization(receive, 1000000000LL);
        svcCloseHandle(receive);
        receive = 0;
        if (R_FAILED(res)) {
            continue;
        }

        {
            int w = 0;
            int h = 0;
            u8 *image = quirc_begin(q, &w, &h);

            frame_to_luma(g_frame, g_luma, CAM_PIXELS);
            memcpy(image, g_luma, CAM_PIXELS);
            quirc_end(q);
        }

        if (quirc_count(q) > 0) {
            struct quirc_code code;
            struct quirc_data data;

            quirc_extract(q, 0, &code);
            if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
                size_t len = (size_t)data.payload_len;

                if (len >= cap) {
                    len = cap - 1;
                }
                memcpy(out, data.payload, len);
                out[len] = '\0';
                result = DAEMOON_OK;
                break;
            }
        }
    }

    if (receive != 0) {
        svcCloseHandle(receive);
    }
    CAMU_StopCapture(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    quirc_destroy(q);
    camExit();

    if (cancelled) {
        return DAEMOON_ERR_USER_CANCELLED;
    }
    return result;
}
