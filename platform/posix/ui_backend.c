/* Scripted UI backend.
 *
 * It answers from a script instead of asking a person, and it renders every prompt
 * through daemoon_strf so the i18n path is exercised by the tests rather than only
 * compiled. A template with a missing argument or a stale placeholder shows up here
 * as visibly wrong text on a desktop, which is much cheaper than finding it on a
 * console. */
#include "daemoon_posix.h"

#include <daemoon/i18n.h>

#include <stdio.h>
#include <string.h>

void daemoon_posix_ui_init(daemoon_posix_ui_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->confirm_answer = 1;
    ctx->choose_answer = 0;
}

static void render(const daemoon_posix_ui_ctx_t *ui, const char *kind,
                   const daemoon_str_ref_t *ref)
{
    char text[512];

    if (!ui->verbose) {
        return;
    }
    (void)daemoon_strf(text, sizeof(text), ref->id, ref->args, ref->nargs);
    printf("[ui:%s] %s\n", kind, text);
}

static int ui_confirm(void *vctx, const daemoon_str_ref_t *msg)
{
    daemoon_posix_ui_ctx_t *ui = (daemoon_posix_ui_ctx_t *)vctx;

    ui->confirms++;
    ui->last_confirm = msg->id;
    render(ui, "confirm", msg);
    return ui->confirm_answer;
}

static void ui_progress(void *vctx, const daemoon_str_ref_t *label, int pct)
{
    daemoon_posix_ui_ctx_t *ui = (daemoon_posix_ui_ctx_t *)vctx;

    (void)pct;
    ui->progresses++;
    render(ui, "progress", label);
}

static int ui_choose(void *vctx, const daemoon_str_ref_t *msg, const daemoon_str_ref_t *opts,
                     size_t n)
{
    daemoon_posix_ui_ctx_t *ui = (daemoon_posix_ui_ctx_t *)vctx;
    size_t i;

    ui->chooses++;
    ui->last_choose = msg->id;
    render(ui, "choose", msg);
    for (i = 0; i < n; ++i) {
        render(ui, "option", &opts[i]);
    }

    if (ui->choose_answer >= (int)n) {
        return -1;
    }
    return ui->choose_answer;
}

static void ui_notify(void *vctx, const daemoon_str_ref_t *msg)
{
    daemoon_posix_ui_ctx_t *ui = (daemoon_posix_ui_ctx_t *)vctx;

    ui->notifies++;
    ui->last_notify = msg->id;
    render(ui, "notify", msg);
}

const daemoon_ui_backend_t daemoon_posix_ui_backend = {
    ui_confirm,
    ui_progress,
    ui_choose,
    ui_notify
};
