/* The interface core talks to.
 *
 * Four functions, and the important half is that they are the same four the 3DS
 * build implements and the desktop tests implement: a confirmation before anything
 * destructive, a choice rather than a merge on a conflict, a progress line, and a
 * notice. Core does not know a dialog exists, and this file does not know what a
 * sync is.
 *
 * Every one of these is called from the worker thread and every one of them draws on
 * the main thread. See ops.cpp for why that split exists; what matters here is that
 * a question asked from the main thread would wait for the main thread to draw it,
 * which is a hang rather than a question. That is checked rather than assumed.
 */
#include "ui/dm_ui.hpp"

#include <cstring>

extern "C" {

static int ui_confirm(void *ctx, const daemoon_str_ref_t *msg)
{
    (void)ctx;

    if (!dm::onWorkerThread())
    {
        /* Refused rather than risked. Answering "no" is the safe reading of a
         * question that cannot be asked: rule 7 says a destructive action goes
         * through a confirmation, and one that never reached anybody is not one. */
        brls::Logger::error("confirm() from the main thread - refusing");
        return 0;
    }

    /* Two options and the cursor on "no". The question is only ever asked before
     * something that cannot be undone, and a cursor resting on yes turns a reflex
     * into a decision that was never made. B means the same as no, for the same
     * reason. */
    return dm::askBlocking(dm::strf(msg),
                           { dm::str(DAEMOON_STR_BTN_NO), dm::str(DAEMOON_STR_BTN_YES) },
                           0) == 1;
}

static void ui_progress(void *ctx, const daemoon_str_ref_t *label, int pct)
{
    (void)ctx;
    dm::progressUpdate(dm::strf(label), pct);
}

static int ui_choose(void *ctx, const daemoon_str_ref_t *msg,
                     const daemoon_str_ref_t *opts, size_t n)
{
    std::vector<std::string> options;

    (void)ctx;

    if (!dm::onWorkerThread())
    {
        brls::Logger::error("choose() from the main thread - refusing");
        return -1;
    }

    for (size_t i = 0; i < n; ++i)
        options.push_back(dm::strf(&opts[i]));

    /* No default. A conflict is two saves and the answer is not this application's
     * to lean toward, so the cursor sits on the first option and B leaves both
     * alone - which is what the negative means to core. */
    return dm::askBlocking(dm::strf(msg), options, -1);
}

static void ui_notify(void *ctx, const daemoon_str_ref_t *msg)
{
    std::string text = dm::strf(msg);

    (void)ctx;
    brls::sync([text]() { brls::Application::notify(text); });
}

const daemoon_ui_backend_t daemoon_nx_ui_backend = {
    ui_confirm,
    ui_progress,
    ui_choose,
    ui_notify
};

void daemoon_nx_ui_init(daemoon_nx_ui_ctx_t *ctx)
{
    std::memset(ctx, 0, sizeof(*ctx));
}

} /* extern "C" */
