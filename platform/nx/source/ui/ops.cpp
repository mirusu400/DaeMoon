/* Running an operation, and everything that has to happen around one.
 *
 * The shape here follows from one fact about core: its UI backend is blocking by
 * design. `confirm()` returns the answer, `choose()` returns the index, and the sync
 * path calls them from the middle of itself, because the alternative - a state
 * machine with a callback per question - is how a rule like "never auto merge" gets
 * lost. borealis is the opposite: one main loop, drawing, that cannot be re-entered
 * to ask a question.
 *
 * So the operation runs on a worker thread and the interface stays on the main one.
 * A question posts a dialog and blocks the worker until a button is pressed; the
 * progress line posts an update and does not block. Nothing on the worker touches a
 * view, and nothing on the main thread touches a save.
 *
 * One operation at a time. This is not a queue and must not become one: two of these
 * at once is two writers to the same save archive, and `save_backend.c` refuses a
 * second mount out loud rather than silently shadowing the first.
 */
#include "ui/dm_ui.hpp"

#include "backend_conformance.h"
#include "test.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

namespace dm
{

static std::atomic<bool>     g_running { false };
static std::thread::id       g_workerId;
static std::function<void()> g_titlesChanged;

/* The progress dialog, and the two things in it that change. Main thread only. */
static brls::Dialog* g_progress      = nullptr;
static brls::Label*  g_progressLabel = nullptr;

bool operationRunning()
{
    return g_running.load();
}

bool onWorkerThread()
{
    return g_running.load() && std::this_thread::get_id() == g_workerId;
}

void setTitlesChangedCallback(std::function<void()> cb)
{
    g_titlesChanged = std::move(cb);
}

/* ------------------------------------------------------------------- progress */

void progressOpen(daemoon_str_id_t title)
{
    std::string heading = str(title);

    brls::sync([heading]() {
        if (g_progress != nullptr)
            return;

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setAlignItems(brls::AlignItems::CENTER);
        content->setPadding(20.0f, 30.0f, 20.0f, 30.0f);

        auto* spinner = new brls::ProgressSpinner();
        spinner->setWidth(56.0f);
        spinner->setHeight(56.0f);
        spinner->setMarginBottom(20.0f);
        content->addView(spinner);

        auto* what = new brls::Label();
        what->setText(heading);
        what->setFontSize(22.0f);
        what->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        content->addView(what);

        g_progressLabel = new brls::Label();
        g_progressLabel->setFontSize(18.0f);
        g_progressLabel->setMarginTop(8.0f);
        g_progressLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        g_progressLabel->setTextColor(brls::Application::getTheme()["daemoon/muted"]);
        content->addView(g_progressLabel);

        g_progress = new brls::Dialog(content);
        /* No buttons and no way out. A save being written is not something to walk
         * away from halfway; the operation ends and then this does. */
        g_progress->setCancelable(false);
        g_progress->open();
    });
}

void progressUpdate(const std::string& text, int pct)
{
    std::string line = text;

    /* The percentage is appended as a number rather than as a sentence, because it
     * is the same number in every language and a template for it would be a
     * translation with nothing to translate. */
    if (pct >= 0)
    {
        char suffix[16];

        (void)std::snprintf(suffix, sizeof(suffix), "  %d%%", pct);
        line += suffix;
    }

    brls::sync([line]() {
        if (g_progressLabel != nullptr)
            g_progressLabel->setText(line);
    });
}

void progressClose()
{
    std::promise<void> done;
    auto waited = done.get_future();

    /* Waited on rather than fired and forgotten: what follows a close is a result
     * dialog, and pushing that before this one has popped would leave the progress
     * spinner on top of the answer. */
    brls::sync([&done]() {
        if (g_progress == nullptr)
        {
            done.set_value();
            return;
        }

        brls::Dialog* dialog = g_progress;

        g_progress      = nullptr;
        g_progressLabel = nullptr;
        dialog->close([&done]() { done.set_value(); });
    });

    /* Bounded, because the callback is not guaranteed. borealis will not pop the
     * bottom activity, and a dialog that finds itself there closes without telling
     * anybody - which would leave this thread waiting forever with the spinner still
     * on screen and no way to reach anything.
     *
     * The wait is only about the order the next dialog appears in, so giving up on it
     * costs a cosmetic overlap. It is never used to decide anything about save data,
     * which is why a timeout is acceptable here and is not acceptable in
     * askBlocking: an unanswered question has to stay unanswered. */
    (void)waited.wait_for(std::chrono::seconds(3));
}

/* --------------------------------------------------------------------- asking */

int askBlocking(const std::string& body, const std::vector<std::string>& options,
                int cancelResult)
{
    std::promise<int> answer;
    auto waited = answer.get_future();

    /* The promise lives on this thread's stack and this thread blocks below until it
     * is set, so it outlives every callback that captures it. */
    brls::sync([&answer, body, options, cancelResult]() {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24.0f, 30.0f, 20.0f, 30.0f);

        auto* question = new brls::Label();
        question->setText(body);
        question->setFontSize(21.0f);
        question->setLineHeight(1.3f);
        question->setMarginBottom(24.0f);
        content->addView(question);

        auto* dialog = new brls::Dialog(content);

        /* Buttons in a box of their own rather than the dialog's own three slots: a
         * conflict offers keep local, keep server and keep both, and adding a way
         * out of it would have been a fourth. */
        for (size_t i = 0; i < options.size(); ++i)
        {
            auto* button = new brls::Button();

            /* All the same weight on purpose. A primary looking "yes" beside a
             * plain "no" is a nudge, and every question asked through here is one
             * where the answer is the user's rather than this application's. */
            button->setStyle(&brls::BUTTONSTYLE_BORDERED);
            button->setText(options[i]);
            button->setFontSize(20.0f);
            button->setMarginBottom(10.0f);
            button->registerClickAction([dialog, &answer, i](brls::View*) {
                dialog->close([&answer, i]() { answer.set_value((int)i); });
                return true;
            });
            content->addView(button);
        }

        /* B is answered here rather than by the dialog's own cancel, because the
         * caller needs to be told what it meant - "no" to a confirmation, and a
         * negative to a choice, which are not the same answer. */
        dialog->setCancelable(false);
        if (cancelResult != ASK_NO_CANCEL)
        {
            content->registerAction(
                str(DAEMOON_STR_BTN_CANCEL), brls::ControllerButton::BUTTON_B,
                [dialog, &answer, cancelResult](brls::View*) {
                    dialog->close([&answer, cancelResult]() { answer.set_value(cancelResult); });
                    return true;
                });
        }

        dialog->open();
    });

    return waited.get();
}

/* ------------------------------------------------------------------ reporting */

/* One place where an outcome becomes a sentence.
 *
 * The wire code goes on the end in brackets. It is an identifier rather than prose -
 * the same kind of thing as a title id - and it is what a photograph in a bug report
 * has to carry, because "동기화 실패" narrows nothing on its own. */
static void report(daemoon_str_id_t op, daemoon_result_t r)
{
    if (r == DAEMOON_OK)
    {
        brls::Application::notify(strf(DAEMOON_STR_REPORT_OK, { str(op) }));
        return;
    }

    std::string body = strf(DAEMOON_STR_REPORT_FAILED,
                            { str(op), str(daemoon_result_str_id(r)) });

    body += "  [";
    body += daemoon_result_code(r);
    body += "]";

    (void)askBlocking(body, { str(DAEMOON_STR_BTN_OK) }, ASK_NO_CANCEL);
}

/* --------------------------------------------------------------- the operation */

static void start(std::function<void()> body)
{
    bool was = g_running.exchange(true);

    if (was)
        return;

    std::thread([body]() {
        g_workerId = std::this_thread::get_id();
        body();

        brls::sync([]() {
            g_running.store(false);
            if (g_titlesChanged)
                g_titlesChanged();
        });
    }).detach();
}

static bool confirmTitle(daemoon_str_id_t question, const daemoon_title_t& title)
{
    daemoon_str_ref_t ask {};

    ask.id      = question;
    ask.args[0] = title.name;
    ask.nargs   = 1;

    return app().env.ui->confirm(app().env.ui_ctx, &ask) != 0;
}

void runBackup(size_t index)
{
    if (index >= app().count)
        return;

    start([index]() {
        const daemoon_title_t& title = app().titles[index];
        char path[DAEMOON_PATH_MAX * 2];

        if (!confirmTitle(DAEMOON_STR_CONFIRM_BACKUP, title))
            return;

        daemoon_nx_trace("backup/begin", title.id);
        progressOpen(DAEMOON_STR_OP_BACKUP);

        daemoon_result_t r = daemoon_sync_backup_local(&app().env, &app().archive,
                                                       &app().titles[index], path,
                                                       sizeof(path));
        progressClose();
        report(DAEMOON_STR_OP_BACKUP, r);
    });
}

void runSync(size_t index)
{
    if (index >= app().count)
        return;

    /* Said here, on the main thread, rather than by starting an operation that has
     * nowhere to go. askBlocking would be the wrong tool: it waits for the main
     * thread to draw, and this is the main thread. */
    if (!app().canSync())
    {
        brls::Application::notify(str(DAEMOON_STR_ERR_NO_SERVER));
        return;
    }

    start([index]() {
        daemoon_sync_stats_t stats {};

        daemoon_nx_trace("sync/begin", app().titles[index].id);
        progressOpen(DAEMOON_STR_OP_SYNC);

        daemoon_result_t r = daemoon_sync_title(&app().env, &app().archive,
                                                &app().titles[index], &stats);
        daemoon_nx_trace("sync/done", daemoon_result_code(r));
        progressClose();

        if (r != DAEMOON_OK)
        {
            report(DAEMOON_STR_OP_SYNC, r);
            return;
        }

        /* What the sync actually did, in four numbers. A sync that says only "done"
         * is one nobody can tell apart from a sync that found nothing to do. */
        char counts[4][12];
        const unsigned n[4] = { stats.uploaded, stats.downloaded, stats.skipped,
                                stats.conflicts };
        std::vector<std::string> args;

        for (size_t i = 0; i < 4; ++i)
        {
            (void)std::snprintf(counts[i], sizeof(counts[i]), "%u", n[i]);
            args.push_back(counts[i]);
        }
        (void)askBlocking(strf(DAEMOON_STR_SYNC_RESULT, args),
                          { str(DAEMOON_STR_BTN_OK) }, ASK_NO_CANCEL);
    });
}

/* The contract, on hardware, against the title the cursor is on.
 *
 * It clears that save to prove that clearing works, so it asks first and it says so
 * in the question. Twice, because the first question is the one somebody reads and
 * the second is the one they mean.
 *
 * Every way out of this is recorded. Three runs in a row once left `list/done`
 * followed straight by `app/exit`, which is what a declined confirmation looks like
 * and also what never reaching it at all looks like - two different problems the
 * file could not tell apart.
 */
void runSelfTest(size_t index)
{
    if (index >= app().count)
        return;

    start([index]() {
        const daemoon_title_t& title = app().titles[index];

        daemoon_nx_trace("selftest/asked", title.name);

        if (!confirmTitle(DAEMOON_STR_SELFTEST_WARNING, title))
        {
            daemoon_nx_trace("selftest/declined", "first");
            return;
        }
        if (!confirmTitle(DAEMOON_STR_CONFIRM_RESTORE, title))
        {
            daemoon_nx_trace("selftest/declined", "second");
            return;
        }
        daemoon_nx_trace("selftest/run", nullptr);
        progressOpen(DAEMOON_STR_MENU_SELFTEST);

        daemoon_backend_under_test_t ut {};

        ut.name    = "nx";
        ut.backend = &daemoon_nx_save_backend;
        ut.ctx     = &app().save;
        ut.title   = &app().titles[index];
        /* No second title: a save on this platform is created by the game that owns
         * it, so the isolation cases need two dummy titles that both have saves
         * already. That is a hardware setup question rather than something to fake
         * here. */
        ut.other       = nullptr;
        ut.scratch     = app().scratch;
        ut.scratch_len = app().scratchLen;

        int before = daemoon_test_failures;

        daemoon_backend_conformance(&ut);

        char body[256];

        (void)std::snprintf(body, sizeof(body), "%d checks, %d failures. %s",
                            daemoon_test_checks, daemoon_test_failures - before,
                            daemoon_test_failures == before
                                ? "this backend behaves the way core assumes"
                                : daemoon_test_last_failure);
        daemoon_nx_trace("conformance", body);

        /* On the card as well as on screen. Nobody transcribes a failure message off
         * a television correctly, and this one names the caller in core that would
         * break. */
        daemoon_stream_t* out = nullptr;

        if (app().env.fs->open(app().env.fs_ctx, DAEMOON_NX_WORK_DIR "/conformance.txt",
                               DAEMOON_OPEN_WRITE, &out) == DAEMOON_OK)
        {
            (void)daemoon_stream_write(out, body, std::strlen(body));
            (void)daemoon_stream_write(out, "\n", 1);
            (void)daemoon_stream_close(out);
        }

        progressClose();
        (void)askBlocking(std::string(body), { str(DAEMOON_STR_BTN_OK) }, ASK_NO_CANCEL);
    });
}

} // namespace dm
